CC = gcc
# -Wall: 所有**常见**警告信息
# -Wextra: 额外的警告信息
# -Werror: 将警告视为错误
# -g: 生成调试信息
# -O0: 关闭优化
# -D_GNU_SOURCE: 定义 _GNU_SOURCE 宏，以启用 GNU 扩展
# -MMD: 生成依赖文件（.d 文件），用于自动化构建
# -MP: 为每个依赖文件生成一个伪目标，以避免某个头文件删除时出现错误
CFLAGS = -Wall -Wextra -Werror -g -O0 -D_GNU_SOURCE -MMD -MP -fsanitize=address -fno-omit-frame-pointer
LDFLAGS = -fsanitize=address
TARGET = mysh
SRC_DIR = src
SOURCES = $(addprefix $(SRC_DIR)/, shell.c executor.c tokenizer.c parser.c builtin.c expander.c controller.c job.c)
OBJS = $(SOURCES:.c=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)
