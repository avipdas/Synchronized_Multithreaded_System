CC      := clang
CFLAGS  := -Wall -Wextra -Werror -pedantic

.PHONY: all clean

all: queue.o rwlock.o

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c -o queue.o

rwlock.o: rwlock.c rwlock.h
	$(CC) $(CFLAGS) -c rwlock.c -o rwlock.o

clean:
	rm -f *.o
