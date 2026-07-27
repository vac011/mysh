#ifndef BUILTIN_H
#define BUILTIN_H

typedef int (*builtin_fn)(char** argv);

typedef struct {
    char* func_name;
    builtin_fn fn;
} Builtin;

#endif /*BUILTIN_H*/