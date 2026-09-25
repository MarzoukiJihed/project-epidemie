CC = gcc
CFLAGS = -O3 -march=native -Wall -std=c11
LDFLAGS = -lm

all: simulate_par

simulate_par: simulate.c
	$(CC) $(CFLAGS) -fopenmp simulate.c -o simulate_par $(LDFLAGS)

clean:
	rm -f simulate_par
