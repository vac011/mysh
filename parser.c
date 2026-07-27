#include "parser.h"
#include "stdlib.h"
#include "stdio.h"

void free_commands(Commands* commands) {
    if (commands == NULL) return;
    Command* p = commands->command;
    while (p != NULL) {
        free(p->argv);
        free(p->redirs);
        Command* q = p->next;
        free(p);
        p = q;
    }
    free(commands);
}

/* 
 * BORROW string argv and redir target from token_array, make sure the lifetime of token_array longer than these.
 * Support NULLCMD(bare redirect) to creat file.
 * Return NULL if there's no both COMMAND and REDIRECTOR
 */
Commands* parse_commands(TokenArray* token_array) {
    if (token_array == NULL) return NULL;
    Commands* commands = malloc(sizeof(Commands));
    if (commands == NULL) return NULL;
    Command* current_command = malloc(sizeof(Command));
    if (current_command == NULL) {
        free_commands(commands);
        return NULL;
    }
    commands->command = current_command;
    commands->pipeline = 0;
    commands->background = 0;
    current_command->argv = NULL;
    current_command->redirs = NULL;
    current_command->next = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t redir_count = 0;
    size_t redir_capacity = 0;
    for (size_t i = 0; i < token_array->count; i++) {
        TokenType type = token_array->tokens[i]->type;
        if (type == TOKEN_WORD) {
            if (count + 1 >= capacity) {
                capacity = (capacity == 0) ? 2 : capacity * 2;
                char** new_argv = realloc(current_command->argv, sizeof(char*) * capacity);
                if (new_argv == NULL) goto failed;
                current_command->argv = new_argv;
            }
            current_command->argv[count] = token_array->tokens[i]->value;
            count++;
            current_command->argv[count] = NULL;
        } else if (type == TOKEN_REDIRECT_APPEND || type == TOKEN_REDIRECT_IN || type == TOKEN_REDIRECT_OUT) {
            TokenType next_type = token_array->tokens[i+1]->type;
            char* next_value = token_array->tokens[i+1]->value;
            if (next_type != TOKEN_WORD) {
                fprintf(stderr, "syntax error near the token '%s'\n", token_array->tokens[i]->value);
                goto failed;
            }
            if (redir_count + 1 >= redir_capacity) {
                redir_capacity = (redir_capacity == 0) ? 2 : redir_capacity * 2;
                Redirector* new_redirs = realloc(current_command->redirs, sizeof(Redirector) * redir_capacity);
                if (new_redirs == NULL) goto failed;
                current_command->redirs = new_redirs;
            }
            current_command->redirs[redir_count].type = type;
            current_command->redirs[redir_count].target = next_value;
            i++;
            redir_count++;
            current_command->redirs[redir_count].type = TOKEN_EOF;
        } else if (type == TOKEN_PIPE) {
            if (current_command->argv == NULL && current_command->redirs == NULL) {
                fprintf(stderr, "syntax error near the token '|'\n");
                goto failed;
            }
            commands->pipeline = 1;
            current_command->next = malloc(sizeof(Command));
            if (current_command->next == NULL) goto failed;
            current_command = current_command->next;
            current_command->argv = NULL;
            current_command->redirs = NULL;
            current_command->next = NULL;
            count = 0;
            capacity = 0;
            redir_count = 0;
            redir_capacity = 0;
        } else if (type == TOKEN_AMP) {
            // & 支持 NULLCMD 形式
            // 前一个 token 不能直接为重定向符由重定向分支保证
            // 前一个 token 不能直接为管道符由结束分支保证
            // 下一个 token 必须是 TOKEN_EOF 由 tokenizer 保证
            commands->background = 1;   
        } else {
            // TOKEN_EOF
            if (current_command->argv == NULL && current_command->redirs == NULL) {
                fprintf(stderr, "syntax error: uncompleted command\n");
                goto failed;
            }
        }
    }

    return commands;

failed:
    free_commands(commands);
    return NULL;
}