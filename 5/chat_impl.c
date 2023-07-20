#include "chat_impl.h"
#include "chat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct chat_message* chat_message_new(const char* author, const char* data) {
    struct chat_message* message = malloc(sizeof(struct chat_message));
    if (message == NULL) {
        abort();
    }
    message->author = strdup(author);
    message->data = strdup(data);
    return message;
}

struct chat_messages chat_messages_new() {
    struct chat_messages messages = {
        .messages = NULL, .capacity = 0, .start = 0, .end = 0};
    return messages;
}

void chat_messages_push(struct chat_messages* messages,
                        struct chat_message* message) {
    if (messages->end - messages->start == messages->capacity) {
        size_t new_capacity =
            messages->capacity < 16 ? 16 : messages->capacity * 2;
        struct chat_message** new_messages =
            malloc(new_capacity * sizeof(struct chat_message));
        if (new_messages == NULL) {
            abort();
        }

        if (messages->capacity > 0) {
            size_t start_index = messages->start % messages->capacity;
            size_t from_end = messages->capacity - start_index;
            size_t from_start = messages->capacity - from_end;

            memcpy(new_messages, messages->messages + start_index,
                   from_end * sizeof(struct chat_message*));
            memcpy(new_messages + from_end, messages->messages,
                   from_start * sizeof(struct chat_message*));
        }

        messages->start = 0;
        messages->end = messages->capacity;
        messages->capacity = new_capacity;
        messages->messages = new_messages;
    }

    messages->messages[messages->end % messages->capacity] = message;
    ++messages->end;
}

struct chat_message* chat_messages_pop(struct chat_messages* messages) {
    if (messages->start == messages->end) {
        return NULL;
    }

    struct chat_message* message =
        messages->messages[messages->start % messages->capacity];
    ++messages->start;
    return message;
}

void chat_messages_delete(struct chat_messages* messages) {
    for (size_t i = messages->start; i < messages->end; ++i) {
        chat_message_delete(messages->messages[i % messages->capacity]);
    }
    free(messages->messages);
}

struct buffer buffer_new() {
    struct buffer buffer = {
        .buffer = NULL, .capacity = 0, .size = 0, .start = 0};
    return buffer;
}

bool buffer_has_string(struct buffer* buffer) {
    for (size_t i = buffer->start; i < buffer->size; ++i) {
        if (buffer->buffer[i] == '\0') {
            return true;
        }
    }
    return false;
}

void buffer_make_room(struct buffer* buffer, size_t at_least) {
    if (buffer->start > 0) {
        memmove(buffer->buffer, buffer->buffer + buffer->start,
                buffer->size - buffer->start);
        buffer->size -= buffer->start;
        buffer->start = 0;
    }

    if (buffer->capacity > buffer->size + at_least) {
        return;
    }

    size_t new_capacity;
    if (at_least > 1) {
        new_capacity = buffer->size + at_least;
    } else if (buffer->capacity < 1024) {
        new_capacity = 1024;
    } else {
        new_capacity = buffer->capacity * 2;
    }
    buffer->buffer = realloc(buffer->buffer, new_capacity);
    if (buffer->buffer == NULL) {
        abort();
    }
    buffer->capacity = new_capacity;
}

void buffer_push(struct buffer* buffer, const char* data, size_t size) {
    buffer_make_room(buffer, size);
    memcpy(buffer->buffer + buffer->size, data, size);
    buffer->size += size;
}

int buffer_recv(struct buffer* buffer, int socket_fd) {
    while (true) {
        buffer_make_room(buffer, 1);
        ssize_t read = recv(socket_fd, buffer->buffer + buffer->size,
                            buffer->capacity - buffer->size, 0);
        if (read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == ECONNRESET) {
                return 1;
            }
            return -1;
        }
        if (read == 0) {
            return 1;
        }

        buffer->size += read;
    }
}

int buffer_send(struct buffer* buffer, int socket_fd) {
    while (buffer->start < buffer->size) {
        ssize_t sent = send(socket_fd, buffer->buffer + buffer->start,
                            buffer->size - buffer->start, 0);
        if (sent == -1 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -1;
        }
        if (sent <= 0) {
            return 1;
        }
        buffer->start += sent;
    }

    buffer->start = 0;
    buffer->size = 0;
    return 0;
}

void buffer_delete(struct buffer* buffer) { free(buffer->buffer); }
