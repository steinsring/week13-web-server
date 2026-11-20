CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all test sanitize clean
all: server

server: server.c http_request.c http_response.c http_request.h http_response.h
	$(CC) $(CFLAGS) -o $@ server.c http_request.c http_response.c

tests/test_http_request: tests/test_http_request.c http_request.c http_request.h
	$(CC) $(CFLAGS) -Drealloc=test_realloc -I. -o $@ tests/test_http_request.c http_request.c

test: tests/test_http_request
	./tests/test_http_request

sanitize:
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Drealloc=test_realloc -I. -o tests/test_http_request_asan tests/test_http_request.c http_request.c
	ASAN_OPTIONS=detect_leaks=1 ./tests/test_http_request_asan

clean:
	rm -f server tests/test_http_request tests/test_http_request_asan
