# 3. tokenizer 与 parser 实现

## 本节背景

在实现了 executor 后，我们已经可以完成单条命令的执行。但是这种执行方式只能处理由空格和 Tab 分隔的单命令, `>`、`<`、`>>` 和 `|` 都还只是普通字符, shell 无法从中识别重定向和多命令管道结构。

因此这一阶段将输入处理拆分为 tokenizer 和 parser 两层。tokenizer 只负责识别 token 的边界和类型, parser 再根据这些 token 构造 Command 链与重定向结构, executor 最后根据结构化结果完成重定向和管道执行。

## 本节目标

- 实现逐字符 tokenizer, 识别 `WORD`、`|`、`<`、`>`、`>>` 和输入结束, 并生成 TokenArray;
- 实现 parser, 将线性的 TokenArray 组装为 Command 链；
- 实现**可重定向**、**管道**多命令执行。

## 实现

### 3.1 定义 Token 数据结构

原来的 `argv` 只能保存字符串, 无法区分一个字符串究竟是普通参数还是 shell 操作符。为此需要为每个词法单元增加类型, 使用 `TokenType` 表示 WORD、管道符、不同重定向符和输入结束, 再由 `Token` 同时保存类型与对应的字符串值。

一行输入会产生数量不确定的 Token, 因此使用 `TokenArray` 动态数组保存, 这里我们模仿 C++ 的 vector 实现, 使用数据指针、count和capacity三个元数据成员保存数组元信息以便后续方便插入、扩容及销毁。

不要问为什么不使用链表, 后面的 `Commands` 命令串就是链表结构, 都用一用当作练习了。实际上这里确实使用链表更好, 因为我们并不需要数组 O(1) 的查找时间复杂度,  而链表 O(1) 的插入效率且无需扩容确实使用起来更加方便。

```c
// tokenizer.h
#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stddef.h>

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_REDIRECT_IN,
    TOKEN_REDIRECT_OUT,
    TOKEN_REDIRECT_APPEND,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    char *value;
} Token;

/* TokenArray 以 TOKEN_EOF 结束。 */
typedef struct {
    Token **tokens;
    size_t count;
    size_t capacity;
} TokenArray;

#endif /* TOKENIZER_H */
```

- `enum`: 用一组具名整数常量表示有限且互斥的状态, 比直接使用无含义的数字更适合表示 Token 类型。

### 3.2 tokenizer 实现

当 `|` 、 `<`、 `>` 等元字符同样成为命令分隔符后, 再直接使用 `strtok_r()` 分隔命令及参数就不太合适了, 因为其会把分隔符直接替换成 `\0`, 而我们的元字符本身就是需要保留的 Token, 因此 tokenizer 改为使用游标逐字符扫描输入。

扫描时遇到空白字符直接跳过; 遇到 `|` 或 `<` 时生成对应的单字符 Token; 遇到 `>` 时先查看下一个字符, 区分 `>` 和 `>>`; 其余字符则从当前位置开始持续向后扫描, 直到遇到空白或元字符, 将中间的子串保存为 WORD。每个分支都负责消费自己的全部字符, 因此循环结束时不需要像双指针那样再额外处理最后一个 WORD。

```c
// tokenizer.c
/*
 * Ruturn NULL when the input is NULL or only spaces.
 */
TokenArray *tokenize(const char *input) {
    if (input == NULL) return NULL;

    TokenArray *token_array = NULL;
    const char *end = input;

    while (*end != '\0') {
        if (isspace((unsigned char)*end)) {
            end++;
            continue;
        }

        // 生成 Token 数据结构
        TokenType token_type;
        char *token_value = NULL;
        switch (*end) {
            case '|':
                token_type = TOKEN_PIPE;
                token_value = strdup("|");
                end++;
                break;

            case '<':
                token_type = TOKEN_REDIRECT_IN;
                token_value = strdup("<");
                end++;
                break;

            case '>':
                if (end[1] == '>') {
                    token_type = TOKEN_REDIRECT_APPEND;
                    token_value = strdup(">>");
                    end += 2;
                } else {
                    token_type = TOKEN_REDIRECT_OUT;
                    token_value = strdup(">");
                    end++;
                }
                break;

            default: {
                token_type = TOKEN_WORD;
                const char *start = end;
                // 当遇到空白符或元字符时作为 WORD 终止条件
                while (*end != '\0' && !isspace((unsigned char)*end) && *end != '|' && *end != '<' && *end != '>') {
                    end++;
                }

                size_t length = (size_t)(end - start);
                token_value = malloc(length + 1);
                if (token_value == NULL) goto failed;
                memcpy(token_value, start, length);
                token_value[length] = '\0';
                break;
            }
        }

        // 插入 Token 进 TokenArray
        if (token_value == NULL) goto failed;
        token_array = push_token(token_array, token_type, token_value);
        if (token_array == NULL) {
            free(token_value);
            return NULL;
        }
    }

    // 放置哨兵 TOKEN_EOF
    token_array = push_token(token_array, TOKEN_EOF, NULL);
    return token_array;

failed:
    free_tokens(token_array);
    return NULL;
}
```

- `isspace(int ch)`: 判断空格、Tab、换行等空白字符。参数必须先转换为 `unsigned char`, 因为负值 `char` 直接传入字符分类函数会导致未定义行为。此类字符判断函数均声明于 `<ctype.h>` 中。
- `memcpy()`: 当已知字符串长度时, 使用 `memcpy()` 比使用 `strcpy()` 性能更好, 但不会自动追加字符串终止符, 所以必须额外写入 `token_value[length] = '\0'`。
- `strcpy()`: 尽量绝对不要使用, 因为其并不会判断目的缓冲区大小, 极其危险的函数, 应使用 `strncpy()` 替代。

### 3.3 定义 Command 结构

TokenArray 只描述输入中依次出现了哪些 Token, 但 executor 需要知道哪些 WORD 属于同一条命令、哪些 WORD 是重定向目标, 以及管道中存在多少条命令。因此 executor 的输入不能只是一串简单 `TokenArray`, 而是由 Command 节点组成的链表。

每个 Command 保存自己的 `argv` 和重定向数组, `next` 指向管道中的下一条命令。重定向单独使用 `Redirector` 保存类型和目标路径, 不再把 `>` 与文件名传入命令的 `argv`。最外层的 `Commands` 保存链表头, 并记录当前输入是否包含管道。

```c
// parser.h
#ifndef PARSER_H
#define PARSER_H

#include "tokenizer.h"

typedef struct {
    TokenType type;
    char *target;
} Redirector;

typedef struct Command {
    char **argv;
    Redirector *redirs;
    struct Command *next;
} Command;

typedef struct {
    Command *command;
    int pipeline;
} Commands;

#endif /* PARSER_H */
```

### 3.4 实现 parser

parser 按顺序遍历 TokenArray, 根据 Token 类型更新当前 Command:

- **WORD** 被追加到当前 `argv`;
- **REDIRECT-** 要求下一个 Token 必须是 **WORD**, 并将这个 **WORD** 作为重定向目标文件;
- **PIPE** 结束当前 Command 并创建下一个 Command;
- **EOF** 结束解析。

parser 不再处理字符边界, 但需要负责结构是否合法。例如管道前后不能出现空命令, 重定向符后必须存在目标。

此外, `argv` 和 重定向 `target` 直接**借用** Token 中的 `value`。因此 Commands 必须先于 TokenArray 释放, `free_commands()` 只能释放数组和节点, 不能释放其中借用的字符串。

```c
// parser.c（核心结构）
Commands *parse_commands(TokenArray *token_array) {
    if (token_array == NULL) return NULL;

    Commands *commands = malloc(sizeof(*commands));
    if (commands == NULL) return NULL;
    commands->command = NULL;
    commands->pipeline = 0;

    Command *current = malloc(sizeof(*current));
    if (current == NULL) {
        free(commands);
        return NULL;
    }
    commands->command = current;
    current->argv = NULL;
    current->redirs = NULL;
    current->next = NULL;

    size_t argc = 0, argv_capacity = 0;
    size_t redir_count = 0, redir_capacity = 0;

    for (size_t i = 0; i < token_array->count; i++) {
        Token *token = token_array->tokens[i];

        if (token->type == TOKEN_WORD) {
            // 扩容 current->argv, 借用 token->value 并保证末尾为 NULL。
            append_argument(current, token->value, &argc, &argv_capacity);
        } else if (token->type == TOKEN_REDIRECT_IN ||
                   token->type == TOKEN_REDIRECT_OUT ||
                   token->type == TOKEN_REDIRECT_APPEND) {
            if (i + 1 >= token_array->count ||
                token_array->tokens[i + 1]->type != TOKEN_WORD) {
                fprintf(stderr, "syntax error near '%s'\n",
                        token->value);
                goto failed;
            }

            // 扩容 redirs, 保存类型并借用下一个 WORD 作为 target。
            append_redirector(current, token->type, token_array->tokens[i + 1]->value, &redir_count, &redir_capacity);
            i++;
        } else if (token->type == TOKEN_PIPE) {
            if (current->argv == NULL) {
                fprintf(stderr, "syntax error near '|'\n");
                goto failed;
            }

            commands->pipeline = 1;
            current->next = create_command();
            if (current->next == NULL) goto failed;
            current = current->next;

            argc = argv_capacity = 0;
            redir_count = redir_capacity = 0;
        } else {
            // TOKEN_EOF
            if (current->argv == NULL) {
                fprintf(stderr, "syntax error: uncompleted command\n");
                goto failed;
            }
            break;
        }
    }

    return commands;

failed:
    free_commands(commands);
    return NULL;
}
```

### 3.5 重定向实现

parser 已经把重定向从 `argv` 中分离出来, executor 接下来需要根据重定向符将标准输入/输出/错误映射到某个文件。多个重定向按照出现顺序依次执行, 后面的 `dup2()` 可以覆盖前面的结果。

外部命令在 fork 后的子进程中应用重定向, fd 的变化不会影响父 shell。

独立 builtin 在父 shell 中执行前必须**保存**原来的标准输入输出, 执行后先刷新 stdio 缓冲区再**恢复**。

```c
// executer.c（重定向核心逻辑）
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
```

### 3.6 管道实现

任意长度管道可以使用两组滚动的 fd 实现: `last_fds` 表示当前命令与上一条命令之间的管道, `current_fds` 表示当前命令与下一条命令之间的管道。

标准Shell采用**动态流水线策略**：每解析一个命令，创建它和下一个命令之间的管道，然后立即fork当前进程。

父进程每 fork 一个子进程后就要及时关闭当前 `current` 管道的读端, 以及上一轮 `last` 管道的写端, 因为这两端已经被本轮 fork 出的子进程使用到了, 然后把 `current` 移交为下一轮的 `last`。

子进程仍然需要用 `dup2()` 把写端安装为标准输出, 或把读端安装为标准输入。

管道执行中最重要的是**关闭规则**。读端只有在指向该管道写端的**所有**文件描述符都关闭后才能读到 EOF; 而写端如果尝试向读端全部关闭的管道写数据, 内核将直接向写端进程发送 SIGPIPE 信号（默认动作是终止进程, 除非进程捕获或忽略）。

```c
// executer.c（管道核心逻辑）
static void handle_pipes(int last_fds[2], int current_fds[2]) {
    if (current_fds[0] != -1) {
        close(current_fds[0]);
        dup2(current_fds[1], STDOUT_FILENO);
        close(current_fds[1]);
    }

    if (last_fds[0] != -1) {
        dup2(last_fds[0], STDIN_FILENO);
        close(last_fds[0]);
    }
}

static int execute_commands(Commands *commands) {
    Command *first = commands->command;
    char *fn_name =
        first->argv == NULL ? NULL : first->argv[0];
    builtin_fn fn = lookup_builtin(fn_name);

    // 独立 builtin 仍然在父 shell 中执行。
    if (!commands->pipeline && fn != NULL) {
        int backup_in_fd = STDIN_FILENO;
        int backup_out_fd = STDOUT_FILENO;
        if (handle_redirs(first->redirs, &backup_in_fd, &backup_out_fd) == -1)
            return 1;

        int status = fn(first->argv);
        fflush(stdout);
        // 需要恢复标准输入输出
        restore_stdio(backup_in_fd, backup_out_fd);
        return status;
    }

    int last_fds[2] = {-1, -1};
    int current_fds[2] = {-1, -1};
    size_t child_count = 0;
    pid_t last_pid = -1;

    for (Command *command = commands->command;
         command != NULL;
         command = command->next) {
        if (command->next != NULL) {
            if (pipe(current_fds) == -1) return 1;
        } else {
            current_fds[0] = current_fds[1] = -1;
        }

        pid_t pid = fork();
        if (pid == -1) return 1;

        if (pid == 0) {
            //  POSIX 标准（Shell & Utilities 卷，第 2.9.2 节“管道”）。标准明确规定：
            // "The standard output of command1 shall be connected to the standard input of command2 ...
            // This redirection shall be performed before any redirections specified by the command itself."
            // Bash 参考手册在“Pipelines”一节中也明确指出：
            // "... This connection is performed before any redirections specified by command1."
            // 因此管道应该在重定向前被解析, 以便能被重定向覆盖。
            handle_pipes(last_fds, current_fds);
            if (handle_redirs(command->redirs, NULL, NULL) == -1)
                _exit(1);

            char *fn_name =
                command->argv == NULL ? NULL : command->argv[0];
            builtin_fn fn = lookup_builtin(fn_name);
            if (fn) {
                // 管道中的 builtin 要在子进程中执行, 因为管道要求所有成员并发执行
                int status = fn(command->argv);
                fflush(stdout);
                _exit(status);
            } else {
                execvp(fn_name, command->argv);
                perror("execvp");
                _exit(127);
            }
        }

        child_count++;
        last_pid = pid;

        if (current_fds[0] != -1)
            close(current_fds[1]);
        if (last_fds[0] != -1)
            close(last_fds[0]);

        last_fds[0] = current_fds[0];
        last_fds[1] = -1;
    }

    int result = 0;
    while (child_count > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid == -1) {
            if (errno == EINTR) continue;
            perror("waitpid");
            return 1;
        }
        child_count--;

        if (pid == last_pid) {
            if (WIFEXITED(status))
                result = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result = 128 + WTERMSIG(status);
        }
    }
    return result;
}
```

- `pipe(int pipefd[2])`: 创建一对文件描述符, 成功后 `pipefd[0]` 是读端, `pipefd[1]` 是写端。声明于 `<unistd.h>` 中。

管道中的 builtin 还存在一项容易被不同 shell 行为混淆的执行环境差异。GNU Bash 默认让管道中的每一段都在独立的子进程环境中运行，因此即使 `cd` 位于最后一段，它对工作目录的修改也不会保留到父 Bash；Bash 可以通过 `lastpipe` 选项改变最后一段的行为，但该选项要求 job control 没有启用。zsh 则通常让管道最后一段 builtin 在当前 shell 环境中执行，而位于它左侧的管道成员仍然在子进程中执行。

```sh
cd /tmp
printf x | cd /
pwd
```

默认 Bash 仍然输出 `/tmp`，而原生模式 zsh 输出 `/`。反过来写成 `cd / | printf x` 时，`cd` 位于管道左侧，在两个 shell 中都不会改变父 shell 的目录。

mysh 当前采用的是 Bash 的默认策略：只要 Commands 中存在管道，每个 Command 都由 executor fork 子进程执行，包括管道最后一段 builtin。因此 `cd`、变量赋值或其他会修改 shell 自身状态的 builtin 位于管道中时，其修改不会保留到父 mysh。具体差异可以参考 [Bash 的 Pipelines](https://www.gnu.org/software/bash/manual/html_node/Pipelines.html) 与 [zsh 对管道执行环境的说明](https://zsh.sourceforge.io/Guide/zshguide03.html)。

### 3.7 无命令重定向 NULLCMD

parser 允许一个 Command 没有 `argv`，但至少包含一个重定向，例如：

```sh
> output
echo aaa | > output
> output | echo bbb
```

第一条命令只需要创建或截断 `output`；后两条命令则让没有命令名的 Command 作为管道中的一个成员。虽然它不执行任何用户程序，executor 仍然要先为它建立管道和重定向，并产生一个可以用于判断成功或失败的执行结果。

如果继续沿用普通外部命令分支，代码会访问不存在的 `argv[0]`，甚至把空命令名传给 `execvp()`，既不符合接口契约也不具备可移植行为。为避免在 executor 中到处增加“argv 是否为空”的特殊分支，我们实现一个内部使用的 `builtin_null`：它不执行任何操作，只返回成功状态。`lookup_builtin()` 收到空命令名时直接返回该函数，从而让无命令重定向复用现有 builtin 执行路径。

```c
// builtin.c
static int builtin_null(char **argv) {
    (void)argv;
    return 0;
}

builtin_fn lookup_builtin(char *func_name) {
    if (func_name == NULL)
        return builtin_null;

    for (size_t i = 0;
         builtins[i].func_name != NULL;
         i++) {
        if (strcmp(func_name,
                   builtins[i].func_name) == 0)
            return builtins[i].fn;
    }
    return NULL;
}
```

`builtin_null` 不是用户可以输入名称调用的普通 builtin，也不需要加入 `builtins` 函数表；它只是 executor 对“合法但没有命令名的 Command”使用的内部实现。

独立无命令重定向仍然走父 shell 的 builtin 路径。executor 先保存标准输入输出并调用 `handle_redirs()`，所以目标文件会被正常创建、截断或打开；随后 `builtin_null` 返回 `0`，executor 再恢复原来的文件描述符。重定向失败时，`handle_redirs()` 会直接返回错误，空命令本身的成功状态不会覆盖这个错误。

位于管道中的无命令 Command 则在子进程中运行。子进程先安装管道、应用自身重定向，再调用 `builtin_null` 并退出。它不会主动读取管道输入，也不会把输入复制到输出文件。例如 mysh 中的：

```sh
printf aaa | > output
```

会创建一个空的 `output`，而不是把 `aaa` 写入文件。上游进程是否正常完成或收到 `SIGPIPE` 取决于写入量和两侧进程的运行时序，但整条管道默认仍以最后一个空命令的状态作为返回值。

### 3.8 bash 与 zsh 语义差异

关于管道 bash 和 zsh 的设计并不完全一致，而且区别来自 shell 语义，不是 Linux 与 macOS 内核本身：

- GNU Bash 在没有命令名时只执行重定向；重定向不会永久改变当前 shell，若没有重定向错误或命令替换，命令状态为 `0`。因此 Bash 中 `printf aaa | > output` 得到空文件，这与 mysh 的 `builtin_null` 语义一致。
- zsh 原生模式默认设置 `NULLCMD=cat`，会为某些只有重定向的语法隐式补入 `cat`；只有输入重定向时还可能使用默认值为 `more` 的 `READNULLCMD`。因此 macOS 默认 zsh 中 `printf aaa | > output` 通常等价于 `printf aaa | cat > output`，文件中会得到 `aaa`。启用 `SH_NULLCMD` 或以 sh/ksh 模式运行 zsh 时，zsh 会改为隐式使用 `:`，行为才更接近 Bash。

具体规则可以分别参考 [Bash 的 Simple Command Expansion](https://www.gnu.org/software/bash/manual/html_node/Simple-Command-Expansion.html) 与 [zsh 的 Redirections with no command](https://zsh.sourceforge.io/Doc/Release/Redirection.html#Redirections-with-no-command)。

此外, 在 zsh 中, 管道的最后一条命令如果是 builtin 则在当前 shell 环境中执行, 而不是像 bash 那样全都在子进程中执行。

## 总结

完成本阶段后, mysh 已经从“只能执行一条由空白切分的简单命令”, 发展为能够识别命令结构并执行重定向和任意长度管道的 shell。第二阶段建立的 builtin/external 分派和 `fork/exec/wait` 模型没有被推翻, 而是被放入 Command 与管道执行模型中继续使用。

- `tokenizer.h/.c`: 定义 Token 类型与 TokenArray, 完成逐字符词法分析；
- `parser.h/.c`: 定义 Command、Redirector 与 Commands, 将 TokenArray 组织为命令链；
- `executer.c`: 根据 Command 结构处理重定向和多命令管道；
- `shell.h/.c`: 声明并连接 tokenizer、parser 与 executor 的公共接口。

当前 tokenizer 只根据空白和元字符确定边界, 还没有理解引号、反斜杠和变量展开。下一阶段将在 tokenizer 与 parser 之间加入 expander, 并为 tokenizer 增加引号状态, 使同一个 WORD 能够由普通字符、引号内容和变量展开结果共同组成。
