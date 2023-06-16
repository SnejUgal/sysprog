#pragma once

#include <stddef.h>

struct vec {
    void* elements;
    size_t element_size;
    size_t length;
    size_t capacity;
};

struct vec vec_new(size_t element_size);
struct vec vec_with_capacity(size_t capacity, size_t element_size);
void vec_push(struct vec* self, const void* value);
void vec_free(struct vec* self);
