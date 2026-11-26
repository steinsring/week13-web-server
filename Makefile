CC = gcc
CFLAGS = -Wall -Wextra -g -I./sys -pthread
TARGETS = mini echoclient echoserver forward_proxy
all: $(TARGETS)

mini :sys/mini.c sys/server.c  sys/client.c proxy/reverse_proxy.c
	$(CC) $(CFLAGS) $^ -o $@
echoclient: sys/echoclient.c sys/client.c
	$(CC) $(CFLAGS) $^ -o $@
echoserver: sys/echoserver.c sys/server.c
	$(CC) $(CFLAGS) $^ -o $@
forward_proxy: proxy/forward_proxy.c sys/server.c sys/client.c
	$(CC) $(CFLAGS) $^ -o $@

test: mini forward_proxy
	python3 tests/integration.py

clean:
	rm -f $(TARGETS)
.PHONY: all clean test