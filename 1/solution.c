#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libcoro.h"

#define NS_PER_S 1e9
#define NS_PER_MS 1e6
#define NS_PER_US 1e3

/* Utilities */

uintmax_t parse_integer_argument(char* string, uintmax_t max_value) {
    char* end;
    uintmax_t result = strtoumax(string, &end, 10);
    if (result > max_value || *end != '\0') {
        printf("error: %s is not a valid integer\n", string);
        exit(1);
    }
    return result;
}

long time_between(struct timespec* end, struct timespec* start) {
    return (end->tv_sec - start->tv_sec) * NS_PER_S +
           (end->tv_nsec - start->tv_nsec);
}

void swap(int* a, int* b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

/* Workers */

struct task {
    char* filepath;
    int* numbers;
    size_t numbers_count;
};

struct queue {
    struct task* tasks;
    size_t tasks_count;
    size_t next_task;
};

struct queue init_queue(char** filepaths, size_t tasks_count) {
    struct queue queue = {
        .tasks = calloc(tasks_count, sizeof(struct task)),
        .tasks_count = tasks_count,
        .next_task = 0,
    };
    if (queue.tasks == NULL) {
        puts("Failed to allocate memory for tasks");
        exit(2);
    }
    for (size_t i = 0; i < tasks_count; ++i) {
        queue.tasks[i].filepath = filepaths[i];
        queue.tasks[i].numbers = NULL;
        queue.tasks[i].numbers_count = 0;
    }

    return queue;
}

void free_queue(struct queue* queue) {
    for (size_t i = 0; i < queue->tasks_count; ++i) {
        free(queue->tasks[i].numbers);
    }
    free(queue->tasks);
}

struct worker {
    struct queue* queue;
    long quantum;
    long work_time;
    struct timespec resumed_at;
    size_t switches;
};

void start_timer(struct worker* worker) {
    clock_gettime(CLOCK_MONOTONIC, &worker->resumed_at);
}

void pause_timer(struct worker* worker) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    worker->work_time += time_between(&now, &worker->resumed_at);
}

void yield(struct worker* worker) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long time_spent = time_between(&now, &worker->resumed_at);
    if (time_spent > worker->quantum) {
        worker->work_time += time_spent;
        coro_yield();
        worker->switches = coro_switch_count(coro_this());
        start_timer(worker);
    }
}

static int worker(void* context);

struct worker* init_workers(size_t workers_count, struct queue* queue,
                            long target_latency) {
    long quantum = target_latency * NS_PER_US / workers_count;
    struct worker* workers = calloc(workers_count, sizeof(struct worker));
    if (workers == NULL) {
        puts("Failed to allocate memory for workers");
        exit(2);
    }
    for (size_t i = 0; i < workers_count; ++i) {
        workers[i].queue = queue;
        workers[i].quantum = quantum;
        workers[i].work_time = 0;
        workers[i].switches = 0;

        coro_new(worker, &workers[i]);
    }
    return workers;
}

/* Processing */

size_t get_parent_index(size_t child) { return (child - 1) / 2; }
size_t get_left_child_index(size_t parent) { return parent * 2 + 1; }
size_t get_right_child_index(size_t parent) { return parent * 2 + 2; }

void sift_down(int* numbers, size_t start, size_t heap_end) {
    size_t root = start;
    while (get_left_child_index(root) <= heap_end) {
        size_t max = root;

        size_t left_child = get_left_child_index(root);
        if (numbers[left_child] > numbers[max]) {
            max = left_child;
        }

        size_t right_child = get_right_child_index(root);
        if (right_child <= heap_end && numbers[right_child] > numbers[max]) {
            max = right_child;
        }

        if (max == root) {
            break;
        }

        swap(&numbers[root], &numbers[max]);
        root = max;
    }
}

void build_heap(int* numbers, size_t heap_end) {
    size_t parent = get_parent_index(heap_end);
    while (true) {
        sift_down(numbers, parent, heap_end);
        if (parent == 0) {
            break;
        }
        --parent;
    }
}

void heap_sort(struct task* task, struct worker* worker) {
    if (task->numbers_count <= 1) {
        return;
    }

    size_t heap_end = task->numbers_count - 1;
    build_heap(task->numbers, heap_end);

    while (heap_end > 0) {
        swap(&task->numbers[0], &task->numbers[heap_end]);
        --heap_end;
        sift_down(task->numbers, 0, heap_end);

        yield(worker);
    }
}

static int worker(void* context) {
    struct worker* worker = context;
    start_timer(worker);

    struct queue* queue = worker->queue;
    while (queue->next_task != queue->tasks_count) {
        struct task* task = &queue->tasks[queue->next_task];
        ++queue->next_task;

        FILE* file = fopen(task->filepath, "r");
        if (file == NULL) {
            perror("Failed to open file");
            continue;
        }

        int number;
        while (fscanf(file, "%d", &number) == 1) {
            task->numbers =
                realloc(task->numbers, (task->numbers_count + 1) * sizeof(int));
            task->numbers[task->numbers_count] = number;
            ++task->numbers_count;
        }
        heap_sort(task, worker);
        fclose(file);
    }

    pause_timer(worker);
    return 0;
}

void merge_results(struct queue* queue, FILE* output) {
    size_t* positions = calloc(queue->tasks_count, sizeof(size_t));
    if (positions == NULL) {
        puts("Failed to allocate memory");
        exit(2);
    }

    while (true) {
        int min_element = INT_MIN;
        size_t min_from = queue->tasks_count;
        for (size_t i = 0; i < queue->tasks_count; ++i) {
            if (positions[i] == queue->tasks[i].numbers_count) {
                continue;
            }

            int this_element = queue->tasks[i].numbers[positions[i]];
            if (min_from == queue->tasks_count || min_element > this_element) {
                min_element = this_element;
                min_from = i;
            }
        }
        if (min_from == queue->tasks_count) {
            break;
        }

        ++positions[min_from];
        fprintf(output, "%d ", min_element);
    }

    fflush(output);
    free(positions);
}

void print_stats(struct worker* workers, size_t workers_count,
                 struct timespec* start, struct timespec* end) {
    printf("Total work time: %fms\n",
           (double)time_between(end, start) / NS_PER_MS);

    for (size_t i = 0; i < workers_count; ++i) {
        printf("Coroutine %zu: worked for %fms, switched %zu times\n", i,
               (double)workers[i].work_time / NS_PER_MS, workers[i].switches);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        char* name = (argc > 0) ? argv[0] : "./a.out";
        printf("Usage: %s target_latency workers files...\n", name);
        return 1;
    }
    size_t files_count = argc - 3;
    long target_latency = (long)parse_integer_argument(argv[1], LONG_MAX);
    size_t workers_count = (size_t)parse_integer_argument(argv[2], SIZE_MAX);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    FILE* output = fopen("output.txt", "w");
    if (output == NULL) {
        printf("Failed to open output.txt");
        return 1;
    }

    coro_sched_init();
    struct queue queue = init_queue(argv + 3, files_count);
    struct worker* workers =
        init_workers(workers_count, &queue, target_latency);

    struct coro* c;
    while ((c = coro_sched_wait()) != NULL) {
        coro_delete(c);
    }

    merge_results(&queue, output);
    fclose(output);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    print_stats(workers, workers_count, &start, &end);

    free(workers);
    free_queue(&queue);
    return 0;
}
