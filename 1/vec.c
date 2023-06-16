#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vec.h"

struct vec vec_with_capacity(size_t capacity, size_t element_size) {
    struct vec self = {
        .element_size = element_size, .length = 0, .capacity = capacity};
    if (capacity > 0) {
        self.elements = reallocarray(NULL, capacity, element_size);
        if (self.elements == NULL) {
            puts("Failed to allocate memory");
            exit(2);
        }
    }
    return self;
}

struct vec vec_new(size_t element_size) {
    return vec_with_capacity(0, element_size);
}

void vec_expand(struct vec* self) {
    size_t new_capacity = (self->capacity < 16 ? 16 : self->capacity * 2);
    self->elements =
        reallocarray(self->elements, new_capacity, self->element_size);
    if (self->elements == NULL) {
        puts("Failed to allocate memory");
        exit(2);
    }
    self->capacity = new_capacity;
}

void vec_push(struct vec* self, const void* value) {
    if (self->length == self->capacity) {
        vec_expand(self);
    }

    memcpy(self->elements + self->length * self->element_size, value,
           self->element_size);
    ++self->length;
}

void vec_free(struct vec* self) {
    free(self->elements);
    self->elements = NULL;
    self->length = 0;
    self->capacity = 0;
}
