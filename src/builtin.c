#include "shell.h"
#include "builtin.h"
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

static int builtin_null(char** argv);
static int builtin_echo(char** argv);
static int builtin_cd(char** argv);
static int builtin_pwd(char** argv);
static int builtin_exit(char** argv);
static int builtin_type(char** argv);
static int builtin_jobs(char** argv);
static int builtin_bg(char** argv);
static int builtin_fg(char** argv);

static const Builtin builtins[] = {
    {"echo", builtin_echo},
    {"cd", builtin_cd},
    {"pwd", builtin_pwd},
    {"exit", builtin_exit},
    {"type", builtin_type},
    {"jobs", builtin_jobs},
    {"bg", builtin_bg},
    {"fg", builtin_fg},
    {NULL, NULL}
};


builtin_fn lookup_builtin(char* func_name) {
    if (func_name == NULL) return builtin_null;
    for (size_t i = 0; builtins[i].func_name != NULL; i++) {
        if (strcmp(func_name, builtins[i].func_name) == 0) return builtins[i].fn;
    }
    return NULL;
}


static int builtin_null(char** argv) {
    (void)argv;
    return 0;
}


static int builtin_echo(char** argv) {
    for (size_t i = 1; argv[i] != NULL; i++) {
        if (i != 1) printf(" ");
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}


static int builtin_cd(char** argv) {
    char* dir = argv[1];
    if (dir == NULL) {
        dir = getenv("HOME");
        if (dir == NULL) return 1;
    }
    if (chdir(dir) == -1) {
        return 1;
    }
    return 0;
}


static int builtin_pwd(char** argv) {
    (void)argv;
    char* cwd = getcwd(NULL, 0);
    if (cwd == NULL) return 1;
    printf("%s\n", cwd);
    free(cwd);
    return 0;
}


static int builtin_exit(char** argv) {
    long exit_code;
    if (argv[1] == NULL) {
        exit_code = shell.last_status;
    } else {
        char* endptr = NULL;
        exit_code = strtol(argv[1], &endptr, 10);
        if (errno == ERANGE || endptr == argv[1] || *endptr != '\0') {
            fprintf(stderr, "Wrong exit code: %s\n", argv[1]);
            exit_code = 1;
        }
    }
    shell.should_exit = 1;
    return exit_code;
}


static int builtin_type(char** argv) {
    if (argv[1] == NULL) return 1;
    int found = 0;
    if (strcmp(argv[1], "-a") == 0) {
        char* command = argv[2];
        if (command == NULL) {
            fprintf(stderr, "TYPE needs an argument after '-a'\n");
            return 1;
        }
        if (lookup_builtin(command)) {
            printf("%s is a builtin command\n", command);
            found = 1;
        }
        // getenv返回值是借用指针，使用前需要先备份
        char* env = getenv("PATH");
        if (env == NULL) return 1;
        char* path_env = strdup(env);
        if (path_env == NULL) return 1;
        char* endptr = NULL;
        size_t command_len = strlen(command);
        char* dir = strtok_r(path_env, ":", &endptr);
        char* fullpath = NULL;
        while (dir) {
            size_t dir_len = strlen(dir);
            // +2 别忘了中间拼接的"\"
            char* new_fullpath = realloc(fullpath, command_len + dir_len + 2);
            if (new_fullpath == NULL) {
                free(fullpath);
                free(path_env);
                return 1;
            }
            fullpath = new_fullpath;
            snprintf(fullpath, command_len + dir_len + 2, "%s/%s", dir, command);
            fullpath[command_len + dir_len + 1] = '\0';
            if (access(fullpath, X_OK) == 0) {
                printf("%s is %s\n", command, fullpath);
                found = 1;
            }
            dir = strtok_r(NULL, ":", &endptr);
        }
        free(fullpath);
        free(path_env);
        if (!found) {
            printf("%s not found\n", command);
            return 1;
        }
        return 0;
    } else {
        char* command = argv[1];
        if (command == NULL) {
            fprintf(stderr, "TYPE needs an argument\n");
            return 1;
        }
        if (lookup_builtin(command)) {
            printf("%s is a builtin command\n", command);
            return 0;
        }
        char* env = getenv("PATH");
        if (env == NULL) return 1;
        char* path_env = strdup(env);
        if (path_env == NULL) return 1;
        char* endptr = NULL;
        size_t command_len = strlen(command);
        char* dir = strtok_r(path_env, ":", &endptr);
        char* fullpath = NULL;
        while (dir) {
            size_t dir_len = strlen(dir);
            char* new_fullpath = realloc(fullpath, command_len + dir_len + 2);
            if (new_fullpath == NULL) {
                free(fullpath);
                free(path_env);
                return 1;
            }
            fullpath = new_fullpath;
            snprintf(fullpath, command_len + dir_len + 2, "%s/%s", dir, command);
            fullpath[command_len + dir_len + 1] = '\0';
            if (access(fullpath, X_OK) == 0) {
                printf("%s is %s\n", command, fullpath);
                free(fullpath);
                free(path_env);
                return 0;
            }
            dir = strtok_r(NULL, ":", &endptr);
        }
        free(fullpath);
        free(path_env);
        printf("%s not found\n", command);
        return 1;
    }
}


static int builtin_jobs(char** argv) {
    (void)argv;
    check_jobs();
    Job* p = shell.jobs;
    while (p != NULL) {
        char marker = p == shell.jobs ? '+' :
                      (shell.jobs != NULL && p == shell.jobs->next ? '-' : ' ');
        const char* status = p->status == RUNNING ? "Running" :
                             (p->status == STOPPED ? "Stopped" : "Done");
        printf("[%zu]%c %-7s %s\n", p->job_id, marker, status, p->cmd);
        p = p->next;
    }
    return 0;
}


static Job* resolve_jobspec(const char* jobspec) {
    if (jobspec == NULL || strcmp(jobspec, "%") == 0 ||
        strcmp(jobspec, "%%") == 0 || strcmp(jobspec, "%+") == 0) {
        if (shell.jobs == NULL)
            fprintf(stderr, "no current job\n");
        return shell.jobs;
    }
    if (strcmp(jobspec, "%-") == 0) {
        if (shell.jobs == NULL || shell.jobs->next == NULL) {
            fprintf(stderr, "no previous job\n");
            return NULL;
        }
        return shell.jobs->next;
    }
    if (jobspec[0] != '%' || jobspec[1] == '\0') {
        fprintf(stderr, "invalid jobspec: %s\n", jobspec);
        return NULL;
    }

    errno = 0;
    char* endptr = NULL;
    unsigned long long value = strtoull(jobspec + 1, &endptr, 10);
    if (errno == ERANGE || *endptr != '\0' || value == 0 || value > SIZE_MAX) {
        fprintf(stderr, "invalid jobspec: %s\n", jobspec);
        return NULL;
    }

    Job* job = shell.jobs;
    while (job != NULL) {
        if (job->job_id == value) break;
        job = job->next;
    }
    if (job == NULL)
        fprintf(stderr, "%s: no such job\n", jobspec);
    return job;
}


static int continue_in_background(Job* job) {
    if (kill(-job->pgid, SIGCONT) == -1) {
        perror("bg");
        return 1;
    }
    job_mark_running(job);
    job->background = 1;
    job_make_current(job);
    printf("[%zu]+ Running %s\n", job->job_id, job->cmd);
    return 0;
}


static int builtin_bg(char** argv) {
    if (!shell.interactive) {
        fprintf(stderr, "bg: job control is not available\n");
        return 1;
    }

    check_jobs();
    if (argv[1] == NULL) {
        Job* job = resolve_jobspec(NULL);
        return job == NULL ? 1 : continue_in_background(job);
    }

    int result = 0;
    for (size_t i = 1; argv[i] != NULL; i++) {
        Job* job = resolve_jobspec(argv[i]);
        if (job == NULL || continue_in_background(job) != 0)
            result = 1;
    }
    return result;
}


static int builtin_fg(char** argv) {
    if (!shell.interactive) {
        fprintf(stderr, "fg: job control is not available\n");
        return 1;
    }
    if (argv[1] != NULL && argv[2] != NULL) {
        fprintf(stderr, "fg: too many arguments\n");
        return 1;
    }

    check_jobs();
    Job* job = resolve_jobspec(argv[1]);
    if (job == NULL)
        return 1;

    int was_stopped = job->status == STOPPED;
    Job** link = &shell.jobs;
    while (*link != NULL) {
        if (*link == job) {
            *link = job->next;
            job->next = NULL;
            break;
        }
        link = &(*link)->next;
    }
    job->background = 0;

    if (give_terminal(job) == -1) {
        perror("fg: give terminal");
        job->background = 1;
        job_make_current(job);
        return 1;
    }

    printf("%s\n", job->cmd);
    fflush(stdout);
    if (was_stopped) {
        if (kill(-job->pgid, SIGCONT) == -1) {
            perror("fg");
            if (take_terminal(job) == -1)
                perror("fg: take terminal");
            job->background = 1;
            job_make_current(job);
            return 1;
        }
        job_mark_running(job);
    }

    int result = wait_foreground_job(job);
    if (take_terminal(job) == -1) {
        perror("fg: take terminal");
        result = 1;
    }

    if (job->status == STOPPED || job->status == RUNNING) {
        job->background = 1;
        if (job_add(job) == -1) {
            perror("fg: job_add");
            kill_wait_job(job);
            free_job(job);
            return 1;
        }
    } else {
        free_job(job);
    }
    return result;
}
