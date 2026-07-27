#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>


static void restore_stdio(int backup_in_fd, int backup_out_fd) {
    if (backup_in_fd != 0) {
        close(0);
        dup2(backup_in_fd, 0);
        close(backup_in_fd);
    }
    if (backup_out_fd != 1) {
        close(1);
        dup2(backup_out_fd, 1);
        close(backup_out_fd);
    }
}


static int handle_redirs(Redirector* redirs, int* backup_in_fd, int* backup_out_fd) {
    if (redirs != NULL) {
        for (size_t i = 0; redirs[i].type != TOKEN_EOF; i++) {
            TokenType type = redirs[i].type;
            if (type == TOKEN_REDIRECT_IN) {
                int fd = open(redirs[i].target, O_RDONLY);
                if (fd == -1) {
                    perror("open");
                    if (backup_in_fd != NULL) restore_stdio(*backup_in_fd, *backup_out_fd);
                    return -1;
                }
                if (backup_in_fd != NULL && *backup_in_fd == 0) *backup_in_fd = dup(0);
                dup2(fd, 0);
                close(fd);
            } else if (type == TOKEN_REDIRECT_OUT) {
                int fd = open(redirs[i].target, O_WRONLY | O_TRUNC | O_CREAT, 0644);
                if (fd == -1) {
                    perror("open");
                    if (backup_out_fd != NULL) restore_stdio(*backup_in_fd, *backup_out_fd);
                    return -1;
                }
                if (backup_out_fd != NULL && *backup_out_fd == 1) *backup_out_fd = dup(1);
                dup2(fd, 1);
                close(fd);
            } else if (type == TOKEN_REDIRECT_APPEND) {
                int fd = open(redirs[i].target, O_WRONLY | O_APPEND | O_CREAT, 0644);
                if (fd == -1) {
                    perror("open");
                    if (backup_out_fd != NULL) restore_stdio(*backup_in_fd, *backup_out_fd);
                    return -1;
                }
                if (backup_out_fd != NULL && *backup_out_fd == 1) *backup_out_fd = dup(1);
                dup2(fd, 1);
                close(fd);
            }
        }
    }
    return 0;
}


static void handle_pipes(int last_fds[2], int current_fds[2], int* backup_in_fd, int* backup_out_fd) {
    if (current_fds[0] != -1) {
        if (backup_out_fd == NULL) {
            close(current_fds[0]);
        } else if (*backup_out_fd == 1) {
            *backup_out_fd = dup(1);
        }
        dup2(current_fds[1], 1);
        close(current_fds[1]);
    }
    if (last_fds[1] != -1) {
        if (backup_in_fd == NULL) {
            close(last_fds[1]);
        } else if (*backup_in_fd == 0) {
            *backup_in_fd = dup(0);
        }
        dup2(last_fds[0], 0);
        close(last_fds[0]);
    }
}


static int execute_commands(Commands* commands, const char* commands_string) {
    Job* job = NULL;
    int ret_code = 0;

    char* fn_name = (commands->command->argv == NULL) ? NULL : commands->command->argv[0];
    builtin_fn fn = lookup_builtin(fn_name);
    // 只有独立前台 builtin 才会在父 shell 中直接运行。
    if (fn && !commands->pipeline && !commands->background) {
        int backup_in_fd = 0, backup_out_fd = 1;
        int handle_ret = handle_redirs(commands->command->redirs, &backup_in_fd, &backup_out_fd);
        if (handle_ret == -1) return 1;
        int fn_ret = fn(commands->command->argv);
        fflush(stdout);
        restore_stdio(backup_in_fd, backup_out_fd);
        return fn_ret;
    }

    // 否则都去 fork。因为管道要求并行, 而后台要求异步, 这两种情况下都不能直接在父 shell 中执行。
    // 并都要创建 Job 结构体, 因为任务都可能被随时放入后台。
    job = create_job(commands->background, commands_string);
    if (job == NULL) return 1;
    // 为前台交互式 job 创建阻塞管道, 以便在父 shell 创建完所有任务并将前台控制交出后子任务再执行。
    int launch_gate[2] = {-1, -1};
    if (shell.interactive && !commands->background) {
        if (pipe(launch_gate) == -1) {
            free_job(job);
            return 1;
        }
    }
    int last_fds[2] = {-1, -1}, current_fds[2] = {-1, -1};
    Command* command = commands->command;
    while (command != NULL) {
        if (command->next != NULL) {
            int pipe_ret = pipe(current_fds);
            if (pipe_ret == -1) goto failed;
        } else {
            current_fds[0] = -1;
            current_fds[1] = -1;
        }
        
        pid_t pid = fork();
        // fork 失败
        if (pid < 0) {
            perror("fork");
            goto failed;
        }
        // 子进程
        if (pid == 0) {
            int restore_ret = child_restore_signals();
            if (restore_ret == -1) {
                perror("child_restore_signals");
                _exit(1);
            }
            // 父进程仅在子进程 exec 前拥有替它安排进程组的临时权限；exec 成功后，这项权限失效。
            // 为避免竞态，需要在父子进程都调用setpgid。
            if (setpgid(0, job->pgid) == -1) {
                perror("setpgid");
                _exit(1);
            }
            //  POSIX 标准（Shell & Utilities 卷，第 2.9.2 节“管道”）。标准明确规定：
            // "The standard output of command1 shall be connected to the standard input of command2 ... 
            // This redirection shall be performed before any redirections specified by the command itself."
            // Bash 参考手册在“Pipelines”一节中也明确指出：
            // "... This connection is performed before any redirections specified by command1."
            // 因此管道应该在重定向前被解析, 以便能被重定向覆盖。
            handle_pipes(last_fds, current_fds, NULL, NULL);
            int handle_ret = handle_redirs(command->redirs, NULL, NULL);
            if (handle_ret == -1) _exit(1);

            if (shell.interactive && !commands->background) {
                close(launch_gate[1]);
                char buf;
                while (read(launch_gate[0], &buf, 1) == -1) {
                    if (errno == EINTR) {
                        continue;
                    } else {
                        perror("launch gate");
                        _exit(1);
                    }
                }
                close(launch_gate[0]);
            }

            char* fn_name = (command->argv == NULL) ? NULL : command->argv[0];
            builtin_fn fn = lookup_builtin(fn_name);
            if (fn) {
                int fn_ret = fn(command->argv);
                fflush(stdout);
                _exit(fn_ret);
            } else {
                execvp(fn_name, command->argv);
                perror("execvp");
                _exit(127);
            }
        }
        // 父进程
        if (add_process(job, pid) == -1) {
            kill_wait_pid(pid);
            goto failed;
        }
        if (setpgid(pid, job->pgid) == -1) {
            // EACCES: 子进程已经exec, 无法访问; ESRCH: 子进程无法找到, 已经消失
            // 这两种错误可接受, 其余报错
            if (errno != EACCES && errno != ESRCH) {
                perror("setpgid");
                kill_wait_pid(pid);
                goto failed;
            }
        }
        if (current_fds[0] != -1) close(current_fds[1]);
        if (last_fds[1] != -1) close(last_fds[0]);
        last_fds[0] = current_fds[0];
        last_fds[1] = current_fds[1];
        command = command->next;
    }

    if (commands->background) {
        if (job_add(job) == -1) {
            perror("job_add");
            goto failed;
        }
        ret_code = 0;
    } else {
        if (shell.interactive) {
            if (give_terminal(job) == -1) goto failed;
            close(launch_gate[0]);
            close(launch_gate[1]);
        }

        ret_code = wait_foreground_job(job);

        if (shell.interactive) {
            if (take_terminal(job) == -1) goto failed;
        }

        if (job->status == STOPPED) {
            job->background = 1;
            if (job_add(job) == -1) {
                perror("job_add");
                goto failed;
            }
        } else {
            free_job(job);
        }
    }

    return ret_code;

failed:
    close(launch_gate[0]);
    close(launch_gate[1]);
    close(last_fds[0]);
    close(last_fds[1]);
    close(current_fds[0]);
    close(current_fds[1]);

    kill_wait_job(job);
    free_job(job);
    return -1;
}


int execute_line(const char* line) {
    TokenArray* token_array = tokenize(line);
    if (token_array == NULL) {
        return 1;
    }

    int expander_ret = expand_token(token_array);
    if (expander_ret == -1) {
        free_tokens(token_array);
        return 1;
    }

    Commands* commands = parse_commands(token_array);
    if (commands == NULL) {
        free_tokens(token_array);
        return 1;
    }

    shell.last_status = execute_commands(commands, line);

    free_commands(commands);
    free_tokens(token_array);

    return 0;
}
