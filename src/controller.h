#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "job.h"
#include <sys/types.h>

typedef struct {
    pid_t pid;
    pid_t pgid;

    struct termios terminal_modes;

    Job* jobs;
    
    int interactive;
    int should_exit;
    int last_status;
} ShellState;


#endif /*CONTROLLER_H*/
