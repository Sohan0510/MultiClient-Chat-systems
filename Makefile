CC=gcc
CFLAGS=-Wall -g
SRC_DIR=src

all: server client admin_client filter

server: $(SRC_DIR)/server.c
	$(CC) $(CFLAGS) -o server $(SRC_DIR)/server.c

client: $(SRC_DIR)/client.c
	$(CC) $(CFLAGS) -o client $(SRC_DIR)/client.c

admin_client: $(SRC_DIR)/admin_client.c
	$(CC) $(CFLAGS) -o admin_client $(SRC_DIR)/admin_client.c

filter: $(SRC_DIR)/filter.c
	$(CC) $(CFLAGS) -o filter $(SRC_DIR)/filter.c

clean:
	rm -f server client admin_client filter

.PHONY: all clean
