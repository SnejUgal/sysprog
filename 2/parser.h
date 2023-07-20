#pragma once

#include "command.h"

enum parsing_result {
    PARSING_SUCCESS,
    PARSING_SYNTAX_ERROR,
    PARSING_INCOMPLETE_INPUT,
    PARSING_EMPTY,
};

enum parsing_result parse_command(char* input, struct job_command* command);
