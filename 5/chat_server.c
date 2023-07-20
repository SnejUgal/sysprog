#define _GNU_SOURCE

#include "chat_server.h"
#include "chat.h"
#include "chat_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

struct chat_peer {
    int socket;
    char* name;
    struct buffer from_peer;
    struct buffer to_peer;
    struct epoll_event event;
};

struct chat_server {
    int socket;
    int epoll;

    struct chat_peer** peers;
    size_t peer_count;

    struct chat_messages messages;
    struct buffer my_message;
};

int chat_peer_process_message(struct chat_peer* peer,
                              struct chat_server* server) {
    char* message = peer->from_peer.buffer + peer->from_peer.start;
    size_t message_size = strlen(message) + 1;
    peer->from_peer.start += message_size;

    if (peer->name == NULL) {
        peer->name = strdup(message);
        return 0;
    }

    int result = 0;
    chat_messages_push(&server->messages,
                       chat_message_new(peer->name, message));

    for (size_t i = 0; i < server->peer_count; ++i) {
        struct chat_peer* other_peer = server->peers[i];
        if (other_peer == peer) {
            continue;
        }

        buffer_push(&other_peer->to_peer, peer->name, strlen(peer->name));
        buffer_push(&other_peer->to_peer, "\n", 1);
        buffer_push(&other_peer->to_peer, message, message_size);
        if ((other_peer->event.events & EPOLLOUT) != 0) {
            continue;
        }
        other_peer->event.events |= EPOLLOUT;
        if (epoll_ctl(server->epoll, EPOLL_CTL_MOD, other_peer->socket,
                      &other_peer->event) == -1) {
            result = CHAT_ERR_SYS;
        }
    }

    return result;
}

void chat_peer_delete(struct chat_peer* peer) {
    close(peer->socket);
    free(peer->name);
    buffer_delete(&peer->from_peer);
    buffer_delete(&peer->to_peer);
    free(peer);
}

struct chat_server* chat_server_new(void) {
    struct chat_server* server = malloc(sizeof(struct chat_server));
    if (server == NULL) {
        abort();
    }

    server->socket = -1;
    server->epoll = -1;

    server->peers = NULL;
    server->peer_count = 0;

    server->messages = chat_messages_new();
    server->my_message = buffer_new();

    return server;
}

void chat_server_delete(struct chat_server* server) {
    if (server->socket >= 0) {
        close(server->socket);
    }
    if (server->epoll >= 0) {
        close(server->epoll);
    }

    for (size_t i = 0; i < server->peer_count; ++i) {
        chat_peer_delete(server->peers[i]);
    }
    free(server->peers);

    chat_messages_delete(&server->messages);
    buffer_delete(&server->my_message);
    free(server);
}

int chat_server_listen(struct chat_server* server, uint16_t port) {
    if (server->socket != -1) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    if (server->epoll == -1) {
        server->epoll = epoll_create(1);
        if (server->epoll == -1) {
            return CHAT_ERR_SYS;
        }
    }

    server->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket == -1) {
        return CHAT_ERR_SYS;
    }
    int enable = 1;
    if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &enable,
                   sizeof(int)) == -1) {
        return CHAT_ERR_SYS;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    /* Listen on all IPs of this machine. */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server->socket, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (errno == EADDRINUSE) {
            return CHAT_ERR_PORT_BUSY;
        }
        return CHAT_ERR_SYS;
    }

    if (listen(server->socket, 16) == -1) {
        return CHAT_ERR_SYS;
    }

    int flags = fcntl(server->socket, F_GETFL, 0);
    if (flags == -1) {
        return CHAT_ERR_SYS;
    }
    if (fcntl(server->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        return CHAT_ERR_SYS;
    }

    struct epoll_event event = {.events = EPOLLIN, .data = {.ptr = NULL}};
    if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, server->socket, &event) == -1) {
        return CHAT_ERR_SYS;
    }

    return 0;
}

struct chat_message* chat_server_pop_next(struct chat_server* server) {
    return chat_messages_pop(&server->messages);
}

int chat_server_accept_peer(struct chat_server* server) {
    int peer_fd = accept4(server->socket, NULL, NULL, SOCK_NONBLOCK);
    if (peer_fd == -1 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
        return CHAT_ERR_SYS;
    }

    server->peers = realloc(server->peers, (server->peer_count + 1) *
                                               sizeof(struct chat_peer*));
    if (server->peers == NULL) {
        abort();
    }

    struct chat_peer* peer = malloc(sizeof(struct chat_peer));
    if (peer == NULL) {
        abort();
    }
    server->peers[server->peer_count] = peer;
    ++server->peer_count;

    peer->socket = peer_fd;
    peer->name = NULL;
    peer->from_peer = buffer_new();
    peer->to_peer = buffer_new();
    peer->event.events = EPOLLIN;
    peer->event.data.ptr = peer;
    epoll_ctl(server->epoll, EPOLL_CTL_ADD, peer_fd, &peer->event);

    return 0;
}

int chat_server_update(struct chat_server* server, double timeout) {
    if (server->socket == -1) {
        return CHAT_ERR_NOT_STARTED;
    }

    struct epoll_event event;
    int events = epoll_wait(server->epoll, &event, 1, (int)(timeout * 1000.0));
    if (events == 0) {
        return CHAT_ERR_TIMEOUT;
    }
    if (events == -1) {
        return CHAT_ERR_SYS;
    }

    if (event.data.ptr == NULL) {
        return chat_server_accept_peer(server);
    }

    struct chat_peer* peer = event.data.ptr;
    if ((event.events & EPOLLIN) != 0) {
        int code = buffer_recv(&peer->from_peer, peer->socket);
        if (code == -1) {
            return CHAT_ERR_SYS;
        }

        int result = 0;
        while (buffer_has_string(&peer->from_peer)) {
            result = chat_peer_process_message(peer, server);
            if (result != 0) {
                break;
            }
        }

        if (code == 1) {
            if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, peer->socket, NULL) ==
                -1) {
                result = CHAT_ERR_SYS;
            }
            chat_peer_delete(peer);
            for (size_t i = 0; i < server->peer_count - 1; ++i) {
                if (server->peers[i] == peer) {
                    server->peers[i] = server->peers[server->peer_count - 1];
                    break;
                }
            }
            --server->peer_count;
            return result;
        }
    }

    if ((event.events & EPOLLOUT) != 0) {
        int result = buffer_send(&peer->to_peer, peer->socket);
        if (result == -1) {
            return CHAT_ERR_SYS;
        }
        if (result == 0) {
            peer->event.events &= ~EPOLLOUT;
            if (epoll_ctl(server->epoll, EPOLL_CTL_MOD, peer->socket,
                          &peer->event) == -1) {
                return CHAT_ERR_SYS;
            }
        }
    }

    return 0;
}

int chat_server_get_descriptor(const struct chat_server* server) {
    return server->epoll;
}

int chat_server_get_socket(const struct chat_server* server) {
    return server->socket;
}

int chat_server_get_events(const struct chat_server* server) {
    if (server->socket == -1) {
        return 0;
    }
    int flags = CHAT_EVENT_INPUT;
    for (size_t i = 0; i < server->peer_count; ++i) {
        struct chat_peer* peer = server->peers[i];
        if (peer->to_peer.start < peer->to_peer.size) {
            flags |= CHAT_EVENT_OUTPUT;
            break;
        }
    }

    return flags;
}

int chat_server_feed(struct chat_server* server, const char* msg,
                     uint32_t msg_size) {

    buffer_push(&server->my_message, msg, msg_size);
    for (size_t i = server->my_message.start; i < server->my_message.size;
         ++i) {
        switch (server->my_message.buffer[i]) {
        case '\n':
            server->my_message.buffer[i] = '\0';
            break;
        }
    }

    int result = 0;
    while (buffer_has_string(&server->my_message)) {
        char* message = server->my_message.buffer + server->my_message.start;
        size_t message_size = strlen(message) + 1;
        server->my_message.start += message_size;

        for (size_t i = 0; i < server->peer_count; ++i) {
            struct chat_peer* peer = server->peers[i];

            buffer_push(&peer->to_peer, "server\n", strlen("server\n"));
            buffer_push(&peer->to_peer, message, message_size);
            if ((peer->event.events & EPOLLOUT) != 0) {
                continue;
            }
            peer->event.events |= EPOLLOUT;
            if (epoll_ctl(server->epoll, EPOLL_CTL_MOD, peer->socket,
                          &peer->event) == -1) {
                result = CHAT_ERR_SYS;
            }
        }
    }

    return result;
}
