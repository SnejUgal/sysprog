#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "command.h"

struct pipes {
    bool should_pipe_input;
    bool should_pipe_output;
    int input_fd;
    int output_fd;
};

enum builtin_result {
    NOT_BUILTIN,
    BUILTIN_EXECUTED,
    BUILTIN_EXIT,
};

void execute_cd(struct simple_command* command, int* exit_code) {
    if (command->words_count > 2) {
        fprintf(stderr, "sush: cd: too many arguments\n");
        *exit_code = 1;
    }

    char* new_cwd = NULL;
    if (command->words_count == 1) {
        new_cwd = getenv("HOME");
    } else {
        new_cwd = command->words[1];
    }
    if (new_cwd == NULL) {
        *exit_code = 0;
        return;
    }

    if (chdir(new_cwd) < 0) {
        perror("sush: cd");
        *exit_code = 1;
        return;
    }

    *exit_code = 0;
}

enum builtin_result execute_exit(struct simple_command* command,
                                 struct execution_context* context,
                                 int* exit_code) {
    if (command->words_count > 2) {
        fprintf(stderr, "sush: exit: too many arguments\n");
        *exit_code = 1;
        return BUILTIN_EXECUTED;
    }

    if (command->words_count == 2) {
        int length;
        int result = sscanf(command->words[1], "%d%n", exit_code, &length);
        if (result < 1 || (size_t)length < strlen(command->words[1])) {
            fprintf(stderr, "sush: exit: invalid number: %s\n",
                    command->words[1]);

            *exit_code = 1;
            return BUILTIN_EXECUTED;
        }
    } else {
        *exit_code = context->last_exit_code;
    }

    return BUILTIN_EXIT;
}

enum builtin_result execute_builtin_command(struct simple_command* command,
                                            struct execution_context* context,
                                            int* exit_code) {
    if (strcmp(command->words[0], "cd") == 0) {
        execute_cd(command, exit_code);
        return BUILTIN_EXECUTED;
    }

    if (strcmp(command->words[0], "exit") == 0) {
        return execute_exit(command, context, exit_code);
    }

    return NOT_BUILTIN;
}

int execute_simple_command(struct simple_command* command,
                           struct execution_context* context,
                           struct pipes* pipes) {
    int exit_code;
    switch (execute_builtin_command(command, context, &exit_code)) {
    case NOT_BUILTIN:
        break;
    case BUILTIN_EXIT:
    case BUILTIN_EXECUTED:
        return exit_code;
    }

    if (command->input_file != NULL) {
        int fd = open(command->input_file, O_RDONLY);
        if (fd < 0 || dup2(fd, 0) < 0 || close(fd) < 0) {
            perror("sush: failed to redirect input");
            return 127;
        }
    }

    if (pipes->should_pipe_input) {
        if (dup2(pipes->input_fd, 0) < 0 || close(pipes->input_fd) < 0) {
            perror("sush: failed to redirect input");
            return 127;
        }
    }

    if (command->output_file != NULL) {
        int flags = O_WRONLY | O_CREAT;
        if (command->output_mode == OUTPUT_APPEND) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }

        int fd = open(command->output_file, flags, 0664);
        if (fd < 0 || dup2(fd, 1) < 1 || close(fd) < 0) {
            perror("sush: failed to redirect output");
            return 127;
        }
    }

    if (pipes->should_pipe_output) {
        if (dup2(pipes->output_fd, 1) < 0 || close(pipes->output_fd) < 0) {
            perror("sush: failed to redirect input");
            return 127;
        }
    }

    execvp(command->words[0], command->words);
    perror("sush: failed to execute command");
    return 127;
}

void free_simple_command(struct simple_command* command) {
    free(command->input_file);
    command->input_file = NULL;

    free(command->output_file);
    command->output_file = NULL;

    for (size_t i = 0; i < command->words_count; ++i) {
        free(command->words[i]);
    }
    command->words_count = 0;

    free(command->words);
    command->words = NULL;
}

struct execution_result execute_pipeline(struct pipeline* pipeline,
                                         struct execution_context* context) {
    if (pipeline->commands_count == 1) {
        int exit_code;
        enum builtin_result builtin_result = execute_builtin_command(
            &pipeline->commands[0], context, &exit_code);

        if (builtin_result != NOT_BUILTIN) {
            struct execution_result result = {
                .exit_code = exit_code,
                .should_terminate = builtin_result == BUILTIN_EXIT};
            return result;
        }
    }

    pid_t* children = malloc(sizeof(pid_t) * pipeline->commands_count);
    if (children == NULL) {
        perror("Failed to allocate memory");
        exit(127);
    }

    int previous_read_end;
    for (size_t i = 0; i < pipeline->commands_count; ++i) {
        struct pipes this_pipes = {
            .should_pipe_input = i > 0,
            .should_pipe_output = i < pipeline->commands_count - 1,
        };
        if (this_pipes.should_pipe_input) {
            this_pipes.input_fd = previous_read_end;
        }

        int next_read_end;
        if (this_pipes.should_pipe_output) {
            int pipe_fds[2];
            if (pipe(pipe_fds) < 0) {
                perror("Failed to create a pipe");
                exit(127);
            }
            next_read_end = pipe_fds[0];
            this_pipes.output_fd = pipe_fds[1];
        }

        pid_t child = fork();
        if (child < 0) {
            perror("sush: failed to fork");
            exit(127);
        }
        if (child == 0) {
            if (this_pipes.should_pipe_output) {
                close(next_read_end);
            }
            exit(execute_simple_command(&pipeline->commands[i], context,
                                        &this_pipes));
        }

        children[i] = child;
        if (this_pipes.should_pipe_input) {
            close(this_pipes.input_fd);
        }
        if (this_pipes.should_pipe_output) {
            close(this_pipes.output_fd);
            previous_read_end = next_read_end;
        }
    }

    int exit_code;
    for (size_t i = 0; i < pipeline->commands_count; ++i) {
        int status;
        waitpid(children[i], &status, 0);
        if (i < pipeline->commands_count - 1) {
            // don't care about exit code of non-final command
            continue;
        }

        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        } else {
            exit_code = WTERMSIG(status);
        }
    }

    free(children);
    struct execution_result result = {.exit_code = exit_code,
                                      .should_terminate = false};
    return result;
}

void free_pipeline(struct pipeline* pipeline) {
    for (size_t i = 0; i < pipeline->commands_count; ++i) {
        free_simple_command(&pipeline->commands[i]);
    }
    pipeline->commands_count = 0;

    free(pipeline->commands);
    pipeline->commands = NULL;
}

struct execution_result
execute_boolean_command(struct boolean_command* command,
                        struct execution_context* context) {
    struct execution_result result =
        execute_pipeline(&command->pipeline, context);
    if (result.should_terminate || command->next == NULL) {
        return result;
    }

    struct boolean_command* current = command;
    while (current->next != NULL) {
        if ((current->tag == AND_COMMAND && result.exit_code == 0) ||
            (current->tag == OR_COMMAND && result.exit_code != 0)) {
            return execute_boolean_command(current->next, context);
        }

        current = current->next;
    }

    return result;
}

void free_boolean_command(struct boolean_command* command) {
    if (command->next != NULL) {
        free_boolean_command(command->next);
        free(command->next);
        command->next = NULL;
    }
    free_pipeline(&command->pipeline);
}

struct execution_result execute_job_command(struct job_command* job,
                                            struct execution_context* context) {
    struct execution_result result;
    if (job->tag == JOB_FOREGROUND) {
        result = execute_boolean_command(&job->command, context);
    } else {
        pid_t child = fork();
        if (child < 0) {
            perror("sush: failed to fork");
            exit(127);
        }
        if (child == 0) {
            struct execution_result result =
                execute_boolean_command(&job->command, context);
            exit(result.exit_code);
        }

        context->jobs =
            realloc(context->jobs, sizeof(pid_t) * (context->jobs_count + 1));
        if (context->jobs == NULL) {
            perror("Failed to allocate memory");
            exit(127);
        }
        context->jobs[context->jobs_count] = child;
        ++context->jobs_count;
#ifdef PROMPT
        fprintf(stderr, "Job started\n");
#endif

        result.exit_code = 0;
        result.should_terminate = false;
    }

    if (result.should_terminate || job->next == NULL) {
        return result;
    }

    return execute_job_command(job->next, context);
}

void free_job_command(struct job_command* job) {
    if (job->next != NULL) {
        free_job_command(job->next);
        free(job->next);
        job->next = NULL;
    }
    free_boolean_command(&job->command);
}
