#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "command.h"
#include "parser.h"

bool read_line(char** string, size_t* length, size_t* capacity) {
    bool is_eof = false;

    while (true) {
        int character = getchar();
        if (character != EOF) {
            size_t required_capacity = *length + 2;
            if (required_capacity > *capacity) {
                *string = realloc(*string, sizeof(char) * required_capacity);
                if (*string == NULL) {
                    perror("Failed to allocate memory");
                    exit(127);
                }
                *capacity = required_capacity;
            }

            (*string)[*length] = (char)character;
            (*string)[*length + 1] = '\0';
            ++*length;
        }

        if (character == '\n' || character == EOF) {
            is_eof = character == EOF;
            break;
        }
    }

    return is_eof;
}

int main() {
    struct execution_context context = {
        .last_exit_code = 0, .jobs = NULL, .jobs_count = 0};

    char* input = malloc(sizeof(char));
    size_t input_length = 0;
    size_t input_capacity = 1;

    if (input == NULL) {
        perror("Failed to allocate memory");
        return 127;
    }
    input[0] = '\0';

    bool is_eof = false;

    while (true) {
#ifdef PROMPT
        fprintf(stderr, ">> ");
#endif
        is_eof = read_line(&input, &input_length, &input_capacity);

        struct job_command command;
        while (true) {
            switch (parse_command(input, &command)) {
            case PARSING_SUCCESS: {
                struct execution_result result =
                    execute_job_command(&command, &context);
                context.last_exit_code = result.exit_code;

                free_job_command(&command);
                if (result.should_terminate) {
                    goto exit;
                }
                break;
            }
            case PARSING_EMPTY:
                break;
            case PARSING_INCOMPLETE_INPUT:
#ifdef PROMPT
                fprintf(stderr, ".. ");
#endif
                read_line(&input, &input_length, &input_capacity);
                continue;
            case PARSING_SYNTAX_ERROR:
                fprintf(stderr, "sush: syntax error in command\n");
                context.last_exit_code = 127;
                break;
            }

            input[0] = '\0';
            input_length = 0;
            break;
        }

        for (size_t i = 0; i < context.jobs_count; ++i) {
            if (waitpid(context.jobs[i], NULL, WNOHANG) > 0) {
                context.jobs[i] = context.jobs[context.jobs_count - 1];
                --context.jobs_count;
#ifdef PROMPT
                fprintf(stderr, "Job completed\n");
#endif
            }
        }

        if (is_eof) {
            break;
        }
    }

exit:
#ifdef PROMPT
    printf("exit\n");
#endif
    free(context.jobs);
    free(input);
    return context.last_exit_code;
}
