# 2. executor 实现

## 本节背景

REPL 得到的只是一整行 C 字符串并打印它。为了能够执行一条命令，shell 至少还需要完成两件事：

1. 将字符串按**空白符**拆分为 argv, 第一个元素作为命令, 其余元素作为该命令的参数。
2. 判断命令是内置命令还是外部程序，并选择对应的执行方式。

## 本节目标

- 使用 `strtok_r()` 将输入字符串按空格和 Tab 拆分为以 `NULL` 结尾的 `argv`, 并明确其内存所有权；
- 使用统一的函数签名和函数指针表注册、查找并执行内置命令；
- 理解 `fork -> execvp -> waitpid` 的进程执行模型, 用子进程执行外部程序并由父进程完成回收；
- 正确区分正常退出、执行失败和信号终止, 将它们转换为 shell 的命令退出状态；

## 实现

### 2.1 拆分 argv

首先使用 `parse_command()` 函数将输入字符串按**空白符**拆分为 argv。

```c
void free_argv(char **args) {
    if (args == NULL) return;
    for (int i = 0; args[i] != NULL; i++) free(args[i]);
    free(args);
}

// 一般如果接收字符串参数除了字符串变量外, 还可能包括**常量字符串**(如 `fn("A Const String")`) 就要在声明时使用 `const char*`, 而不是单纯的 `char*`。
char **parse_command(const char* command) {
    if (command == NULL) return NULL;

    char** argv = NULL;
    size_t count = 0;
    size_t capacity = 0;

    char *input = strdup(command); 
    if (input == NULL) return NULL;

    char *token = NULL;
    char *saveptr = NULL;
    // 第一次分割时传入 input, 并初始化 saveptr
    token = strtok_r(input, " \t", &saveptr);
    while (token != NULL) {
        if (count + 1 >= capacity) {
            capacity = capacity > 0 ? capacity * 2 : 2;
            char **new_argv = realloc(argv, sizeof(char *) * capacity);
            if (new_argv == NULL) {
                free_argv(argv);
                free(input);
                return NULL;
            }
            argv = new_argv;
        }
        // 每个 token 独立拷贝一份
        argv[count] = strdup(token);
        if (argv[count] == NULL) {
            free_argv(argv);
            free(input);
            return NULL;
        }
        count++;
        argv[count] = NULL;
        // 后续切割无需再传入 input
        token = strtok_r(NULL, " \t", &saveptr);
    }
    free(input);
    return argv;
}
```

- `realloc(void *ptr, size_t size)`: `realloc` 将重新分配的堆块地址作为返回值返回, 但当分配失败时并不会直接将原指针返回, 而是返回 NULL。因此在使用时应该先用一个临时指针接收返回值, 待判断结果不为空时再赋值给原指针, 而不应该直接用原指针接收返回值, 否则可能导致内存泄漏。一般内存管理函数都位于 `<stdlib.h>` 中, 例如 `malloc()`、`calloc()`、`free()` 等。

- `strdup(const char *s)`: 可以直接使用 `strdup` 复制字符串。它会在堆上分配一块足以保存副本的内存, 失败时返回 `NULL`, 使用完需要调用 `free()`。`strdup()` 是 POSIX 接口, 声明于 `<string.h>` 中。

- `strtok_r(char *restrict s, const char *restrict delim, char **restrict saveptr)`: 按分隔符集合切分字符串, 声明于 `<string.h>` 中。
  - `strtok_r()` 是 `strtok()` 的可重入版本。解析状态由调用者提供的 `saveptr` 保存, 因此是线程安全的, 可以避免多线程环境下的冲突。
  - `strtok()` 系列会直接修改传入字符串, 把找到的分隔符替换为 `\0`; 如果原字符串还需要保留, 应先使用 `strdup()` 创建副本。
  - 第一次调用传入待切分字符串, 后续调用的第一个参数必须传入 `NULL`; 返回值是指向被修改字符串内部的**借用指针**, 需要最后统一 `free()` 而不能单独释放。

### 2.2 builtin 内置命令

一些会**改变 shell 自身状态**或**使用 shell 内部数据结构**的命令必须在父 shell 中执行, 不能像外部程序一样在 fork 出的子进程中执行, 因此需要为这些命令构造对应的**内置命令函数**。

在实现 executor 之前, 我们需要先实现一个简单的内置命令表, 使得 executor 可以通过函数指针表查找内置命令并执行。

我们使用 `typedef int (*builtin_fn)(char **argv)` 定义内置命令函数指针类型, 统一所有内置命令的函数签名, 并使用 `struct Builtin` 将命令名和对应的函数指针绑定在一起, 形成一个函数指针表。

```c
// builtin.h
// 在创建头文件时, 应该在头文件开头使用 **include guard** 防止重复包含。
#ifndef BUILTIN_H
#define BUILTIN_H

// 注意这里定义函数指针的写法, 由于 C 规定**声明要类似使用方式**
// 因此一个普通函数指针**变量**的声明方式是 `返回类型 (*变量名)(参数类型)`
// 则一个函数指针**类型**的声明方式是 `typedef 返回类型 (*类型名)(参数类型)`。
typedef int (*builtin_fn)(char** argv);

typedef struct {
    char* func_name;
    builtin_fn fn;
} Builtin;

#endif /*BUILTIN_H*/
```

定义一个 Builtin 内置命令表, 将其声明为 `static const`, 避免运行时被改动。唯一对外暴露的函数只有 `lookup_builtin`, 根据命令名查找对应的函数指针。

一般不对外暴露的函数及全局变量也都声明并定义为 `static`，这样它们的作用域仅限于当前文件，避免与其他文件中的同名函数冲突, 且避免被其他文件调用, 造成不必要的依赖, 同时还可以让编译器采取更加激进的优化策略。

```c
// builtin.c
static int builtin_echo(char** argv);
static int builtin_cd(char** argv);
static int builtin_pwd(char** argv);
static int builtin_exit(char** argv);

static const Builtin builtins[] = {
    {"echo", builtin_echo},
    {"cd", builtin_cd},
    {"pwd", builtin_pwd},
    {"exit", builtin_exit},
    {NULL, NULL}
};


builtin_fn lookup_builtin(char* func_name) {
    if (func_name == NULL) return NULL;
    for (size_t i = 0; builtins[i].func_name != NULL; i++) {
        if (strcmp(func_name, builtins[i].func_name) == 0) return builtins[i].fn;
    }
    return NULL;
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
    // 使用 (void) 避免编译警告“未使用的参数”
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
```

- `getenv(const char* name)`: 根据名称查找环境变量, 找到后返回指向进程环境存储区的**借用指针**, 未找到时返回 `NULL`。该指针所有权不属于调用者, 因此不能对它调用 `free()`; 后续调用 `setenv()`、`unsetenv()` 修改环境时, 之前取得的指针还可能失效。该函数声明于 `<stdlib.h>` 中。

- `getcwd(char *buf, size_t size)`: 将当前工作目录写入 `buf`, 成功时返回 `buf`, 失败时返回 `NULL`。`getcwd(NULL, 0)` 会在 glibc、macOS 等实现中动态分配足够大的缓冲区, 使用完必须 `free()`, 但这种调用方式不是 POSIX 保证的可移植形式。该函数声明于 `<unistd.h>` 中。

- `strtol(const char *nptr, char **endptr, int base)`: 将字符串转换为 `long`, 并通过 `*endptr` 返回第一个未参与转换字符的位置。健壮的判错需要同时检查: `endptr == nptr` 表示一个数字也没有读到, `*endptr != '\0'` 表示尾部仍有非法字符, `errno == ERANGE` 表示结果超出 `long` 的范围。相比之下, `atol()` 无法可靠地区分合法的 `0` 与转换失败。此类转换函数声明于 `<stdlib.h>` 中。

### 2.3 执行器 executor

shell 执行命令的核心逻辑为:

1. 判断命令类型: 内置命令或外部命令；
2. 当且仅当命令为**独立**、**前台**、**内置命令**时, 才会在父 shell 中直接执行;
3. 否则通过 `fork/execvp/waitpid` 一串系统调用在子进程中执行命令, 父进程等待子进程结束。

这里我们的执行器 executor 接收刚刚切割得到的 argv:

```c
// executer.c
int execute_command(char** argv){
    if (argv == NULL) return 0;
    builtin_fn fn = lookup_builtin(argv[0]);
    if (fn) {
        // 执行内置命令
        int ret_code = fn(argv);
        // 一定要刷新输出, 否则可能导致下一次提示符显示混乱
        fflush(stdout);
        return ret_code;
    } else {
        pid_t pid = fork();
        // fork 失败
        if (pid < 0) {
            perror("fork");
            return 1;
        } 
        // 子进程
        if (pid == 0) {
            execvp(argv[0], argv);
            // execvp 失败, 直接_exit退出。
            perror("execvp");
            // `127` 通常表示命令无法执行或未找到。
            _exit(127);
        }
        // 父进程
        int status = 0;
        while (waitpid(pid, &status, 0) == -1) {
            if (errno == EINTR) {
                // 阻塞系统调用需要处理EINTR(interrupted system call)错误
                continue;
            } else {
                perror("wait pid");
                return 1;
            }
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            // shell 通常把“被信号终止”转换为 `128 + signal_number` 作为退出值
            return 128 + WTERMSIG(status);
        } else {
            // 由于未开启 `WUNTRACED` 和 `WCONTINUED` 选项, 无需考虑 `WIFSTOPPED` 和 `WIFCONTINUED` 条件。
            // 不会执行到这里, 只是为了返回值兜底
            perror("wait pid status")
            return 1;
        }
    }
}
```

- `fork()`: 有三种返回情况: 在子进程中返回 `0`, 在父进程中返回子进程 PID, 失败时只在父进程中返回 `-1`, 且不会创建子进程。逻辑上子进程得到父进程地址空间的副本, 操作系统通常通过写时复制(COW)延迟真正的内存复制。子进程还会继承打开的文件描述符、信号 disposition 与信号屏蔽字、用户身份、进程组和会话等状态, 但拥有新的 PID, 且不会继承父进程的待处理信号集合。该函数声明于 `<unistd.h>` 中。

- `execvp(const char *file, char *const argv[])`: 用目标程序替换当前进程的用户态内存映像, 包括代码段、数据段、堆和栈; 成功后不会返回。名称中的 `p` 表示: 当 `file` 不包含 `/` 时按 `PATH` 搜索, 包含 `/` 时直接把它当作路径。exec 不会创建新进程, 因而 PID、当前目录、umask、信号处理句柄、进程组和会话等进程属性仍然存在; 未设置 `FD_CLOEXEC` 的文件描述符也会保留, 设置了该标志的描述符则会在 exec 时关闭。该函数声明于 `<unistd.h>` 中。
  - 阅读带 `const` 的声明时应从变量名向外分析。`const char *p` 是“指向只读字符的指针”; `char *const p` 是“自身不可重新赋值、但所指字符可修改的指针”; `const char **p` 是“指向一个‘指向只读字符的指针’的指针”; `char **const p` 是“自身不可重新赋值的二级指针”; `char *const *p` 是“指向 const 字符指针的指针”, 因此不能通过 `p` 改写 `*p` 的指向。

- `waitpid(pid_t pid, int *stat_loc, int options)`: 等待符合条件的子进程发生指定状态变化, 并返回该状态; 当取得的是终止状态时, 同时完成对子进程的回收。该函数声明于 `<sys/wait.h>` 中。
  - **pid**: 指定等待的子进程, 可以为以下几种值:
    - **-1**: 等待任意一个子进程, `wait(int* stat_loc)` 相当于 `waitpid()` 在 pid = -1、默认阻塞、只等待退出时的特例。
    - **0**: 等待与调用进程同进程组（PGID）的任意一个子进程。
    - **<-1**: 等待进程组 ID 等于 `pid` 绝对值的任意一个子进程。它仍然只能等待调用进程自己的子进程，不能借此等待其他进程的子进程。
    - **>0**: 等待指定pid对应的子进程。
  - **stat_loc**: 指向存放进程状态的变量的地址, 用于 `wait` 返回进程状态, 可以使用以下宏来读取状态值:
    - `WIFEXITED(status)`: 检查进程是否正常终止, 包括调用 `exit()`、`_exit()` 或从 `main()` 返回; 使用 `WEXITSTATUS(status)` 读取退出状态；
    - `WIFSIGNALED(status)`: 检查进程是否被**信号终止**；使用 `WTERMSIG(status)` 读取终止信号。
      - `SIGKILL(9)`: 强制终止信号, 立即杀死进程, 唯二不能被捕获或忽略的信号之一。
      - `SIGINT(2)`: 中断信号, 即 Ctrl+C。可捕获或忽略。程序捕获后可做清理工作后再退出，而不是被瞬间杀死。
      - `SIGTERM(15)`: 标准终止信号, `kill` 命令的默认信号。可捕获或忽略。同样进程可以选择捕获后做清理工作后再退出。
      - `SIGHUP(1)`: 终端挂断信号, 当终端关闭时会内核发送该信号。可捕获或忽略, 一般直接退出。
      - `SIGABRT(6)`: 异常终止信号, `abort()` 函数发送的信号。在产生异常时(比如`assert()`失败)使用 `abort()` 退出可以生成 core dump 文件并产生终止信号, 而 `exit()` 会直接清理内存不产生 core dump 文件。
      - `SIGILL(4)/SIGSEGV(11)`: 非法指令/段错误, 此类硬件异常可以被捕获, 但绝不建议忽略, 一般直接退出。
    - `WIFSTOPPED(status)`: 检查进程是否被**信号停止**；使用 `WSTOPSIG(status)` 读取停止信号。需要使用 `WUNTRACED` 选项才能捕获到。
      - `SIGSTOP(19)`: 强制暂停信号, 立即终止进程, 唯二不能被捕获或忽略的信号之一。
      - `SIGTSTP(20)`: 终端暂停信号, 即 Ctrl+Z, 可捕获或忽略。
      - `SIGTTIN(21)`: 终端输入暂停信号, 当后台进程尝试写入终端时发送, 可捕获或忽略, 一般会将进程挂起。
      - `SIGTTOU(22)`: 终端输出暂停信号, 当后台进程尝试写入终端时发送, 可捕获或忽略, 一般会将进程挂起。
    - `WIFCONTINUED(status)`: 检查进程是否被**信号继续**。无需专门信号读取宏, 需要使用 `WCONTINUED` 选项才能捕获到。
      - `SIGCONT(18)`: 继续信号, 用来恢复被暂停的进程。可捕获或忽略。
  - **options**: 等待选项参数, 可以为以下几种值:
    - **0**: 阻塞等待一个子进程结束。
    - `WNOHANG`: 如果当前没有符合所请求状态变化的子进程, 不阻塞等待而是立即返回 `0`。
    - `WUNTRACED`: 要求报告尚未被跟踪的停止事件, 此时当有子进程被信号暂停时也返回(而不只是退出)。
    - `WCONTINUED`: 要求报告收到 `SIGCONT` 后的继续事件, 当有子进程被信号控制继续执行时也返回。
  - **pid_t返回值**: 返回值有三类:
    - **>0**: 返回wait成功的子进程pid。
    - **=0**: 只有使用 `WNOHANG` 时才会立即返回, 表示当前没有符合所请求状态变化的子进程。
    - **-1**: 等待失败。此时应检查 `errno` 的值。
      - `EINTR`: 系统调用被中断, 应尝试重新调用。
      - `ECHILD`: 没有子进程, 在循环等待所有子进程结束时可以作为退出循环依据。
      - 其他: 异常错误, 应调用 `perror()` 报告并退出。

- `_exit()`: `_exit()` 是 POSIX 提供的立即终止进程接口, 不会像 `exit()` 那样运行 `atexit()` 注册的处理函数, 也不刷新 stdio 缓冲区。`fork()` 后的子进程会继承父进程 stdio 缓冲区的副本, 因此 exec 失败时使用 `_exit()` 可以避免把父进程尚未刷新的内容重复输出。

## 总结

- `builtin.h/.c`：定义统一的内置命令函数类型和函数指针表, 实现 `echo`、`cd`、`pwd`、`exit`；
- `executer.c`：负责简单的 `argv` 拆分、内置/外部命令分派、外部进程创建与等待；
- `shell.h`：声明本阶段共享的 shell 状态和执行接口；
- `shell.c`：由 REPL 调用 executor, 保存 `last_status`, 并根据 `should_exit` 决定是否结束循环。
