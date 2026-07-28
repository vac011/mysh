#include "tokenizer.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

void free_tokens(TokenArray* token_array) {
    if (token_array == NULL) return;
    for (size_t i = 0; i < token_array->count; i++) {
        free(token_array->tokens[i]->value);
        free(token_array->tokens[i]);
    }
    free(token_array->tokens);
    free(token_array);
}


// keep the "Who creates, who destroys" principle.
// This function only free the token_array and its contents, NOT free the input token_value when failed.
static TokenArray* push_token(TokenArray* token_array, TokenType token_type, char* token_value) {
    if (token_array == NULL) {
        token_array = malloc(sizeof(TokenArray));
        if (token_array == NULL) return NULL;
        token_array->tokens = malloc(sizeof(Token*) * 2);
        if (token_array->tokens == NULL) {free(token_array); return NULL;}
        token_array->capacity = 2;
        token_array->count = 0;
    }
    if (token_array->count >= token_array->capacity) {
        size_t new_capacity = 2 * token_array->capacity;
        Token** new_tokens = realloc(token_array->tokens, sizeof(Token*) * new_capacity);
        if (new_tokens == NULL) {free_tokens(token_array); return NULL;}
        token_array->tokens = new_tokens;
        token_array->capacity = new_capacity;
    }
    Token* token = malloc(sizeof(Token));
    if (token == NULL) {free_tokens(token_array); return NULL;}
    token->type = token_type;
    token->value = token_value;
    token_array->tokens[token_array->count] = token;
    token_array->count++;
    return token_array;
}


/*
 * Ruturn NULL when the input is NULL or only spaces.
 */
TokenArray* tokenize(const char* input) {
    if (input == NULL) return NULL;

    QuoteType quote_state = QUOTE_NORMAL;
    TokenArray* token_array = NULL;
    const char* end = input;

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
            case '&': {
                end++;
                while (*(end) != '\0') {
                    if (!isspace((unsigned char)*(end))) {
                        fprintf(stderr, "syntax error: '&' not in the end\n");
                        free_tokens(token_array);
                        return NULL;
                    }
                    end++;
                }
                token_type = TOKEN_AMP;
                token_value = strdup("&");
                if (token_value == NULL) {
                    free_tokens(token_array);
                    return NULL;
                }
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
                // end指向完整token的下一位。
                size_t len = end - start;
                token_value = malloc(len + 1);
                if (token_value == NULL) {
                    free_tokens(token_array);
                    return NULL;
                }
                memcpy(token_value, start, len);
                token_value[len] = '\x00';
            }
        }
        token_array = push_token(token_array, token_type, token_value);
        if (token_array == NULL) {
            fprintf(stderr, "fail to push token: %s\n", token_value);
            free(token_value);
            return NULL;
        }
    }

    if (quote_state != QUOTE_NORMAL) {
        const char quote = (quote_state == QUOTE_SINGLE) ? '\'' : '\"';
        fprintf(stderr, "syntax error: uncompleted quote '%c'\n", quote);
        free_tokens(token_array);
        return NULL;
    }

    if (token_array == NULL) return NULL;
    token_array = push_token(token_array, TOKEN_EOF, NULL);
    if (token_array == NULL) {
        fprintf(stderr, "fail to push token EOF\n");
        return NULL;
    }
    return token_array;
}