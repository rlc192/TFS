#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
void main() {
	printf("%x", sizeof(struct stat));
}
