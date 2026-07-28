#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stddef.h>

typedef enum {
    QUOTE_NORMAL,
    QUOTE_SINGLE,
    QUOTE_DOUBLE
} QuoteType;

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_REDIRECT_IN,
    TOKEN_REDIRECT_OUT,
    TOKEN_REDIRECT_APPEND,
    TOKEN_AMP,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    char* value;
} Token;

/*
 * TokenArray MUST be NULL or end by TOKEN_EOF.
 */
typedef struct {
    Token** tokens;
    size_t capacity;
    size_t count;
} TokenArray;

#endif /*TOKENIZER_H*/