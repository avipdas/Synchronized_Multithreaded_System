#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#define BUFFER_SIZE       4096
#define MAX_URI_LENGTH    64
#define MAX_HEADER_LENGTH 2048
#define HASH_SIZE         64

typedef struct {
    int client_fd;
    char method[8];
    char uri[MAX_URI_LENGTH];
    char request_id[256];
    int content_length;
    char *body;
    struct sockaddr_in client_addr;
} request_t;

typedef struct queue_node {
    int *client_fd;
    struct queue_node *next;
} queue_node_t;

typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
    pthread_mutex_t mutex;
    sem_t items;
    int size;
    int shutdown; 
} queue_t;

typedef struct file_lock_entry {
    char filepath[MAX_URI_LENGTH + 2];
    pthread_rwlock_t rwlock;
    int ref_count;
    struct file_lock_entry *next;
} file_lock_entry_t;

typedef struct {
    file_lock_entry_t *buckets[HASH_SIZE];
    pthread_mutex_t table_mutex;
} file_lock_table_t;

static queue_t *request_queue;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static file_lock_table_t file_locks;
static int num_threads = 4;
static int server_fd;
static volatile int running = 1;
static pthread_t *workers;

void queue_init(queue_t *q);
void queue_push(queue_t *q, int *client_fd);
int *queue_pop(queue_t *q);
void queue_destroy(queue_t *q);
void queue_shutdown(queue_t *q);
void init_file_locks(void);
pthread_rwlock_t *get_file_lock(const char *filepath);
void release_file_lock(const char *filepath);
void *worker_thread(void *arg);
void process_request(request_t *req);
void handle_get_request(request_t *req);
void handle_put_request(request_t *req);
void send_response(
    int fd, int status_code, const char *status_text, const char *content, int content_length);
void log_request(const char *method, const char *uri, int status_code, const char *request_id);
int parse_request(int client_fd, request_t *req);
void cleanup_request(request_t *req);
void signal_handler(int sig);
void cleanup_server(void);

static unsigned int hash_filepath(const char *filepath) {
    unsigned int hash = 0;
    for (int i = 0; filepath[i]; i++) {
        hash = hash * 31 + filepath[i];
    }
    return hash % HASH_SIZE;
}

void init_file_locks(void) {
    memset(&file_locks, 0, sizeof(file_locks));
    pthread_mutex_init(&file_locks.table_mutex, NULL);
}

pthread_rwlock_t *get_file_lock(const char *filepath) {
    unsigned int bucket = hash_filepath(filepath);

    pthread_mutex_lock(&file_locks.table_mutex);

    file_lock_entry_t *entry = file_locks.buckets[bucket];
    while (entry) {
        if (strcmp(entry->filepath, filepath) == 0) {
            entry->ref_count++;
            pthread_mutex_unlock(&file_locks.table_mutex);
            return &entry->rwlock;
        }
        entry = entry->next;
    }

    entry = malloc(sizeof(file_lock_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&file_locks.table_mutex);
        return NULL;
    }

    strncpy(entry->filepath, filepath, sizeof(entry->filepath) - 1);
    entry->filepath[sizeof(entry->filepath) - 1] = '\0';
    pthread_rwlock_init(&entry->rwlock, NULL);
    entry->ref_count = 1;
    entry->next = file_locks.buckets[bucket];
    file_locks.buckets[bucket] = entry;

    pthread_mutex_unlock(&file_locks.table_mutex);
    return &entry->rwlock;
}

void release_file_lock(const char *filepath) {
    unsigned int bucket = hash_filepath(filepath);

    pthread_mutex_lock(&file_locks.table_mutex);

    file_lock_entry_t *entry = file_locks.buckets[bucket];
    file_lock_entry_t *prev = NULL;

    while (entry) {
        if (strcmp(entry->filepath, filepath) == 0) {
            entry->ref_count--;
            if (entry->ref_count == 0) {
                if (prev) {
                    prev->next = entry->next;
                } else {
                    file_locks.buckets[bucket] = entry->next;
                }
                pthread_rwlock_destroy(&entry->rwlock);
                free(entry);
            }
            break;
        }
        prev = entry;
        entry = entry->next;
    }

    pthread_mutex_unlock(&file_locks.table_mutex);
}

void queue_init(queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->mutex, NULL);
    sem_init(&q->items, 0, 0);
}

void queue_push(queue_t *q, int *client_fd) {
    queue_node_t *node = malloc(sizeof(queue_node_t));
    if (!node) {
        perror("malloc");
        if (client_fd) {
            close(*client_fd);
            free(client_fd);
        }
        return;
    }

    node->client_fd = client_fd;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        if (client_fd) {
            close(*client_fd);
            free(client_fd);
        }
        free(node);
        return;
    }

    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->size++;

    pthread_mutex_unlock(&q->mutex);
    sem_post(&q->items);
}

int *queue_pop(queue_t *q) {
    sem_wait(&q->items);

    pthread_mutex_lock(&q->mutex);

    if (q->shutdown || !q->head) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    queue_node_t *node = q->head;
    int *client_fd = node->client_fd;

    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->size--;

    pthread_mutex_unlock(&q->mutex);

    free(node);
    return client_fd;
}

void queue_shutdown(queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    pthread_mutex_unlock(&q->mutex);

    for (int i = 0; i < num_threads; i++) {
        sem_post(&q->items);
    }
}

void queue_destroy(queue_t *q) {
    pthread_mutex_lock(&q->mutex);

    while (q->head) {
        queue_node_t *node = q->head;
        q->head = node->next;
        if (node->client_fd) {
            close(*node->client_fd);
            free(node->client_fd);
        }
        free(node);
    }

    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    sem_destroy(&q->items);
}

void *worker_thread(void *arg) {
    (void) arg;

    while (1) {
        int *pfd = queue_pop(request_queue);
        if (!pfd) {
            break;
        }

        int fd = *pfd;
        free(pfd);

        request_t req;
        memset(&req, 0, sizeof(req));

        if (parse_request(fd, &req) == -1) {
            close(fd);
            continue;
        }

        process_request(&req);
        cleanup_request(&req);
        close(fd);
    }

    return NULL;
}

void process_request(request_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        handle_get_request(req);
    } else if (strcmp(req->method, "PUT") == 0) {
        handle_put_request(req);
    } else {
        send_response(req->client_fd, 400, "Bad Request", "Bad Request\n", 12);
        log_request(req->method, req->uri, 400, req->request_id);
    }
}

void handle_get_request(request_t *req) {
    char filepath[MAX_URI_LENGTH + 2];
    snprintf(filepath, sizeof(filepath), ".%s", req->uri);

    pthread_rwlock_t *file_lock = get_file_lock(filepath);
    if (!file_lock) {
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("GET", req->uri, 500, req->request_id);
        return;
    }

    pthread_rwlock_rdlock(file_lock);

    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        pthread_rwlock_unlock(file_lock);
        release_file_lock(filepath);
        send_response(req->client_fd, 404, "Not Found", "Not Found\n", 10);
        log_request("GET", req->uri, 404, req->request_id);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        pthread_rwlock_unlock(file_lock);
        release_file_lock(filepath);
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("GET", req->uri, 500, req->request_id);
        return;
    }

    char *content = malloc(st.st_size);
    if (!content) {
        close(fd);
        pthread_rwlock_unlock(file_lock);
        release_file_lock(filepath);
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("GET", req->uri, 500, req->request_id);
        return;
    }

    ssize_t bytes_read = 0;
    ssize_t total_read = 0;
    while (total_read < st.st_size) {
        bytes_read = read(fd, content + total_read, st.st_size - total_read);
        if (bytes_read <= 0) {
            if (bytes_read < 0 && errno == EINTR) {
                continue; 
            }
            break;
        }
        total_read += bytes_read;
    }

    close(fd);
    pthread_rwlock_unlock(file_lock);
    release_file_lock(filepath);

    if (total_read != st.st_size) {
        free(content);
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("GET", req->uri, 500, req->request_id);
        return;
    }

    send_response(req->client_fd, 200, "OK", content, st.st_size);
    log_request("GET", req->uri, 200, req->request_id);

    free(content);
}

void handle_put_request(request_t *req) {
    char filepath[MAX_URI_LENGTH + 2];
    char temp_filepath[MAX_URI_LENGTH + 32];
    snprintf(filepath, sizeof(filepath), ".%s", req->uri);

    snprintf(temp_filepath, sizeof(temp_filepath), "%s.tmp.%d.%lu", filepath, getpid(),
        (unsigned long) pthread_self());

    int temp_fd = open(temp_filepath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (temp_fd == -1) {
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("PUT", req->uri, 500, req->request_id);
        return;
    }

    if (req->body && req->content_length > 0) {
        ssize_t bytes_written = 0;
        ssize_t total_written = 0;
        while (total_written < req->content_length) {
            bytes_written
                = write(temp_fd, req->body + total_written, req->content_length - total_written);
            if (bytes_written <= 0) {
                if (bytes_written < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            total_written += bytes_written;
        }

        if (total_written != req->content_length) {
            close(temp_fd);
            unlink(temp_filepath);
            send_response(
                req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
            log_request("PUT", req->uri, 500, req->request_id);
            return;
        }
    }

    close(temp_fd);

    pthread_rwlock_t *file_lock = get_file_lock(filepath);
    if (!file_lock) {
        unlink(temp_filepath);
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("PUT", req->uri, 500, req->request_id);
        return;
    }

    pthread_rwlock_wrlock(file_lock);

    int file_exists = (access(filepath, F_OK) == 0);

    if (rename(temp_filepath, filepath) == -1) {
        pthread_rwlock_unlock(file_lock);
        release_file_lock(filepath);
        unlink(temp_filepath);
        send_response(req->client_fd, 500, "Internal Server Error", "Internal Server Error\n", 22);
        log_request("PUT", req->uri, 500, req->request_id);
        return;
    }

    pthread_rwlock_unlock(file_lock);
    release_file_lock(filepath);

    if (file_exists) {
        send_response(req->client_fd, 200, "OK", "OK\n", 3);
        log_request("PUT", req->uri, 200, req->request_id);
    } else {
        send_response(req->client_fd, 201, "Created", "Created\n", 8);
        log_request("PUT", req->uri, 201, req->request_id);
    }
}

void send_response(
    int fd, int status_code, const char *status_text, const char *content, int content_length) {
    char response[BUFFER_SIZE];
    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n\r\n", status_code, status_text, content_length);

    if (response_len >= (int) sizeof(response)) {
        return; 
    }

    ssize_t bytes_sent = 0;
    ssize_t total_sent = 0;
    while (total_sent < response_len) {
        bytes_sent = send(fd, response + total_sent, response_len - total_sent, MSG_NOSIGNAL);
        if (bytes_sent <= 0) {
            if (bytes_sent < 0 && errno == EINTR) {
                continue; 
            }
            return;
        }
        total_sent += bytes_sent;
    }

    if (content && content_length > 0) {
        total_sent = 0;
        while (total_sent < content_length) {
            bytes_sent = send(fd, content + total_sent, content_length - total_sent, MSG_NOSIGNAL);
            if (bytes_sent <= 0) {
                if (bytes_sent < 0 && errno == EINTR) {
                    continue; 
                }
                return;
            }
            total_sent += bytes_sent;
        }
    }
}

void log_request(const char *method, const char *uri, int status_code, const char *request_id) {
    pthread_mutex_lock(&log_mutex);
    fprintf(stderr, "%s,%s,%d,%s\n", method, uri, status_code, request_id);
    fflush(stderr);
    pthread_mutex_unlock(&log_mutex);
}

int parse_request(int client_fd, request_t *req) {
    char buffer[BUFFER_SIZE * 2];
    int total_received = 0;
    int header_end_pos = -1;

    memset(req, 0, sizeof(request_t));
    req->client_fd = client_fd;
    strcpy(req->request_id, "0");
    req->content_length = 0;
    req->body = NULL;

    while (total_received < (int) sizeof(buffer) - 1) {
        int n = recv(client_fd, buffer + total_received, sizeof(buffer) - 1 - total_received, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue; 
            }
            return -1;
        }

        total_received += n;
        buffer[total_received] = '\0';

        char *header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) {
            header_end_pos = header_end - buffer + 4;
            break;
        }
    }

    if (header_end_pos == -1) {
        return -1;
    }

    buffer[header_end_pos - 4] = '\0';

    char *lines = strdup(buffer);
    if (!lines)
        return -1;

    char *saveptr;
    char *request_line = strtok_r(lines, "\r\n", &saveptr);
    if (!request_line) {
        free(lines);
        return -1;
    }

    char *method = strtok(request_line, " ");
    char *uri = strtok(NULL, " ");
    char *version = strtok(NULL, " ");

    if (!method || !uri || !version) {
        free(lines);
        return -1;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "PUT") != 0) {
        free(lines);
        return -1;
    }

    strncpy(req->method, method, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';

    if (strlen(uri) >= MAX_URI_LENGTH || uri[0] != '/') {
        free(lines);
        return -1;
    }

    for (int i = 1; uri[i]; i++) {
        char c = uri[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.'
                || c == '-')) {
            free(lines);
            return -1;
        }
    }

    strncpy(req->uri, uri, sizeof(req->uri) - 1);
    req->uri[sizeof(req->uri) - 1] = '\0';

    char *line;
    while ((line = strtok_r(NULL, "\r\n", &saveptr))) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *value = line + 15;
            while (*value == ' ')
                value++;
            req->content_length = atoi(value);
            if (req->content_length < 0) {
                req->content_length = 0;
            }
        } else if (strncasecmp(line, "Request-Id:", 11) == 0) {
            char *value = line + 11;
            while (*value == ' ')
                value++;
            strncpy(req->request_id, value, sizeof(req->request_id) - 1);
            req->request_id[sizeof(req->request_id) - 1] = '\0';
        }
    }

    free(lines);

    if (req->content_length > 0) {
        int body_in_buffer = total_received - header_end_pos;

        req->body = malloc(req->content_length);
        if (!req->body)
            return -1;

        if (body_in_buffer > 0) {
            int copy_len
                = (body_in_buffer > req->content_length) ? req->content_length : body_in_buffer;
            memcpy(req->body, buffer + header_end_pos, copy_len);
        }

            int body_received
            = (body_in_buffer > req->content_length) ? req->content_length : body_in_buffer;
        while (body_received < req->content_length) {
            int n = recv(
                client_fd, req->body + body_received, req->content_length - body_received, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue; 
                }
                free(req->body);
                req->body = NULL;
                return -1;
            }
            body_received += n;
        }
    }

    return 0;
}

void cleanup_request(request_t *req) {
    if (req->body) {
        free(req->body);
        req->body = NULL;
    }
}

void signal_handler(int sig) {
    (void) sig;
    running = 0;

    if (request_queue) {
        queue_shutdown(request_queue);
    }

    if (workers) {
        for (int i = 0; i < num_threads; i++) {
            pthread_join(workers[i], NULL);
        }
        free(workers);
        workers = NULL;
    }

    cleanup_server();
    exit(0);
}

void cleanup_server(void) {
    if (request_queue) {
        queue_destroy(request_queue);
        free(request_queue);
        request_queue = NULL;
    }
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    for (int i = 0; i < HASH_SIZE; i++) {
        file_lock_entry_t *entry = file_locks.buckets[i];
        while (entry) {
            file_lock_entry_t *next = entry->next;
            pthread_rwlock_destroy(&entry->rwlock);
            free(entry);
            entry = next;
        }
    }

    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&file_locks.table_mutex);
}

int main(int argc, char *argv[]) {
    int port = 0;
    int i = 1;

    while (i < argc) {
        if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
                return 1;
            }
            num_threads = atoi(argv[i + 1]);
            if (num_threads <= 0) {
                fprintf(stderr, "Invalid number of threads\n");
                return 1;
            }
            i += 2;
        } else {
            port = atoi(argv[i]);
            i++;
        }
    }

    if (port == 0) {
        fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
        return 1;
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    init_file_locks();

    request_queue = malloc(sizeof(queue_t));
    if (!request_queue) {
        perror("malloc");
        return 1;
    }
    queue_init(request_queue);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        cleanup_server();
        return 1;
    }

    int opt_val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)) == -1) {
        perror("setsockopt");
        cleanup_server();
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        cleanup_server();
        return 1;
    }

    if (listen(server_fd, 128) == -1) {
        perror("listen");
        cleanup_server();
        return 1;
    }

    workers = malloc(num_threads * sizeof(pthread_t));
    if (!workers) {
        perror("malloc");
        cleanup_server();
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            cleanup_server();
            free(workers);
            return 1;
        }
    }

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_len);
        if (client_fd == -1) {
            if (running && errno != EINTR) {
                perror("accept");
            }
            continue;
        }

        int *pfd = malloc(sizeof(int));
        if (!pfd) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        *pfd = client_fd;
        queue_push(request_queue, pfd);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(workers[i], NULL);
    }

    free(workers);
    cleanup_server();
    return 0;
}
