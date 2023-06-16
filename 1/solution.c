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
};

struct queue {
    struct task* tasks;
    size_t tasks_count;
    size_t next_task;
};

struct worker {
    struct queue* queue;
    long quantum;
    long work_time;
    struct timespec resumed_at;
    size_t switches;
};

long time_between(struct timespec* end, struct timespec* start) {
    return (end->tv_sec - start->tv_sec) * NS_PER_S +
           (end->tv_nsec - start->tv_nsec);
}

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

void heapsort(struct task* task, struct worker* worker) {
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
            vec_push(&task->result, &number);
        }
        heapsort(task, worker);
        fclose(file);
    }

    pause_timer(worker);
    return 0;
}

void free_queue(struct queue* queue) {
    for (size_t i = 0; i < queue->tasks_count; ++i) {
        vec_free(&queue->tasks[i].result);
    }
    free(queue->tasks);
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
            if (positions[i] == queue->tasks[i].result.length) {
                continue;
            }

            int this_element =
                ((int*)queue->tasks[i].result.elements)[positions[i]];
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
    if (argc < 3) {
        char* name = (argc > 0) ? argv[0] : "./a.out";
        printf("Usage: %s target_latency workers files...\n", name);
        return 1;
    }
    size_t files_count = argc - 3;
    long target_latency = (long)parse_integer(argv[1], LONG_MAX);
    long quantum = target_latency * NS_PER_US / files_count;
    size_t workers_count = (size_t)parse_integer(argv[2], SIZE_MAX);

    FILE* output = fopen("output.txt", "w");
    if (output == NULL) {
        printf("Failed to open output.txt");
        return 1;
    }

    struct queue queue = {
        .tasks = calloc(files_count, sizeof(struct task)),
        .tasks_count = files_count,
        .next_task = 0,
    };
    if (queue.tasks == NULL) {
        puts("Failed to allocate memory for tasks");
        fclose(output);
        return 2;
    }
    for (size_t i = 0; i < files_count; ++i) {
        queue.tasks[i].filepath = argv[i + 3];
        queue.tasks[i].result = vec_new(sizeof(int));
    }

    coro_sched_init();
    struct worker* workers = calloc(workers_count, sizeof(struct worker));
    if (workers == NULL) {
        puts("Failed to allocate memory for workers");
        free_queue(&queue);
        fclose(output);
        return 2;
    }
    for (size_t i = 0; i < workers_count; ++i) {
        workers[i].queue = &queue;
        workers[i].quantum = quantum;
        workers[i].work_time = 0;
        workers[i].switches = 0;

        coro_new(worker, &workers[i]);
    }

    struct coro* c;
    while ((c = coro_sched_wait()) != NULL) {
        coro_delete(c);
    }

    long total_work_time = 0;
    for (size_t i = 0; i < workers_count; ++i) {
        total_work_time += workers[i].work_time;
        printf("Coroutine %zu: worked for %fms, switched %zu times\n", i,
               (double)workers[i].work_time / NS_PER_MS, workers[i].switches);
    }
    printf("Total work time: %fms\n", (double)total_work_time / NS_PER_MS);

    merge_results(&queue, output);

    free(workers);
    free_queue(&queue);
    fclose(output);
    return 0;
}
