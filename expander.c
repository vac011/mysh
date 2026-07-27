#include "shell.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static char* copy_to_new(char* new_value, size_t* count, size_t* capacity, const char* old_start, char* old_end) {
    size_t copy_len;
    if (old_start == NULL) {
        copy_len = 0;
    } else if (old_end == NULL) {
        copy_len = strlen(old_start);
    } else {
        copy_len = (old_end - old_start);
    }
    if (*count + copy_len + 1 > *capacity) {
        *capacity = (*count + copy_len + 1) * 2;
        char* tmp = realloc(new_value, *capacity);
        if (tmp == NULL) return NULL;
        new_value = tmp;
    }
    if (copy_len > 0) {
        memcpy(new_value + *count, old_start, copy_len);
    }
    *count += copy_len;
    new_value[*count] = '\0';
    return new_value;
}


// 目前只有env环境变量的展开
// input: &new_value; output: new_index
static char* expand_var(char* new_value, size_t* count, size_t* capacity, char* old_value, size_t* index) {
    // index points to '$'
    size_t end = *index + 1;
    const char nc = old_value[end];
    if (nc != '\0') {
        if (nc == '?') {
            char exit_code[4];
            snprintf(exit_code, 4, "%u", (unsigned char)shell.last_status);
            char* tmp = copy_to_new(new_value, count, capacity, exit_code, NULL);
            if (tmp == NULL) return NULL;
            new_value = tmp;
            *index = end + 1;
            return new_value;
        } else if (nc == '$') {
            pid_t pid = getpid();
            size_t len = snprintf(NULL, 0, "%d", pid);
            char* pid_c = malloc(len + 1);
            if (pid_c == NULL) return NULL;
            snprintf(pid_c, len + 1, "%d", pid);
            char* tmp = copy_to_new(new_value, count, capacity, pid_c, NULL);
            free(pid_c);
            if (tmp == NULL) return NULL;
            new_value = tmp;
            *index = end + 1;
            return new_value;
        } else if (isalpha((unsigned char)old_value[end]) || old_value[end] == '_') {
            end++;
            while (old_value[end] != '\0' && (isalnum((unsigned char)old_value[end]) || old_value[end] == '_')) end++;
        }
    }
    size_t len = (end - *index - 1);
    if (len == 0) {
        char* tmp = copy_to_new(new_value, count, capacity, old_value + *index, old_value + end);
        if (tmp == NULL) return NULL;
        new_value = tmp;
        *index = end;
        return new_value;
    }
    char* env_key = malloc(len + 1);
    if (env_key == NULL) return NULL;
    memcpy(env_key, old_value + *index + 1, len);
    env_key[len] = '\0';
    char* env_value = getenv(env_key);
    // 无需判断env_value是否有值, copy函数保证其正确性。
    char* tmp = copy_to_new(new_value, count, capacity, env_value, NULL);
    if (tmp == NULL) {
        free(env_key);
        return NULL;
    }
    new_value = tmp;
    free(env_key);
    *index = end;
    return new_value;
}


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