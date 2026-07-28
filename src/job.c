#include "job.h"
#include "shell.h"
#include "stdio.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

void free_job(Job* job) {
    if (job == NULL) return;
    free(job->cmd);
    Process* p = job->processes;
    while (p != NULL) {
        Process* q = p->next;
        free(p);
        p = q;
    }
    free(job);
}


Job* create_job(int background, const char* commands_string) {
    Job* job = malloc(sizeof(Job));
    if (job == NULL) return NULL;
    job->status = RUNNING;
    job->terminal_models = shell.terminal_modes;
    job->background = background;
    job->cmd = strdup(commands_string);
    if (job->cmd == NULL) {
        free(job);
        return NULL;
    }
    job->processes = NULL;
    job->next = NULL;
    job->job_id = 0;  // Assigned only if the job enters shell.jobs.
    job->pgid = 0;  // pgid will be set after the first process is forked

    return job; 
}


void job_make_current(Job* job) {
    if (job == NULL || shell.jobs == job)
        return;
    Job** link = &shell.jobs;
    while (*link != NULL) {
        if (*link == job) {
            *link = job->next;
            job->next = NULL;
            break;
        }
        link = &(*link)->next;
    }
    job->next = shell.jobs;
    shell.jobs = job;
}


static int allocate_job_id(size_t* result) {
    size_t job_count = 0;
    for (Job* job = shell.jobs; job != NULL; job = job->next) {
        if (job_count == SIZE_MAX - 1) {
            errno = EOVERFLOW;
            return -1;
        }
        job_count++;
    }

    unsigned char* used = calloc(job_count + 2, sizeof(*used));
    if (used == NULL)
        return -1;

    for (Job* job = shell.jobs; job != NULL; job = job->next) {
        if (job->job_id > 0 && job->job_id <= job_count + 1)
            used[job->job_id] = 1;
    }

    for (size_t job_id = 1; job_id <= job_count + 1; job_id++) {
        if (!used[job_id]) {
            *result = job_id;
            free(used);
            return 0;
        }
    }

    free(used);
    errno = EOVERFLOW;
    return -1;
}


void job_remove(Job* job) {
    char marker = shell.jobs == job ? '+' :
                  (shell.jobs != NULL && shell.jobs->next == job ? '-' : ' ');
    printf("[%zu]%c Done %s\n", job->job_id, marker, job->cmd);
    Job** link = &shell.jobs;
    while (*link != NULL) {
        if (*link == job) {
            *link = job->next;
            job->next = NULL;
            break;
        }
        link = &(*link)->next;
    }
    free_job(job);
}


int job_add(Job* job) {
    if (job->job_id == 0) {
        size_t job_id = 0;
        if (allocate_job_id(&job_id) == -1)
            return -1;
        job->job_id = job_id;
    }
    job_make_current(job);
    if (job->status == STOPPED) {
        printf("[%zu]+ Stopped %s\n", job->job_id, job->cmd);
    } else {
        printf("[%zu]", job->job_id);
        Process* process = job->processes;
        while (process != NULL) {
            printf(" %d", process->pid);
            process = process->next;
        }
        printf("\n");
    }
    fflush(stdout);
    return 0;
}


int add_process(Job* job, pid_t pid) {
    Process* process = malloc(sizeof(Process));
    if (process == NULL) return -1;
    process->pid = pid;
    process->status = RUNNING;
    process->wait_status = 0;
    process->next = NULL;
    if (job->processes == NULL) {
        job->processes = process;
        job->pgid = pid;
    } else {
        Process* p = job->processes;
        while(p->next != NULL) p = p->next;
        p->next = process;
    }
    job->last_pid = pid;
    return 0;
}


static Status calculate_job_status(const Job* job) {
    int has_stopped = 0;
    const Process* process = job->processes;
    while (process != NULL) {
        if (process->status == RUNNING)
            return RUNNING;
        if (process->status == STOPPED)
            has_stopped = 1;
        process = process->next;
    }
    return has_stopped ? STOPPED : DONE;
}


void job_mark_running(Job* job) {
    Process* process = job->processes;
    while (process != NULL) {
        if (process->status == STOPPED)
            process->status = RUNNING;
        process = process->next;
    }
    job->status = calculate_job_status(job);
}


static void update_process(Job* given_job, pid_t pid, int status) {
    Job* job = (given_job == NULL) ? shell.jobs : given_job;
    while (job != NULL) {
        Process* proc = job->processes;
        while (proc != NULL) {
            if (proc->pid == pid) {
                Status old_job_status = job->status;
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    proc->status = DONE;
                    proc->wait_status = status;
                }
                else if (WIFSTOPPED(status)) {
                    proc->status = STOPPED;
                    proc->wait_status = status;
                }
                else if (WIFCONTINUED(status)) {
                    proc->status = RUNNING;
                    proc->wait_status = status;
                }

                job->status = calculate_job_status(job);
                if (given_job == NULL && job->status == DONE) {
                    job_remove(job);
                } else if (given_job == NULL && job->status == STOPPED &&
                           old_job_status != STOPPED) {
                    job_make_current(job);
                    printf("[%zu]+ Stopped %s\n", job->job_id, job->cmd);
                } else if (given_job == NULL && job->status == RUNNING &&
                           old_job_status == STOPPED) {
                    printf("[%zu]+ Continued %s\n", job->job_id, job->cmd);
                }
                return;
            }
            proc = proc->next;
        }
        job = job->next;
    }
}


int wait_foreground_job(Job* job) {
    while (!(job->status == DONE) && !(job->status == STOPPED)) {
        int status = 0;
        pid_t wait_pid = 0;
        // 等待前台进程使用阻塞等待, 且需要额外关注 Ctrl + Z stop 情况(WUNTRACED);
        wait_pid = waitpid(-job->pgid, &status, WUNTRACED);
        if (wait_pid == -1) {
            if (errno == EINTR) {
                // 阻塞系统调用需要处理EINTR(interrupted system call)错误
                continue;
            } else if (errno == ECHILD) {
                break;
            } else {
                perror("wait foreground job");
                return 255;
            }
        } else {
            update_process(job, wait_pid, status);
        }

    }

    Process* process = job->processes;
    while (process != NULL && process->pid != job->last_pid)
        process = process->next;
    if (process == NULL)
        return 255;
    if (WIFEXITED(process->wait_status))
        return WEXITSTATUS(process->wait_status);
    if (WIFSIGNALED(process->wait_status))
        return 128 + WTERMSIG(process->wait_status);
    if (WIFSTOPPED(process->wait_status))
        return 128 + WSTOPSIG(process->wait_status);
    return 0;
}


void check_jobs() {
    pid_t wait_pid;
    int status = 0;
    while (1) {
        wait_pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (wait_pid == 0) {
            break;
        } else if (wait_pid == -1) {
            if (errno == ECHILD) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                perror("check_jobs");
                break;
            }
        } else {
            update_process(NULL, wait_pid, status);
        }
    }
}


void kill_wait_pid(pid_t pid) {
    kill(pid, SIGTERM);
    sleep(1);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}


void kill_wait_job(Job* job) {
    if (job == NULL) return;
    if (job->pgid > 0) {
        kill(-job->pgid, SIGTERM);
        sleep(1);
        kill(-job->pgid, SIGKILL); 
    }

    pid_t wait_pid = 0;
    while (1) {
        wait_pid = waitpid(-job->pgid, NULL, 0);
        if (wait_pid > 0) {
            continue;
        } else if (wait_pid == -1 && errno == EINTR) {
            continue;
        } else if (wait_pid == -1 && errno == ECHILD) {
            break;
        } else {
            perror("kill and wait job failed");
            break;
        }
    }
}
