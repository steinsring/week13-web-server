CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all test sanitize clean
all: server

server: static_files.c static_files.h server.c http_request.c http_response.c request_io.c http_request.h http_response.h request_io.h
	$(CC) $(CFLAGS) -o $@ static_files.c server.c http_request.c http_response.c request_io.c

tests/test_http_request: tests/test_http_request.c http_request.c http_request.h
	$(CC) $(CFLAGS) -Drealloc=test_realloc -I. -o $@ tests/test_http_request.c http_request.c

tests/test_http_response: tests/test_http_response.c http_response.c request_io.c http_request.c http_response.h request_io.h http_request.h
	$(CC) $(CFLAGS) -Dsend=test_send -I. -o $@ tests/test_http_response.c http_response.c request_io.c http_request.c

tests/test_static_files: tests/test_static_files.c static_files.c http_response.c http_request.c static_files.h http_response.h http_request.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_static_files.c static_files.c http_response.c http_request.c

test: tests/test_http_request tests/test_http_response tests/test_static_files
	./tests/test_http_request
	./tests/test_http_response
	./tests/test_static_files

sanitize:
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Drealloc=test_realloc -I. -o tests/test_http_request_asan tests/test_http_request.c http_request.c
	ASAN_OPTIONS=detect_leaks=1 ./tests/test_http_request_asan
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Dsend=test_send -I. -o tests/test_http_response_asan tests/test_http_response.c http_response.c request_io.c http_request.c
	ASAN_OPTIONS=detect_leaks=1 ./tests/test_http_response_asan

	$(CC) -std=c11 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I. -o tests/test_static_files_asan tests/test_static_files.c static_files.c http_response.c http_request.c
	ASAN_OPTIONS=detect_leaks=1 ./tests/test_static_files_asan

clean:
	rm -f server tests/test_static_files tests/test_static_files_asan tests/test_http_request tests/test_http_request_asan tests/test_http_response tests/test_http_response_asan
