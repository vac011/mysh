# mysh 开发手册

> 一个类 Bash shell 的实现过程记录。
>
> 使用 **C 语言**，以 POSIX 接口为核心，兼顾 macOS 与 Linux；少量平台扩展会在对应章节中单独说明。
>
> 与 C++ 相比，C 的“重复造轮子”与“笨拙”会迫使我们直面内存、指针和系统调用，这正是系统底层训练所需要的。
>
> mysh 的目标不是完整复刻 Bash，而是通过实现命令解析、进程创建、文件描述符、管道、信号和作业控制，理解 Unix shell 如何把一行文本转化为一组可控制的进程。因此，本项目会优先完成 shell 的核心执行链路，而不追求覆盖 Bash 的全部语法。

## 总体架构

整个项目从输入循环开始，先建立单条命令的执行能力，再逐步引入语法结构和文本展开，最后在执行器之上建立进程组、终端所有权与作业控制。

1. REPL 骨架实现：建立 Read-Eval-Print Loop，统一处理交互输入、`-c` 命令字符串与脚本文件。
2. executor 实现：支持内置命令与外部命令，通过函数指针表分派内置命令，通过 `fork()` -> `execvp()` -> `waitpid()` 流执行并回收外部程序。
3. tokenizer 与 parser 实现：将输入行切分为 token，标记 `WORD`、`PIPE`、`REDIR_IN`、`REDIR_OUT`、`REDIR_APPEND` 等类型，再将 token 组装成 `Command` 和 `Commands` 结构。
    - 支持重定向 `<`、`>` 和 `>>`。
    - 支持多命令管道 `|`。
4. expander 实现：tokenizer 在引号状态下识别完整的 `WORD`，expander 根据字符所处的引号环境完成变量展开、转义处理与引号删除。
    - 支持单引号 `'`、双引号 `"` 和反斜杠 `\`。
    - 支持环境变量 `$VAR`、上一条命令状态 `$?` 与当前 shell PID `$$` 的展开。
5. 作业控制实现：使用进程组组织管道中的进程，维护 Job/Process 状态，并在 shell 与前台作业之间移交终端所有权。
    - 支持前台、后台作业以及 `jobs`、`fg`、`bg` 内置命令。
    - 处理 `SIGCHLD`、`SIGINT`、`SIGQUIT`、`SIGTSTP`、`SIGCONT`、`SIGTTIN` 和 `SIGTTOU` 等信号。
