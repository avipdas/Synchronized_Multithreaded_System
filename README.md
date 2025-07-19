# 🧵 Synchronized Multithreaded HTTP Server

A high-performance, thread-safe HTTP/1.1 server in pure C that supports concurrent GET and PUT requests. Designed for speed, safety, and correctness, this server uses a synchronized work queue, thread pool, and fine-grained file-level locking.

## 🚀 Features

* 🧵 **Multithreaded**: Thread pool for concurrent request handling
* 🧺 **Synchronized Work Queue**: Producer-consumer model using mutex and semaphores
* 🔐 **File-Level Concurrency**: Read-write locks ensure safe GET/PUT access
* 📄 **HTTP/1.1 Support**: Parses headers, body, `Content-Length`, `Request-Id`
* 🛠️ **Robust Error Handling**: Handles malformed input, system errors, EINTR
* 🪵 **Thread-Safe Logging**: Logs all requests with status codes and IDs
* 🧼 **Graceful Shutdown**: Signal handlers for SIGINT/SIGTERM cleanup

## 📦 Project Structure

```
.
├── httpserver.c     # Main server source code
├── Makefile         # For building the server
├── README.md        # This file
├── test_repo.sh     # Sample test script
├── load_repo.sh     # Optional script to load test files
├── config.json      # Sample config (optional)
├── output.txt       # Sample output (optional)
├── error.txt        # Sample error log (optional)
└── workloads/       # Test files directory (optional)
```

## 🔧 Build Instructions

```bash
make
```

Or compile manually:

```bash
gcc -pthread -o httpserver httpserver.c
```

## ▶️ Usage

```bash
./httpserver -t <num_threads> <port>
```

Example:

```bash
./httpserver -t 4 8080
```

## 📬 Sample Requests

**PUT Request**

```bash
curl -X PUT -H "Request-Id: 123" --data "hello world" http://localhost:8080/hello.txt
```

**GET Request**

```bash
curl -X GET -H "Request-Id: 456" http://localhost:8080/hello.txt
```

## 🧪 Testing

Run all your test cases:

```bash
./test_repo.sh
```

## 🧹 Graceful Shutdown

The server handles `Ctrl+C` or `SIGTERM`, cleans up threads and locks, and shuts down cleanly.

## 📚 Concepts Demonstrated

* Thread pools and pthreads
* Producer-consumer queue using mutex/semaphore
* Per-file fine-grained locking (readers-writer locks)
* HTTP/1.1 protocol parsing and validation
* Memory safety and resource cleanup
* Signal handling in concurrent systems
