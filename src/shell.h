#ifndef SHELL_H
#define SHELL_H

#include "controller.h"
extern ShellState shell;
int give_terminal(Job* job);
int take_terminal(Job* job);

int child_restore_signals();
int shell_ignore_signals();
int controller_init(int interactive);

#include "tokenizer.h"
void free_tokens(TokenArray* token_array);
TokenArray* tokenize(const char* input);

int expand_token(TokenArray* token_array);

#include "parser.h"
Commands* parse_commands(TokenArray* token_array);
void free_commands(Commands* commands);

#include "builtin.h"
builtin_fn lookup_builtin(char* func_name);

int execute_line(const char* line);

#include "job.h"
void free_job(Job* job);
Job* create_job(int background, const char* commands_string);
int add_process(Job* job, pid_t pid);
int job_add(Job* job);
void job_make_current(Job* job);
void job_mark_running(Job* job);
int wait_foreground_job(Job* job);
void kill_wait_pid(pid_t pid);
void kill_wait_job(Job* job);
void check_jobs();

#endif /* SHELL_H */
