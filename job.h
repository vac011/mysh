#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>
#include <termios.h>

typedef enum {
    RUNNING,
    STOPPED,
    DONE
} Status;


typedef struct Process {
    pid_t pid;
    Status status;
    int wait_status;
    struct Process* next;
} Process;


// Job->cmd is deepcopy
typedef struct Job {
    size_t job_id;
    pid_t pgid;
    char* cmd;
    Status status;
    int background;
    Process* processes;
    pid_t last_pid;
    struct termios terminal_models;
    struct Job* next;
} Job;

#endif /*JOBS_H*/