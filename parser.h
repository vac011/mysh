#ifndef PARSER_H
#define PARSER_H

#include "tokenizer.h"

typedef struct {
    TokenType type;
    char* target;
} Redirector;


typedef struct Command{
    char** argv;
    Redirector* redirs;
    struct Command* next;
} Command;

typedef struct Commands{
    Command* command;
    int pipeline;
    int background;
} Commands;

#endif /*PARSER_H*/