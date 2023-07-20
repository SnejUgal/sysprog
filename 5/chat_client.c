#include "chat_client.h"
#include "chat.h"
#include "chat_impl.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct chat_client {
    int socket;
    const char* name;
    struct chat_messages messages;
    struct buffer from_server;
    struct buffer to_server;
};

struct chat_client* chat_client_new(const char* name) {
    struct chat_client* client = malloc(sizeof(struct chat_client));
    if (client == NULL) {
        abort();
    }

    client->socket = -1;
    client->name = name;
    client->messages = chat_messages_new();
    client->from_server = buffer_new();
    client->to_server = buffer_new();

    return client;
}

void chat_client_delete(struct chat_client* client) {
    if (client->socket >= 0) {
        close(client->socket);
    }

    chat_messages_delete(&client->messages);
    buffer_delete(&client->from_server);
    buffer_delete(&client->to_server);

    free(client);
}

int chat_client_connect(struct chat_client* client, const char* addr) {
    if (client->socket != -1) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    char* port = strdup(addr);
    char* node = strsep(&port, ":");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    int code = getaddrinfo(node, port, &hints, &result);
    free(node);
    if (code == EAI_SYSTEM) {
        return CHAT_ERR_SYS;
    }
    if (code != 0 || result == NULL) {
        return CHAT_ERR_NO_ADDR;
    }

    for (struct addrinfo* current = result; current != NULL;
         current = current->ai_next) {
        int socket_fd = socket(current->ai_family, current->ai_socktype,
                               current->ai_protocol);
        if (socket_fd == -1) {
            freeaddrinfo(result);
            return CHAT_ERR_SYS;
        }

        if (connect(socket_fd, current->ai_addr, current->ai_addrlen) == -1) {
            close(socket_fd);
            continue;
        }

        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags == -1) {
            return CHAT_ERR_SYS;
        }
        if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            return CHAT_ERR_SYS;
        }

        client->socket = socket_fd;
        break;
    }

    freeaddrinfo(result);
    if (client->socket == -1) {
        return CHAT_ERR_NO_ADDR;
    }

    return chat_client_feed(client, client->name, strlen(client->name) + 1);
}

struct chat_message* chat_client_pop_next(struct chat_client* client) {
    return chat_messages_pop(&client->messages);
}

int chat_client_process_message(struct chat_client* client) {
    char* message = client->from_server.buffer + client->from_server.start;
    client->from_server.start += strlen(message) + 1;

    char* author = strsep(&message, "\n");
    if (message == NULL) {
        // bad message from server
        return 0;
    }

    chat_messages_push(&client->messages, chat_message_new(author, message));
    return 0;
}

int chat_client_update(struct chat_client* client, double timeout) {
    if (client->socket == -1) {
        return CHAT_ERR_NOT_STARTED;
    }

    struct pollfd fd = {
        .fd = client->socket,
        .events = chat_events_to_poll_events(chat_client_get_events(client))};

    int result = poll(&fd, 1, (int)(timeout * 1000.0));
    if (result == -1) {
        return CHAT_ERR_SYS;
    }
    if (result == 0) {
        return CHAT_ERR_TIMEOUT;
    }

    if ((fd.revents & POLLIN) != 0) {
        int code = buffer_recv(&client->from_server, client->socket);
        if (code == -1) {
            return CHAT_ERR_SYS;
        }

        int result = 0;
        while (buffer_has_string(&client->from_server)) {
            result = chat_client_process_message(client);
            if (result != 0) {
                break;
            }
        }

        if (code == 1) {
            close(client->socket);
            client->socket = -1;
        }
    }

    if ((fd.revents & POLLOUT) != 0) {
        int result = buffer_send(&client->to_server, client->socket);
        if (result == -1) {
            return CHAT_ERR_SYS;
        }
    }

    return 0;
}

int chat_client_get_descriptor(const struct chat_client* client) {
    return client->socket;
}

int chat_client_get_events(const struct chat_client* client) {
    if (client->socket == -1) {
        return 0;
    }

    int flags = CHAT_EVENT_INPUT;
    if (client->to_server.start < client->to_server.size) {
        flags |= CHAT_EVENT_OUTPUT;
    }
    return flags;
}

int chat_client_feed(struct chat_client* client, const char* msg,
                     uint32_t msg_size) {
    buffer_push(&client->to_server, msg, (size_t)msg_size);

    for (size_t i = client->to_server.start; i < client->to_server.size; ++i) {
        switch (client->to_server.buffer[i]) {
        case '\n':
            client->to_server.buffer[i] = '\0';
            break;
        }
    }

    return 0;
}
