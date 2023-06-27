#pragma once

#include <stdlib.h>

struct simple_command {
    char** words;
    size_t words_count;
    char* input_file;
    char* output_file;
    enum {
        OUTPUT_OVERWRITE,
        OUTPUT_APPEND,
    } output_mode;
};

void free_simple_command(struct simple_command* command);

struct pipeline {
    struct simple_command* commands;
    size_t commands_count;
};

void free_pipeline(struct pipeline* pipeline);

struct boolean_command {
    struct pipeline pipeline;
    enum {
        AND_COMMAND,
        OR_COMMAND,
    } tag;
    struct boolean_command* next;
};

void free_boolean_command(struct boolean_command* command);

struct job_command {
    struct boolean_command command;
    enum {
        JOB_FOREGROUND,
        JOB_BACKGROUND,
    } tag;
    struct job_command* next;
};

struct execution_context {
    int last_exit_code;
    pid_t* jobs;
    size_t jobs_count;
};

struct execution_result {
    int exit_code;
    bool should_terminate;
};

struct execution_result execute_job_command(struct job_command* job, struct execution_context* context);
void free_job_command(struct job_command* job);
