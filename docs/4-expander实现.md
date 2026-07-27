# 4. expander 实现

## 本节背景

上一阶段已经能够识别普通单词、重定向符和管道符，但 tokenizer 仍然只根据空白与元字符划分 WORD。引号和反斜杠尚未参与词法分析，因此 `echo "hello world"` 会被拆成两个参数，`echo 'a|b'` 中的 `|` 也会被误认为管道符；此外，变量 `$HOME` 仍然只是一串普通字符，无法根据 shell 状态和环境变量得到真正的参数内容。

引号既影响一个 WORD 在哪里结束，也影响 WORD 内部的字符是否允许展开，所以这部分不能完全交给 tokenizer 或 parser 单独完成。

tokenizer 仍然只负责将字符串拆分为一个个 token, 但需要在扫描时理解引号状态，保证引号内的空白和元字符不会切断 WORD，且仍然保留原始的引号与反斜杠提供给 expander；

随后 expander 再根据这些信息完成变量展开、转义处理和引号删除，最后把处理后的 TokenArray 交给 parser。

## 本节目标

- 为 tokenizer 引入普通、单引号和双引号三种状态，使一个 WORD 可以由普通字符和多个引号片段共同组成。
- 实现环境变量 `$VAR`、上一条命令状态 `$?` 和当前 shell PID `$$` 的展开；
- 支持反斜杠功能，并在展开完成后删除语法性的引号和反斜杠；

## 实现

### 4.1 为 tokenizer 引入引号状态

引号不是 WORD 的状态，而是**字符级**的状态切换。例如 `ab" cd"'ef'` 最终仍然是一个参数 `ab cdef`。我们使用 `QuoteType` 表示 tokenizer 当前处于普通文本、单引号还是双引号中。

- 结束 WORD 的唯一条件是**在普通状态下**遇到**空白符**或 `|`、`<`、`>`**三类元字符**;
- 单引号状态下，除下一个单引号外的所有字符都是普通内容；
- 双引号状态下，除下一个双引号外的所有字符都是普通内容, 但反斜杠可以使后一个双引号字符失去原本的状态切换作用。

tokenizer 在这一阶段**只借助引号状态确定 WORD 的边界**，并不立即删除引号或展开变量。这样 expander 仍然能够**知道每个字符原本处于哪一种引号环境**。

```c
// tokenizer.h
typedef enum {
    QUOTE_NORMAL,
    QUOTE_SINGLE,
    QUOTE_DOUBLE
} QuoteType;
```

```c
// tokenizer.c
while (*end) {
    if (isspace((unsigned char)*end)) {end++; continue;}

    TokenType token_type;
    char* token_value = NULL;
    switch (*end) {
        case '|': {
            token_type = TOKEN_PIPE;
            token_value = strdup("|");
            if (token_value == NULL) {
                free_tokens(token_array);
                return NULL;
            }
            end++;
            break;
        }
        case '<': {
            token_type = TOKEN_REDIRECT_IN;
            token_value = strdup("<");
            if (token_value == NULL) {
                free_tokens(token_array);
                return NULL;
            }
            end++;
            break;
        }
        case '>': {
            if (*(end+1) && *(end+1) == '>') {
                token_type = TOKEN_REDIRECT_APPEND;
                token_value = strdup(">>");
                if (token_value == NULL) {
                    free_tokens(token_array);
                    return NULL;
                }
                end++;
            } else {
                token_type = TOKEN_REDIRECT_OUT;
                token_value = strdup(">");
                if (token_value == NULL) {
                    free_tokens(token_array);
                    return NULL;
                }
            }
            end++;
            break;
        }
        default: {
            token_type = TOKEN_WORD;
            const char* start = end;
            while (*(end) != '\0') {
                const unsigned char nc = *(end);
                if (quote_state == QUOTE_SINGLE) {
                    if (nc == '\'') {
                        quote_state = QUOTE_NORMAL;
                    }
                } else if (quote_state == QUOTE_DOUBLE) {
                    if (nc == '\\') {
                        end++;
                        if (*(end) == '\0') {
                            fprintf(stderr, "syntax error: uncompleted escape '\\'\n");
                            free_tokens(token_array);
                            return NULL;
                        }
                    } else if (nc == '\"') {
                        quote_state = QUOTE_NORMAL;
                    }
                } else {
                    if (nc == '\\') {
                        end++;
                        if (*(end) == '\0') {
                            fprintf(stderr, "syntax error: uncompleted escape '\\'\n");
                            free_tokens(token_array);
                            return NULL;
                        }
                    } else if (nc == '\'') {
                        quote_state = QUOTE_SINGLE;
                    } else if (nc == '\"') {
                        quote_state = QUOTE_DOUBLE;
                    } else if (isspace(nc) || nc == '|' || nc == '>' || nc == '<' || nc == '&') {
                        // 唯一结束分词的情形: 处于NORMAL态且遇到空格或特殊符号。
                        break;
                    }
                }
                end++;
            }
            // 分配 token
        }
    }
}
```

扫描到字符串末尾后，如果状态仍然不是 `QUOTE_NORMAL`，就说明输入中存在未闭合引号。该检查属于 tokenizer，因为只有 tokenizer 掌握整行扫描过程中引号是否配对。

```c
if (quote_state != QUOTE_NORMAL) {
    char quote = quote_state == QUOTE_SINGLE ? '\'' : '"';
    fprintf(stderr,
            "syntax error: uncompleted quote '%c'\n", quote);
    free_tokens(token_array);
    return NULL;
}
```

### 4.2 expander 实现

expander 对 TokenArray 中单每个 WORD 类型的 Token 进行逐字符的扫描, 根据字符所处的**引号状态**进行处理, 然后动态增长新字符串, 最后**原位替换** Token 中的 `value`, 并释放旧字符串。

- 关于引号处理:
  - 普通状态下:
    - 遇到引号进行切换状态，引号本身不写入结果;
    - 遇到 `$` 触发变量展开; 遇到 `\` 转义符无条件复制下一个字符, 转义符本身不写入结果;
    - 其余字符直接复制。
  - 单引号状态下:
    - 遇到单引号即退出单引号状态, 单引号本身不写入结果;
    - 除单引号外其他所有字符都直接复制。
  - 双引号状态下:
    - 遇到双引号进行状态切换, 双引号本身不写入结果;
    - 遇到 `$` 触发变量展开;
    - 遇到 `\` 转义符只对 `\`、`$` 、 `"` 和换行四种符号起转义作用, 删除 `\` 转义符后将下一个字符写入结果; 除此之外其他字符前的 `\`杠作为普通字符保留。

- 关于 `$` 符处理:
  - `$?` 展开为上一条命令的退出状态;
  - `$$` 展开为当前 shell 的 PID。
  - `$VAR` 展开为环境变量 `VAR` 的值，未定义的环境变量则展开为空字符串。普通环境变量名以字母或下划线开头，后续可以包含字母、数字和下划线。变量名采用最长匹配。例如 `$HOMEabc` 会被识别为变量 `HOMEabc`，而不是变量 `HOME` 后接字符串 `abc`。当前阶段不实现 `${HOME}abc` 这种使用花括号明确边界的形式。
  - 除此之外，如果 `$` 后面不是当前支持的变量形式，例如字符串末尾的单独 `$` 或 `$9`，就将 `$` 本身作为普通字符保留。

```c
// expander.c（核心结构）

// in-place expand.
// return 0 if successfully expand, or return -1 if failed.
int expand_token(TokenArray* token_array) {
    if (token_array == NULL) return -1;
    for (size_t i = 0; i < token_array->count; i++) {
        if (token_array->tokens[i]->type != TOKEN_WORD) continue;

        char* old_value = token_array->tokens[i]->value;
        char* new_value = NULL;
        size_t count = 0;
        size_t capacity = 0;

        QuoteType quote_state = QUOTE_NORMAL;
        size_t j = 0;
        while (old_value[j] != '\0') {  
            const char ch = old_value[j];
            if (quote_state == QUOTE_SINGLE) {
                if (ch == '\'') {
                    quote_state = QUOTE_NORMAL;
                    j++;
                    continue;
                }
            } else if (quote_state == QUOTE_DOUBLE) {
                if (ch == '\\') {
                    j++;
                    const char escape_char = old_value[j];
                    char escape_string[3] = "\0\0\0";
                    switch (escape_char) {
                        case '\\': {
                            escape_string[0] = '\\';
                            break;
                        }
                        case '$': {
                            escape_string[0] = '$';
                            break;
                        }
                        case '"': {
                            escape_string[0] = '"';
                            break;
                        }
                        default: {
                            escape_string[0] = '\\';
                            escape_string[1] = escape_char;
                        }
                    }
                    char* tmp = copy_to_new(new_value, &count, &capacity, escape_string, NULL);
                    if (tmp == NULL) {
                        free(new_value);
                        return -1;
                    }
                    new_value = tmp;
                    j++;
                    continue;
                } else if (ch == '$') {
                    char* tmp = expand_var(new_value, &count, &capacity, old_value, &j);
                    if (tmp == NULL) {
                        free(new_value);
                        return -1;
                    }
                    new_value = tmp;
                    continue;
                } else if (ch == '\"') {
                    quote_state = QUOTE_NORMAL;
                    j++;
                    continue;
                }
            } else {
                if (ch == '\\') {
                    j++;
                    char* tmp = copy_to_new(new_value, &count, &capacity,old_value + j, old_value + j + 1);
                    if (tmp == NULL) {
                        free(new_value);
                        return -1;
                    }
                    new_value = tmp;
                    j++;
                    continue;
                } else if (ch == '$') {
                    char* tmp = expand_var(new_value, &count, &capacity, old_value, &j);
                    if (tmp == NULL) {
                        free(new_value);
                        return -1;
                    }
                    new_value = tmp;
                    continue;
                } else if (ch == '\'') {
                    quote_state = QUOTE_SINGLE;
                    j++;
                    continue;
                } else if (ch == '\"') {
                    quote_state = QUOTE_DOUBLE;
                    j++;
                    continue;
                }
            }
            char* tmp = copy_to_new(new_value, &count, &capacity, old_value + j, old_value + j + 1);
            if (tmp == NULL) {
                free(new_value);
                return -1;
            }
            new_value = tmp;
            j++;
        }
        // 置0兜底
        char* tmp = copy_to_new(new_value, &count, &capacity, NULL, NULL);
        if (tmp == NULL) {
            free(new_value);
            return -1;
        }
        new_value = tmp;
        free(old_value);
        token_array->tokens[i]->value = new_value;
    }
    return 0;
}
```

反斜杠在 shell 中是语法字符，不是 C 字符串中的转义序列。用户输入的 `\n` 不会自动变成字节 `0x0a`。它只会**抑制**下一个字符可能具有的语法含义, 普通状态下它只会无条件保留下一个字符并删除自身; 而在双引号中, 除了四个仍在双引号中保持特殊含义的字符可以被抑制, 其余情况下反斜杠会与下一个字符一起当作普通字符被保留。

当前实现不会对未加引号的变量结果再次按 **IFS** 分词。因此即使某个环境变量的值包含空格，`$VAR` 仍然只产生一个参数；它也不会继续进行通配符、花括号或命令替换。这里实现的是 shell 展开机制的核心子集，而不是 Bash 的完整展开顺序。

## 总结

完成本阶段后，mysh 已经能够在引号环境中正确识别 WORD，并根据普通、单引号和双引号状态完成变量展开、反斜杠处理与引号删除。输入中的一个参数可以由多个不同引号环境的片段拼接而成，空引号也能够保留下一个长度为零的真实参数。

- `tokenizer.h/.c`：增加引号状态，在不破坏原始文本的前提下识别正确的 WORD 边界，并报告未闭合引号和转义；
- `expander.c`：构造动态输出字符串，展开 `$VAR`、`$?` 和 `$$`，处理反斜杠并删除语法性引号；
- `executer.c`：在 tokenizer 与 parser 之间调用 expander，使 parser 借用展开后的 Token 字符串；

至此，命令从原始文本到 Token、展开结果、Command 结构再到进程执行的主体链路已经建立。下一阶段将在这条执行链路之上引入后台执行、进程组与终端所有权，开始实现作业控制。
