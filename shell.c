#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {

	if (argc > 1) {
		if (strcmp(argv[1], "-c") == 0) {
			if (argc < 3) {
				fprintf(stderr, "Usage: %s -c <command>\n", argv[0]);
				return 1;
			}
			printf("Command: %s\n", argv[2]);
			return 0;
		} else {
			char *file = strdup(argv[1]);
			printf("File: %s\n", file);
			free(file);
			return 0;
		}
	}
	char *buffer = NULL;
	size_t len = 0;
	ssize_t nread = 0;
	while (1) {
		printf("mysh> ");
		fflush(stdout);
		nread = getline(&buffer, &len, stdin);
		if (nread == -1) {
			if (feof(stdin)) {
				printf("exit\n");
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				perror("getline");
				break;
			}
		}
		buffer[strcspn(buffer, "\n")] = 0;

		printf("%s\n", buffer);
	}
	free(buffer);
	return 0;
}