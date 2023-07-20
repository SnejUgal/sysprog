#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "command.h"
#include "parser.h"

void ensure_string(char** string) {
    if (*string != NULL) {
        return;
    }

    *string = malloc(sizeof(char));
    if (*string == NULL) {
        perror("Failed to allocate a string");
        exit(127);
    }
    (*string)[0] = '\0';
}

void push_character(char** string, size_t* size, char character) {
    *string = realloc(*string, sizeof(char) * (*size + 2));
    if (*string == NULL) {
        perror("Failed to allocate a string");
        exit(127);
    }
    (*string)[*size] = character;
    (*string)[*size + 1] = '\0';
    ++*size;
}

char peek_character(char** input) { return **input; }
void advance(char** input) { ++*input; }

bool is_line_whitespace(char character) {
    return character != '\n' && isspace(character);
}

void skip_whitespace(char** input) {
    while (is_line_whitespace(peek_character(input))) {
        advance(input);
    }
}

void skip_non_whitespace(char** input) {
    while (true) {
        char character = peek_character(input);
        if (character == '\0' || is_line_whitespace(character)) {
            break;
        }
        advance(input);
    }
}

void skip_comment(char** input) {
    while (true) {
        char character = peek_character(input);
        if (character == '\n' || character == '\0') {
            break;
        }
        advance(input);
    }
}

struct token {
    enum {
        TOKEN_WORD,
        TOKEN_REDIRECT_INPUT,
        TOKEN_REDIRECT_OUTPUT,
        TOKEN_APPEND_OUTPUT,
        TOKEN_PIPE,
        TOKEN_BACKGROUND,
        TOKEN_AND,
        TOKEN_OR,
        TOKEN_SEMICOLON,
        TOKEN_NEWLINE,
    } tag;
    char* word;
};

enum parsing_result parse_word(char** input, char** word) {
    size_t word_size = 0;
    *word = NULL;

    enum {
        ESCAPING_NONE,
        ESCAPING_BACKSLASH,
        ESCAPING_SINGLE_QUOTE,
        ESCAPING_DOUBLE_QUOTE,
        ESCAPING_DOUBLE_QUOTE_BACKSLASH,
    } escaping = ESCAPING_NONE;

    char character;
    while ((character = peek_character(input)) != '\0') {
        switch (escaping) {
        case ESCAPING_BACKSLASH:
            if (character != '\n') {
                push_character(word, &word_size, character);
            }
            escaping = ESCAPING_NONE;
            break;

        case ESCAPING_SINGLE_QUOTE:
            if (character == '\'') {
                escaping = ESCAPING_NONE;
            } else {
                push_character(word, &word_size, character);
            }
            break;

        case ESCAPING_DOUBLE_QUOTE_BACKSLASH:
            if (character != '"' && character != '\\' && character != '\n') {
                push_character(word, &word_size, '\\');
            }
            push_character(word, &word_size, character);
            escaping = ESCAPING_DOUBLE_QUOTE;
            break;

        case ESCAPING_DOUBLE_QUOTE:
            // I slightly deviate from Bash's behavior here: Bash interprets '$'
            // and '`', but here they are treated as usual characters.
            if (character == '\\') {
                escaping = ESCAPING_DOUBLE_QUOTE_BACKSLASH;
            } else if (character == '"') {
                escaping = ESCAPING_NONE;
            } else {
                push_character(word, &word_size, character);
            }
            break;

        case ESCAPING_NONE:
            switch (character) {
            case '\\':
                escaping = ESCAPING_BACKSLASH;
                break;
            case '\'':
                ensure_string(word);
                escaping = ESCAPING_SINGLE_QUOTE;
                break;
            case '"':
                ensure_string(word);
                escaping = ESCAPING_DOUBLE_QUOTE;
                break;
            case '>':
            case '<':
            case '|':
            case '&':
            case ';':
            case '\n':
                // This is the end of the word.
                goto end;
            default:
                if (is_line_whitespace(character)) {
                    goto end;
                }
                push_character(word, &word_size, character);
            }
        }

        advance(input);
    }

end:
    if (escaping == ESCAPING_NONE) {
        return PARSING_SUCCESS;
    }

    free(*word);
    *word = NULL;

    if (character == '\0') {
        return PARSING_INCOMPLETE_INPUT;
    }

    return PARSING_SYNTAX_ERROR;
}

bool is_token(char* input, char* token) {
    return strncmp(input, token, strlen(token)) == 0;
}

void skip_token(char** input, char* token) {
    *input += strlen(token);
    skip_whitespace(input);
}

enum parsing_result parse_token(char** input, struct token* token) {
    switch (peek_character(input)) {
    case '\0':
        return PARSING_EMPTY;
    case '#':
        skip_comment(input);
        return parse_token(input, token);
    }

    if (is_token(*input, "<")) {
        token->tag = TOKEN_REDIRECT_INPUT;
        skip_token(input, "<");
    } else if (is_token(*input, ">>")) {
        token->tag = TOKEN_APPEND_OUTPUT;
        skip_token(input, ">>");
    } else if (is_token(*input, ">")) {
        token->tag = TOKEN_REDIRECT_OUTPUT;
        skip_token(input, ">");
    } else if (is_token(*input, "||")) {
        token->tag = TOKEN_OR;
        skip_token(input, "||");
    } else if (is_token(*input, "|")) {
        token->tag = TOKEN_PIPE;
        skip_token(input, "|");
    } else if (is_token(*input, "&&")) {
        token->tag = TOKEN_AND;
        skip_token(input, "&&");
    } else if (is_token(*input, "&")) {
        token->tag = TOKEN_BACKGROUND;
        skip_token(input, "&");
    } else if (is_token(*input, ";")) {
        token->tag = TOKEN_SEMICOLON;
        skip_token(input, ";");
    } else if (is_token(*input, "\n")) {
        token->tag = TOKEN_NEWLINE;
        skip_token(input, "\n");
    } else {
        enum parsing_result result = parse_word(input, &token->word);
        if (result != PARSING_SUCCESS) {
            return result;
        }

        token->tag = TOKEN_WORD;
        skip_whitespace(input);
        return PARSING_SUCCESS;
    }

    return PARSING_SUCCESS;
}

struct lexer {
    char* input;
    bool is_holding_token;
    struct token token;
    enum parsing_result token_result;
};

enum parsing_result peek_token(struct lexer* lexer, struct token* token) {
    if (!lexer->is_holding_token) {
        lexer->token_result = parse_token(&lexer->input, &lexer->token);
        lexer->is_holding_token = true;
    }
    *token = lexer->token;
    return lexer->token_result;
}

void advance_lexer(struct lexer* lexer) {
    if (!lexer->is_holding_token) {
        lexer->token_result = parse_token(&lexer->input, &lexer->token);
    }
    lexer->is_holding_token = false;
}

enum parsing_result skip_newlines(struct lexer* lexer) {
    enum parsing_result result;
    struct token token;
    while ((result = peek_token(lexer, &token)) == PARSING_SUCCESS &&
           token.tag == TOKEN_NEWLINE) {
        advance_lexer(lexer);
    }
    return result;
}

enum parsing_result parse_simple_command(struct lexer* lexer,
                                         struct simple_command* command) {
    command->words = NULL;
    command->words_count = 0;
    command->input_file = NULL;
    command->output_file = NULL;

    struct token token;
    enum parsing_result result;
    bool has_parsed_anything = false;
    while (true) {
        result = peek_token(lexer, &token);
        if (result == PARSING_EMPTY && has_parsed_anything) {
            result = PARSING_INCOMPLETE_INPUT;
        }
        if (result != PARSING_SUCCESS) {
            goto fail;
        }

        switch (token.tag) {
        case TOKEN_WORD:
            if (token.word == NULL) {
                break;
            }
            command->words = realloc(
                command->words, sizeof(char*) * (command->words_count + 2));
            if (command->words == NULL) {
                perror("Failed to allocate memory");
                exit(127);
            }
            command->words[command->words_count] = token.word;
            command->words[command->words_count + 1] = NULL;
            ++command->words_count;
            break;

        case TOKEN_REDIRECT_INPUT:
            advance_lexer(lexer);
            result = peek_token(lexer, &token);
            if (result != PARSING_SUCCESS) {
                goto fail;
            }
            if (token.tag != TOKEN_WORD) {
                result = PARSING_SYNTAX_ERROR;
                goto fail;
            }

            free(command->input_file);
            command->input_file = token.word;
            break;

        case TOKEN_REDIRECT_OUTPUT:
            advance_lexer(lexer);
            result = peek_token(lexer, &token);
            if (result != PARSING_SUCCESS) {
                goto fail;
            }
            if (token.tag != TOKEN_WORD) {
                result = PARSING_SYNTAX_ERROR;
                goto fail;
            }

            free(command->output_file);
            command->output_file = token.word;
            command->output_mode = OUTPUT_OVERWRITE;
            break;

        case TOKEN_APPEND_OUTPUT:
            advance_lexer(lexer);
            result = peek_token(lexer, &token);
            if (result != PARSING_SUCCESS) {
                goto fail;
            }
            if (token.tag != TOKEN_WORD) {
                result = PARSING_SYNTAX_ERROR;
                goto fail;
            }

            free(command->output_file);
            command->output_file = token.word;
            command->output_mode = OUTPUT_APPEND;
            break;

        default:
            goto end;
        }

        has_parsed_anything = true;
        advance_lexer(lexer);
    }

end:
    if (!has_parsed_anything) {
        result = PARSING_SYNTAX_ERROR;
        goto fail;
    }
    return result;

fail:
    free_simple_command(command);
    return result;
}

enum parsing_result parse_pipeline(struct lexer* lexer,
                                   struct pipeline* pipeline) {
    pipeline->commands = NULL;
    pipeline->commands_count = 0;

    enum parsing_result result;
    while (true) {
        struct simple_command command;
        result = parse_simple_command(lexer, &command);
        if (result == PARSING_EMPTY && pipeline->commands_count > 0) {
            result = PARSING_INCOMPLETE_INPUT;
        }
        if (result != PARSING_SUCCESS) {
            goto fail;
        }

        pipeline->commands =
            realloc(pipeline->commands, sizeof(struct simple_command) *
                                            (pipeline->commands_count + 1));
        if (pipeline->commands == NULL) {
            perror("Failed to allocate memory");
            exit(127);
        }
        pipeline->commands[pipeline->commands_count] = command;
        ++pipeline->commands_count;

        struct token token;
        result = peek_token(lexer, &token);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }
        if (token.tag == TOKEN_PIPE) {
            advance_lexer(lexer);
            skip_newlines(lexer);
            continue;
        }

        break;
    }

    return PARSING_SUCCESS;
fail:
    free_pipeline(pipeline);
    return result;
}

enum parsing_result parse_boolean_command(struct lexer* lexer,
                                          struct boolean_command* command) {
    command->next = NULL;

    enum parsing_result result;
    while (true) {
        result = parse_pipeline(lexer, &command->pipeline);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }

        struct token token;
        result = peek_token(lexer, &token);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }
        switch (token.tag) {
        case TOKEN_OR:
            command->tag = OR_COMMAND;
            break;
        case TOKEN_AND:
            command->tag = AND_COMMAND;
            break;
        default:
            goto end;
        }

        advance_lexer(lexer);
        skip_newlines(lexer);

        command->next = malloc(sizeof(struct boolean_command));
        if (command->next == NULL) {
            perror("Failed to allocate memory");
            exit(127);
        }
        result = parse_boolean_command(lexer, command->next);
        if (result == PARSING_EMPTY) {
            result = PARSING_INCOMPLETE_INPUT;
        }
        if (result != PARSING_SUCCESS) {
            goto fail;
        }
        break;
    }

end:
    return PARSING_SUCCESS;

fail:
    free_boolean_command(command);
    return result;
}

enum parsing_result parse_job_command(struct lexer* lexer,
                                      struct job_command* job) {
    job->next = NULL;

    enum parsing_result result;
    while (true) {
        result = parse_boolean_command(lexer, &job->command);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }

        struct token token;
        result = peek_token(lexer, &token);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }
        switch (token.tag) {
        case TOKEN_BACKGROUND:
            job->tag = JOB_BACKGROUND;
            break;
        case TOKEN_SEMICOLON:
        case TOKEN_NEWLINE:
            job->tag = JOB_FOREGROUND;
            break;
        default:
            goto end;
        }

        advance_lexer(lexer);
        skip_newlines(lexer);

        result = peek_token(lexer, &token);
        switch (result) {
        case PARSING_SUCCESS:
            break;
        case PARSING_EMPTY:
            goto end;
        case PARSING_INCOMPLETE_INPUT:
        case PARSING_SYNTAX_ERROR:
            goto fail;
        }

        job->next = malloc(sizeof(struct job_command));
        if (job->next == NULL) {
            perror("Failed to allocate memory");
            exit(127);
        }
        result = parse_job_command(lexer, job->next);
        if (result != PARSING_SUCCESS) {
            goto fail;
        }
        break;
    }

end:
    return PARSING_SUCCESS;

fail:
    free_job_command(job);
    return result;
}

enum parsing_result parse_command(char* input, struct job_command* command) {
    struct lexer lexer = {.input = input, .is_holding_token = false};
    skip_newlines(&lexer);
    return parse_job_command(&lexer, command);
}
