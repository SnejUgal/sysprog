#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libcoro.h"
#include "vec.h"

#define NS_PER_S 1e9
#define NS_PER_MS 1e6
#define NS_PER_US 1e3

struct task {
    char* filepath;
    struct vec result;

    long quantum;
    long work_time;
    struct timespec resumed_at;
    size_t switches;
};

void start_timer(struct task* task) {
    clock_gettime(CLOCK_MONOTONIC, &task->resumed_at);
}

long time_between(struct timespec* end, struct timespec* start) {
    return (end->tv_sec - start->tv_sec) * NS_PER_S +
           (end->tv_nsec - start->tv_nsec);
}

void pause_timer(struct task* task) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    task->work_time += time_between(&now, &task->resumed_at);
}

void yield(struct task* task) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long time_spent = time_between(&now, &task->resumed_at);
    if (time_spent > task->quantum) {
        task->work_time += time_spent;
        coro_yield();
        task->switches = coro_switch_count(coro_this());
        start_timer(task);
    }
}

void swap(int* a, int* b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

size_t get_parent_index(size_t child) { return (child - 1) / 2; }
size_t get_left_child_index(size_t parent) { return parent * 2 + 1; }
size_t get_right_child_index(size_t parent) { return parent * 2 + 2; }

void sift_down(struct vec* numbers, size_t start, size_t heap_end) {
    int* elements = numbers->elements;

    size_t root = start;
    while (get_left_child_index(root) <= heap_end) {
        size_t max = root;

        size_t left_child = get_left_child_index(root);
        if (elements[left_child] > elements[max]) {
            max = left_child;
        }

        size_t right_child = get_right_child_index(root);
        if (right_child <= heap_end && elements[right_child] > elements[max]) {
            max = right_child;
        }

        if (max == root) {
            break;
        }

        swap(&elements[root], &elements[max]);
        root = max;
    }
}

void build_heap(struct vec* numbers) {
    size_t heap_end = numbers->length - 1;
    size_t parent = get_parent_index(heap_end);
    while (true) {
        sift_down(numbers, parent, heap_end);
        if (parent == 0) {
            break;
        }
        --parent;
    }
}

void heapsort(struct task* task) {
    if (task->result.length <= 1) {
        return;
    }
    build_heap(&task->result);

    int* elements = task->result.elements;
    size_t heap_end = task->result.length - 1;
    while (heap_end > 0) {
        swap(&elements[0], &elements[heap_end]);
        --heap_end;
        sift_down(&task->result, 0, heap_end);

        yield(task);
    }
}

static int coroutine_func_f(void* context) {
    struct task* task = context;
    start_timer(task);

    FILE* file = fopen(task->filepath, "r");
    if (file == NULL) {
        perror("Failed to open file");
        pause_timer(task);
        return 1;
    }

    int number;
    while (fscanf(file, "%d", &number) == 1) {
        vec_push(&task->result, &number);
    }
    heapsort(task);

    pause_timer(task);
    return 0;
}

void free_tasks(struct vec* tasks) {
    struct task* elements = tasks->elements;
    for (size_t i = 0; i < tasks->length; ++i) {
        vec_free(&elements[i].result);
    }
    vec_free(tasks);
}

void merge_results(struct vec* tasks_vec, FILE* output) {
    struct task* tasks = tasks_vec->elements;

    struct vec positions_vec =
        vec_with_capacity(tasks_vec->length, sizeof(size_t));
    memset(positions_vec.elements, 0, tasks_vec->length * sizeof(size_t));
    positions_vec.length = tasks_vec->length;
    size_t* positions = positions_vec.elements;

    while (true) {
        int min_element = INT_MIN;
        size_t min_from = tasks_vec->length;
        for (size_t i = 0; i < tasks_vec->length; ++i) {
            if (positions[i] == tasks[i].result.length) {
                continue;
            }

            int this_element = ((int*)tasks[i].result.elements)[positions[i]];
            if (min_from == tasks_vec->length || min_element > this_element) {
                min_element = this_element;
                min_from = i;
            }
        }
        if (min_from == tasks_vec->length) {
            break;
        }

        ++positions[min_from];
        fprintf(output, "%d ", min_element);
    }
    fflush(output);
}

uintmax_t parse_integer(char* string, uintmax_t max_value) {
    char* end;
    uintmax_t result = strtoumax(string, &end, 10);
    if (result > max_value || *end != '\0') {
        printf("error: %s is not a valid integer\n", string);
        exit(1);
    }
    return result;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        char* name = (argc > 0) ? argv[0] : "./a.out";
        printf("Usage: %s target_latency files...\n", name);
        return 1;
    }
    long target_latency = (long)parse_integer(argv[1], LONG_MAX);

    FILE* output = fopen("output.txt", "w");
    if (output == NULL) {
        printf("Failed to open output.txt");
        return 1;
    }

    coro_sched_init();

    size_t files = argc - 2;
    long quantum = target_latency * NS_PER_US / files;

    struct vec tasks = vec_with_capacity(files, sizeof(struct task));
    for (size_t i = 0; i < files; ++i) {
        struct task task = {.filepath = argv[i + 2],
                            .result = vec_new(sizeof(int)),
                            .quantum = quantum,
                            .work_time = 0,
                            .switches = 0};
        vec_push(&tasks, &task);

        coro_new(coroutine_func_f, &((struct task*)tasks.elements)[i]);
    }

    /* Wait for the couritines */
    struct coro* c;
    bool has_failed = false;
    while ((c = coro_sched_wait()) != NULL) {
        if (coro_status(c) != 0) {
            has_failed = true;
        }
        coro_delete(c);
    }
    if (has_failed) {
        free_tasks(&tasks);
        printf("Exiting because some courotines failed");
        return 1;
    }

    long total_work_time = 0;
    for (size_t i = 0; i < tasks.length; ++i) {
        struct task* task = &((struct task*)tasks.elements)[i];
        total_work_time += task->work_time;
        printf("Coroutine %zu: worked for %fms, switched %zu times\n", i,
               (double)task->work_time / NS_PER_MS, task->switches);
    }
    printf("Total work time: %fms\n", (double)total_work_time / NS_PER_MS);

    merge_results(&tasks, output);

    free_tasks(&tasks);
    return 0;
}
