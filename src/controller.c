#include "controller.h"
#include "shell.h"
#include <unistd.h>
#include <signal.h>
#include <errno.h>

int child_restore_signals() {
    if (signal(SIGINT, SIG_DFL) == SIG_ERR) return -1;  // Ctrl-C
    if (signal(SIGQUIT, SIG_DFL) == SIG_ERR) return -1;  /* Ctrl-\ */
    if (signal(SIGTSTP, SIG_DFL) == SIG_ERR) return -1;  // Ctrl-Z
    if (signal(SIGTTIN, SIG_DFL) == SIG_ERR) return -1;  // background read
    if (signal(SIGTTOU, SIG_DFL) == SIG_ERR) return -1;  // background write
    return 0;
}


int shell_ignore_signals() {
    if (signal(SIGINT, SIG_IGN) == SIG_ERR) return -1;  // Ctrl-C
    if (signal(SIGQUIT, SIG_IGN) == SIG_ERR) return -1;  /* Ctrl-\ */
    if (signal(SIGTSTP, SIG_IGN) == SIG_ERR) return -1;  // Ctrl-Z
    if (signal(SIGTTIN, SIG_IGN) == SIG_ERR) return -1;  // background read
    if (signal(SIGTTOU, SIG_IGN) == SIG_ERR) return -1;  // background write
    // if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) return -1;  // child status change
    return 0;
}


int controller_init(int interactive) {
    shell.pid = getpid();
    shell.pgid = getpgrp();
    shell.jobs = NULL;
    shell.last_status = 0;
    shell.should_exit = 0;
    shell.interactive = interactive;

    if (!shell.interactive)
        return 0;

    // 1. 检查自己是否在前台, 如果自己还不在前台进程组，就让内核用 SIGTTIN 暂停，等待父 shell 把我们切回前台后再继续。
    while (tcgetpgrp(STDIN_FILENO) != shell.pgid) {
        if (kill(-shell.pgid, SIGTTIN) == -1)
            return -1;
    }

    // 2. 确保 shell 是独立进程组的组长: pgid 等于 pid。
    if (shell.pgid != shell.pid) {
        if (setpgid(0, shell.pid) == -1)
            return -1;
        shell.pgid = getpgrp();
    }

    // 3. 忽略一般信号(在 tcsetpgrp 前, 防止 setpgid 后不再是前台进程组而被 SIGTTOU 挂起)
    if (shell_ignore_signals() == -1)
        return -1;

    // 4. 设置前台进程组为 shell 所在组
    if (tcsetpgrp(STDIN_FILENO, shell.pgid) == -1)
        return -1;

    // 5. 保存终端属性
    if (tcgetattr(STDIN_FILENO, &shell.terminal_modes) == -1)
        return -1;

    return 0;
}


int give_terminal(Job* job) {
    if (shell.interactive == 0) return 0;
    while (tcsetpgrp(STDIN_FILENO, job->pgid) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &job->terminal_models) == -1) {
        return -1;
    }

    return 0;
}


int take_terminal(Job* job) {
    if (shell.interactive == 0) return 0;
    while (tcsetpgrp(STDIN_FILENO, shell.pgid) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    if (tcgetattr(STDIN_FILENO, &job->terminal_models) == -1) {
        return -1;
    }

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &shell.terminal_modes) == -1) {
        return -1;
    }

    return 0;
}
