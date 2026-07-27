# 1. REPL 骨架实现

## 本节背景

一个 shell 程序一般有以下几种启动方式：

- 直接启动：无参数启动，进入交互式模式。shell 在终端中打印命令提示符，读取用户输入并等待下一条命令，直到用户退出 shell。
- 以 `-c` 参数启动：shell 接收一个字符串作为命令，处理该命令后退出。
- 以脚本文件启动：shell 接收一个脚本文件名作为参数，依次读取其中的命令，直到文件结束。

shell 后续的解析、展开和执行都以完整的输入行为起点，因此首先需要统一三种输入来源，为后续阶段提供稳定的命令入口。

## 本节目标

建立 shell 最初的 REPL 骨架，完成“读取输入 -> 处理当前行 -> 继续读取”的循环。本阶段只回显取得的命令文本，暂不进行参数拆分和命令执行。

## 实现

### 1.1 启动参数检查

首先根据 `argc` 和 `argv` 判断 shell 的启动方式:

- 无额外参数时进入交互循环；
- 第一个参数为 `-c` 时取 `argv[2]` 作为单条命令；
- 否则将 `argv[1]` 视为脚本文件。脚本模式可以先打开文件，再用 `dup2()` 将其复制到标准输入，从而让脚本文件和终端输入共用同一套按行读取逻辑。

```c
int main(int argc, char **argv) {
    if (argc == 1) {
        // 进入交互式 REPL
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            fprintf(stderr, "mysh: -c requires a command\n");
            return 2;
        }
        // 处理 argv[2] 中的单条命令后退出
    } else {
        int fd = open(argv[1], O_RDONLY);
        if (fd == -1) {
            perror(argv[1]);
            return 1;
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return 1;
        }
        close(fd);
        // 从重定向后的标准输入进入非交互式 REPL
    }
}
```

- `int main(void)` 明确表示函数不接收参数；`int main()` 在 C 语言中表示参数列表未指定。需要读取命令行参数时，应使用 `int main(int argc, char **argv)`。`argc` 是参数数量，`argv` 是以 `NULL` 结尾的参数数组，其中 `argv[0]` 是程序名。

- `open()`: 打开文件并返回新的文件描述符。其除了接收一个目的文件名参数外, 还接收一个 `oflag` 参数, 以及在 `O_CREAT` flag 存在时还接收一个 `mode` 参数。声明于 `<fcntl.h>` 中。
  - `oflag`: `O_RDONLY` 表示只读; `O_WRONLY` 表示只写; `O_RDWR` 表示读写; `O_TRUNC` 表示打开时截断(即对文件内容进行清空, 否则只会从头开始覆写); `O_APPEND` 表示写入追加到文件末尾; `O_CREAT` 表示文件不存在时创建。
  - `umask`: `umask` 是进程级别的属性, 它的本质是“禁用/屏蔽”某些权限位, 为了防止程序**无意**使用了较为宽泛的权限创建文件。对于程序调用 `open` 传入的 `mdoe`, 内核会先将其和**取反后的umask**进行**按位与**, 然后再按照得到的文件权限创建文件。

- `dup(oldfd)`: 复制文件描述符, 返回当前可用的最小 fd, 使得 oldfd 和 newfd 指向同一个内核**打开的文件描述** , 其指向一个内存中的 inode 节点。位于 `<unistd.h>` 中。

- `dup2(oldfd, newfd)`: 让 `newfd` 指向与 `oldfd` 相同的打开文件描述; 如果 `newfd` 原本已经打开, 会先原子地关闭原引用。位于 `<unistd.h>` 中。

- `close()`: `dup2()` 后原 fd 如果不再需要, 应该立即关闭, 否则可能会造成文件描述符泄漏。位于 `<unistd.h>` 中。

### 1.2 REPL 循环

交互模式下，shell 在每次读取前先打印提示符；脚本模式则不应打印提示符。提示符末尾没有换行符，所以需要主动调用 `fflush()`，避免它仍停留在 stdio 缓冲区中。

由于 `getline()` 会自动分配或扩展输入缓冲区, 一般直接传入**空指针**和**零容量**即可。将 `line` 和 `capacity` 放在循环外，可以在多次读取之间复用同一块缓冲区，并在 REPL 结束时统一释放。如果读取被信号中断，需检查是否为 `EINTR`，再继续读取。

```c
char *line = NULL;
size_t capacity = 0;
int interactive = isatty(STDIN_FILENO);

while (1) {
    if (interactive) {
        printf("mysh> ");
        fflush(stdout);
    }

    while (getline(&line, &capacity, stdin) == -1) {
        if (feof(stdin)) {
            if (interactive) printf("\n");
            free(line);
            return 0;
        } else if (errno == EINTR) {
            continue;
        } else {
            perror("getline");
            free(line);
            return 1;
        }
    }

    line[strcspn(line, "\n")] = '\0';

    // 回显输入行（初版）
    printf("%s\n", line);
}

free(line);
return 0;
```

- `fflush(FILE *stream)` 将 stdio 缓冲区中尚未写出的内容提交到底层文件。由于终端默认是行缓冲, 当提示符没有换行符时，应主动刷新 `stdout`。该函数声明于 `<stdio.h>` 中。

- `getline(char **lineptr, size_t *n, FILE *stream)` 动态读取一行文本。`*lineptr` 指向由它分配或扩容的缓冲区，`*n` 保存该缓冲区当前可容纳的字节数，而不是本次输入长度。返回值是本次读取的字符数，包含行末换行符，但不包含结尾的 `\0`。`getline()` 是 POSIX 接口，声明于 `<stdio.h>` 中。

- `strcspn(const char *s, const char *reject)` 返回 `s` 中首次出现 `reject` 中任意字符的下标；如果没有找到，则返回 `s` 的长度。因此 `line[strcspn(line, "\n")] = '\0'` 可以同时处理“有换行符”和“最后一行无换行符”两种情况。该函数声明于 `<string.h>` 中。

- `perror(const char *s)` 读取当前 `errno`，将前缀 `s`、对应的错误描述和换行符写入标准错误流。它应紧跟在失败调用之后使用，避免其他函数改变 `errno`。效果类似于 `fprintf(stderr, "%s: %s\n", s, strerror(errno)`。该函数声明于 `<stdio.h>` 中。
  - `errno` 表示当前线程的错误状态，声明于 `<errno.h>` 中。它通常由宏提供，不应简单理解为普通全局变量。只有当函数的返回值已明确表示失败，且该函数约定会设置 `errno` 时，它的值才有判断意义；成功调用通常不会清除之前的错误码。
    - `EINTR` 表示阻塞操作被信号中断。是否重试取决于具体操作的语义；对于本节的按行读取，清除流错误标志后重新读取即可。

- `feof(FILE *stream)` 检查流的 EOF 标志。它不能提前预测文件结束，只有当**上一次**读取操作尝试越过文件末尾读取时，该标志才会被设置。该函数声明于 `<stdio.h>` 中。

### 1.3 Makefile

Makefile 的核心是一张由“目标”和“依赖”组成的关系图。`make` 会比较目标文件与依赖文件的修改时间：如果目标不存在，或者任意依赖比目标更新，就执行对应命令。因此，修改 `shell.c` 后只需重新生成 `shell.o`，再重新链接 `mysh`，无需每次从头编译所有文件。

第一阶段只有 `shell.c`，因此 Makefile 中也只列出该文件。后续每增加一个模块，再将它加入 `SOURCES`。这样文档中的 Makefile 与当前阶段的源码始终可以直接编译。

```Makefile
CC = cc
CFLAGS = -Wall -Wextra -Werror -g -O0 -fsanitize=address -fno-omit-frame-pointer
DEPFLAGS = -MMD -MP
LINKFLAGS = -fsanitize=address

TARGET = mysh
SOURCES = shell.c
OBJS = $(SOURCES:.c=.o)
DEPS = $(OBJS:.o=.d)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LINKFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)
```

- `key = value` 会定义递归展开变量，其中的其他变量到真正使用时才展开；`key := value` 会在定义当时立即展开。两者都使用 `$(key)` 或 `${key}` 取值。
  - `$(SOURCES:.c=.o)`: 替换引用，会将 `SOURCES` 中每个单词结尾的 `.c` 替换为 `.o`。

- `target: prerequisites` 定义一条规则，后续命令行必须以 Tab 开头。`make` 默认构建第一个普通目标，也可以如用 `make run` 或 `make clean` 指定目标。命令中的自动变量由 `make` 根据当前规则填入：
  - `$@`：当前规则的目标文件。
  - `$<`：当前规则的第一个依赖文件。
  - `$^`：当前规则的所有依赖文件，重复项会被去除。
  - `%.o: %.c`: 模式规则，能将任意同名 `.c` 文件编译为 `.o` 文件。

- `CFLAGS` 变量中保存以下 C 编译选项:
  - `-Wall -Wextra` 开启**所有常用**以及**额外**警告, 如未使用的变量、参数, 隐式类型转换等等。
  - `-Werror` 将警告当作错误;
  - `-g` 保留调试信息;
  - `-O0` 关闭优化;
  - `-fno-omit-frame-pointer` 保留栈帧指针，使错误堆栈更完整。
  - `-fsanitize=address` 启用 AddressSanitizer 功能; 编译和链接都要使用该选项。
  - `-MMD` 会在编译 `shell.c` 时根据源文件中包含的头文件同时生成 `shell.d`，其内容大致为 `shell.o: shell.c shell.h`。`-MMD` 只记录项目自己的头文件，不把 `<stdio.h>` 这类系统头文件加入依赖；对于普通项目来说，这正是需要的行为。
  - `-MP` 会在 `.d` 文件中为每个头文件再生成一条没有命令的空规则：`shell.h:`。这样在某个头文件被删除或改名、而旧 `.d` 文件尚未更新时，`make` 不会立即因为“找不到生成该头文件的规则”而停止。它并不会帮你生成被删除的头文件；如果源代码仍然 `#include` 该文件，编译依然会正常失败。

- `DEPS = $(OBJS:.o=.d)` 根据目标文件列表得到对应的依赖文件列表。
- `-include $(DEPS)` 会把这些 `.d` 文件当作 Makefile 的一部分读入。前面的 `-` 表示即使文件尚不存在也不报错，因此第一次构建或执行 `make clean` 后仍能正常工作。从第二次构建开始，`make` 就能从 `.d` 文件中知道头文件与 `.o` 之间的关系：只要 `shell.h` 比 `shell.o` 新，就会重新编译 `shell.c`。

- `.PHONY` 将 `all`、`run` 和 `clean` 声明为伪目标，表示它们不对应真实文件。否则当目录中恰好存在名为 `clean` 或 `run` 的文件时，`make` 可能会认为该目标已经是最新的，从而不执行其命令。

## 总结

至此，mysh 已经能够从交互终端、`-c` 参数或脚本文件中取得命令文本，并持续完成读取循环。不过，输入内容仍然只会被回显，还没有被拆分成参数，也不能真正执行命令。下一节将在这个循环中接入最初的 executor，使单条命令能够运行起来。

- `shell.c`：启动模式判断与 REPL 循环。
- `Makefile`：编译、链接、清理与头文件依赖跟踪规则。
