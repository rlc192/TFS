CC=gcc
CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64
LDFLAGS=-lfuse

OBJ=tfs.o block.o

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

tfs: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o tfs
	mkdir -p /tmp/tmc231_rlc192
	mkdir -p /tmp/tmc231_rlc192/mountdir

.PHONY: clean
clean:
	rm -f *.o tfs 
	rm -rf /tmp/tmc231_rlc192/mountdir 
	rm -rf /tmp/tmc231_rlc192
	
