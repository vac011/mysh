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
	
	char *line = NULL;
	size_t len = 0;
	while (!shell.should_exit) {
		check_jobs();

		if (shell.interactive) {
			printf("mysh> ");
			fflush(stdout);
		}

		while (getline(&line, &len, stdin)== -1) {
			if (feof(stdin)) {
				if (interactive) printf("\n");
				goto cleanup;
			} else if (errno == EINTR) {
				continue;
			} else {
				shell.last_status = 1;
				perror("getline");
				goto cleanup;
			}
		}

		// line == NULL 永远不会发生, 不应该为了“防御式编程”而进行冗余检查, 这会掩盖真正的 bug
		line[strcspn(line, "\n")] = 0;

		execute_line(line);
	}

// // goto cleanup 在 C 系统编程中是公认的工程写法, 并无不优雅之处。
cleanup:
	free(line);
	// clean the jobs
	return shell.last_status;
}