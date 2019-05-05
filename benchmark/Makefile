CC = gcc
CFLAGS = -g
all: warmup mainevent

warmup:
	$(CC) $(CFLAGS) -o simple_test simple_test.c

mainevent:
	$(CC) $(CFLAGS) -o test_case test_case.c

clean:
	rm -rf simple_test test_case
