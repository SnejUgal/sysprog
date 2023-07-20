#pragma once

#include <stdbool.h>
#include <stddef.h>

struct chat_message* chat_message_new(const char* author, const char* data);

struct chat_messages {
    struct chat_message** messages;
    size_t capacity;
    size_t start;
    size_t end;
};

struct chat_messages chat_messages_new();
void chat_messages_push(struct chat_messages* messages,
                        struct chat_message* message);
struct chat_message* chat_messages_pop(struct chat_messages* messages);
void chat_messages_delete(struct chat_messages* messages);

struct buffer {
    char* buffer;
    size_t capacity;
    size_t size;
    size_t start;
};

struct buffer buffer_new();
void buffer_push(struct buffer* buffer, const char* data, size_t count);
bool buffer_has_string(struct buffer* buffer);
/**
 * @retval -1 Check errno.
 * @retval 0 The socket is still active. All delivered data is read.
 * @retval 1 The socket disconnected. All delivered data is read.
 */
int buffer_recv(struct buffer* buffer, int socket_fd);
/**
 * @retval -1 Check errno.
 * @retval 0 All data was sent.
 * @retval 1 Some part couldn't be sent.
 */
int buffer_send(struct buffer* buffer, int socket_fd);
void buffer_delete(struct buffer* buffer);
