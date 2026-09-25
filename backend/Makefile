CC      = gcc
CFLAGS  = -O2 -Wall -std=c11 -D_GNU_SOURCE $(shell pkg-config --cflags libpq jansson 2>/dev/null)
LDFLAGS = -lmicrohttpd -lpq -ljansson -lpthread

SRC = src/server.c src/db.c src/engine_runner.c
BIN = epidemic_backend

all: $(BIN)

$(BIN): $(SRC) src/db.h src/sim_params.h src/engine_runner.h
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
