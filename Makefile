CC=gcc
CFLAGS=-O2 -Wall -Wextra -ggdb

all: weytool dynbl

weytool: weytool.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lusb-1.0 -lpthread

dynbl: dynbl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lusb-1.0 -lpthread

clean:
	rm -f weytool dynbl
