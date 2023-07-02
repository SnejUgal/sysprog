#include "userfs.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    /** Block memory. */
    char* memory;
    /** How many bytes are occupied. */
    int occupied;
    /** Next block in the file. */
    struct block* next;
    /** Previous block in the file. */
    struct block* prev;

    /* PUT HERE OTHER MEMBERS */
};

struct file {
    /** Double-linked list of file blocks. */
    struct block* block_list;
    /**
     * Last block in the list above for fast access to the end
     * of file.
     */
    struct block* last_block;
    /** How many file descriptors are opened on the file. */
    int refs;
    /** File name. */
    char* name;
    /** Files are stored in a double-linked list. */
    struct file* next;
    struct file* prev;

    bool is_deleted;
};

/** List of all files. */
static struct file* file_list = NULL;

struct filedesc {
    struct file* file;
    struct block* current_block;
    int offset_in_block;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc** file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code ufs_errno() { return ufs_error_code; }

struct file* find_file_by_name(const char* filename) {
    struct file* current = file_list;
    while (current != NULL) {
        if (strcmp(current->name, filename) == 0) {
            break;
        }
        current = current->next;
    }

    return current;
}

struct file* create_file(const char* filename) {
    struct file* file = malloc(sizeof(struct file));
    if (file == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return NULL;
    }

    file->block_list = NULL;
    file->last_block = NULL;
    file->refs = 0;
    file->name = strdup(filename);
    file->prev = NULL;
    file->is_deleted = false;

    file->next = file_list;
    if (file_list != NULL) {
        file_list->prev = file;
    }
    file_list = file;

    return file;
}

void delete_file(struct file* file) {
    if (file_list == file) {
        file_list = file->next;
    }

    if (file->prev != NULL) {
        file->prev->next = file->next;
    }
    if (file->next != NULL) {
        file->next->prev = file->prev;
    }
    file->prev = NULL;
    file->next = NULL;

    if (file->refs == 0) {
        struct block* current = file->block_list;
        while (current != NULL) {
            struct block* next = current->next;
            free(current->memory);
            free(current);
            current = next;
        }
        free(file);
    }
}

int get_free_fd() {
    int min_fd = 0;

    if (file_descriptor_count == file_descriptor_capacity) {
        long new_capacity = file_descriptor_capacity * 2;
        if (new_capacity < 16) {
            new_capacity = 16;
        }
        if (new_capacity > INT_MAX / 2) {
            return -1;
        }

        struct filedesc** new_descriptors =
            realloc(file_descriptors, sizeof(struct filedesc*) * new_capacity);
        if (new_descriptors == NULL) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        file_descriptor_capacity = new_capacity;
        file_descriptors = new_descriptors;
        min_fd = file_descriptor_count;
    }

    while (file_descriptors[min_fd] != NULL) {
        ++min_fd;
    }

    return min_fd;
}

void allocate_filedesc(int fd, struct file* file) {
    struct filedesc* filedesc = malloc(sizeof(struct filedesc));
    if (filedesc == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return;
    }

    filedesc->file = file;
    filedesc->current_block = file->block_list;
    filedesc->offset_in_block = 0;

    file_descriptors[fd] = filedesc;
    ++file_descriptor_count;
    ++file->refs;
}

struct filedesc* get_filedesc(int fd) {
    if (fd < 0 || fd >= file_descriptor_capacity ||
        file_descriptors[fd] == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return NULL;
    }

    return file_descriptors[fd];
}

void allocate_block(struct filedesc* filedesc) {
    struct block* new_block = malloc(sizeof(struct block));
    if (new_block == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return;
    }

    new_block->memory = malloc(BLOCK_SIZE);
    if (new_block->memory == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        free(new_block);
        return;
    }

    new_block->occupied = 0;
    new_block->next = NULL;

    new_block->prev = filedesc->file->last_block;
    if (filedesc->file->last_block != NULL) {
        filedesc->file->last_block->next = new_block;
    }
    filedesc->file->last_block = new_block;
    if (filedesc->file->block_list == NULL) {
        filedesc->file->block_list = new_block;
    }

    filedesc->current_block = new_block;
    filedesc->offset_in_block = 0;
}

void free_filedesc(int fd) {
    struct file* file = file_descriptors[fd]->file;
    --file->refs;
    if (file->refs == 0 && file->is_deleted == true) {
        delete_file(file);
    }

    free(file_descriptors[fd]);
    file_descriptors[fd] = NULL;
    --file_descriptor_count;
}

int ufs_open(const char* filename, int flags) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct file* file = find_file_by_name(filename);
    if (file == NULL) {
        if ((flags & UFS_CREATE) == 0) {
            ufs_error_code = UFS_ERR_NO_FILE;
            return -1;
        }

        file = create_file(filename);
        if (ufs_error_code != UFS_ERR_NO_ERR) {
            return -1;
        }
    }

    int free_fd = get_free_fd();
    if (ufs_error_code != UFS_ERR_NO_ERR) {
        return -1;
    }

    allocate_filedesc(free_fd, file);
    if (ufs_error_code != UFS_ERR_NO_ERR) {
        return -1;
    }

    return free_fd;
}

ssize_t ufs_write(int fd, const char* buf, size_t size) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct filedesc* filedesc = get_filedesc(fd);
    if (filedesc == NULL) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    size_t written = 0;
    while (true) {
        if (filedesc->current_block == NULL) {
            if (filedesc->file->block_list == NULL) {
                allocate_block(filedesc);
                if (ufs_error_code != UFS_ERR_NO_ERR) {
                    break;
                }
            } else {
                filedesc->current_block = filedesc->file->block_list;
            }
        }

        size_t to_write = BLOCK_SIZE - filedesc->offset_in_block;
        if (to_write > size - written) {
            to_write = size - written;
        }

        memcpy(filedesc->current_block->memory + filedesc->offset_in_block,
               buf + written, to_write);
        written += to_write;
        filedesc->offset_in_block += to_write;
        if (filedesc->current_block->occupied <= filedesc->offset_in_block) {
            filedesc->current_block->occupied = filedesc->offset_in_block;
        }

        if (written == size) {
            break;
        }
        if (filedesc->current_block->next == NULL) {
            allocate_block(filedesc);
            if (ufs_error_code != UFS_ERR_NO_ERR) {
                break;
            }
        } else {
            filedesc->current_block = filedesc->current_block->next;
            filedesc->offset_in_block = 0;
        }
    }

    if (written == 0) {
        return -1;
    }

    return written;
}

ssize_t ufs_read(int fd, char* buf, size_t size) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct filedesc* filedesc = get_filedesc(fd);
    if (filedesc == NULL) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    size_t read = 0;
    while (true) {
        if (filedesc->current_block == NULL) {
            if (filedesc->file->block_list == NULL) {
                break;
            }
            filedesc->current_block = filedesc->file->block_list;
        }

        size_t to_read =
            filedesc->current_block->occupied - filedesc->offset_in_block;
        if (to_read > size - read) {
            to_read = size - read;
        }

        memcpy(buf + read,
               filedesc->current_block->memory + filedesc->offset_in_block,
               to_read);
        read += to_read;
        filedesc->offset_in_block += to_read;

        if (read == size) {
            break;
        }

        if (filedesc->current_block->next == NULL) {
            break;
        }
        filedesc->current_block = filedesc->current_block->next;
        filedesc->offset_in_block = 0;
    }

    return read;
}

int ufs_close(int fd) {
    ufs_error_code = UFS_ERR_NO_ERR;

    if (get_filedesc(fd) == NULL) {
        return -1;
    }
    free_filedesc(fd);

    return 0;
}

int ufs_delete(const char* filename) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct file* file = find_file_by_name(filename);
    if (file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    file->is_deleted = true;
    delete_file(file);
    return 0;
}

void ufs_destroy(void) {}
