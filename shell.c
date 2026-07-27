#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

ShellState shell;

int main(int argc, char **argv) {
	if (argc > 1) {
		if (strcmp(argv[1], "-c") == 0) {
			if (argc < 3) {
				fprintf(stderr, "Usage: %s -c <command>\n", argv[0]);
				return 1;
			}
			controller_init(0);
			execute_line(argv[2]);
			return shell.last_status;
		} else {
			int fd = open(argv[1], O_RDONLY);
			if (fd == -1) {
				perror("open");
				return 1;
			}
			dup2(fd, 0);
			close(fd);
		}
	}

	if (controller_init(isatty(STDIN_FILENO)) == -1) {
		perror("controller_init");
		return 1;
	}
	
	while (!shell.should_exit) {
		check_jobs();

		if (shell.interactive) {
			printf("mysh> ");
			fflush(stdout);
		}

		char *line = NULL;
		size_t len = 0;
		while (getline(&line, &len, stdin)== -1) {
			if (feof(stdin)) {
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				perror("getline");
				shell.last_status = 1;
				break;
			}
		}
		if (line == NULL) break;
		line[strcspn(line, "\n")] = 0;

		execute_line(line);

		free(line);
	}
	// clean the jobs
	return shell.last_status;
}