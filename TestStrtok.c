#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void test(char *path, int ino) {
	char *token = strdup(path);
	char *rest;
	token = strtok_r(token, "/", &rest);
	/*if(ino == 0) {
		
	}
	else {
		token = strtok_r(NULL, "/", &rest);
	}*/
	printf("Token: %s. Rest: %s.\n", token, rest);
	if (token != NULL) {
		test(rest, ++ino);
	}
}
void main(int argc, char **argv) {
	char *path = argv[1];
	test(path, 0);
}
