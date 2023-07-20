#include "thread_pool.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define NS_PER_S 1000000000

struct thread_task {
    thread_task_f function;
    void* arg;
    void* result;

    enum {
        TASK_NEW,
        TASK_RUNNING,
        TASK_FINISHED,
    } state;
    pthread_mutex_t state_lock;
    pthread_cond_t await_finished;
    bool is_detached;

    struct thread_pool* pool;
    struct thread_task* next;
};

struct thread_pool {
    pthread_t* threads;
    size_t max_thread_count;
    size_t thread_count;

    pthread_mutex_t tasks_lock;
    pthread_cond_t await_task;
    struct thread_task* tasks_front;
    struct thread_task* tasks_back;
    size_t task_count;
    bool should_shutdown;
};

int thread_pool_new(int max_thread_count, struct thread_pool** pool) {
    if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    *pool = malloc(sizeof(struct thread_pool));
    if (*pool == NULL) {
        perror("Failed to allocate memory");
        exit(2);
    }

    (*pool)->threads = NULL;
    (*pool)->max_thread_count = (size_t)max_thread_count;
    (*pool)->thread_count = 0;

    pthread_mutex_init(&(*pool)->tasks_lock, NULL);
    pthread_cond_init(&(*pool)->await_task, NULL);
    (*pool)->tasks_front = NULL;
    (*pool)->tasks_back = NULL;
    (*pool)->task_count = 0;

    (*pool)->should_shutdown = false;

    return 0;
}

int thread_pool_thread_count(const struct thread_pool* pool) {
    return pool->thread_count;
}

int thread_pool_delete(struct thread_pool* pool) {
    if (__atomic_load_n(&pool->task_count, __ATOMIC_ACQUIRE) > 0) {
        return TPOOL_ERR_HAS_TASKS;
    }

    pthread_mutex_lock(&pool->tasks_lock);
    pool->should_shutdown = true;
    pthread_cond_broadcast(&pool->await_task);
    pthread_mutex_unlock(&pool->tasks_lock);

    for (size_t i = 0; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->tasks_lock);
    pthread_cond_destroy(&pool->await_task);
    free(pool->threads);
    free(pool);

    return 0;
}

void* _thread_pool_worker(void* _pool) {
    struct thread_pool* pool = _pool;

    while (true) {
        pthread_mutex_lock(&pool->tasks_lock);
        struct thread_task* task = pool->tasks_front;
        if (task == NULL) {
            if (pool->should_shutdown) {
                pthread_mutex_unlock(&pool->tasks_lock);
                break;
            }

            pthread_cond_wait(&pool->await_task, &pool->tasks_lock);
            pthread_mutex_unlock(&pool->tasks_lock);
            continue;
        }

        pool->tasks_front = task->next;
        if (pool->tasks_back == task) {
            pool->tasks_back = NULL;
        }

        task->next = NULL;
        pthread_mutex_unlock(&pool->tasks_lock);

        __atomic_store_n(&task->state, TASK_RUNNING, __ATOMIC_RELAXED);
        task->result = task->function(task->arg);

        pthread_mutex_lock(&task->state_lock);
        if (task->is_detached) {
            pthread_mutex_unlock(&task->state_lock);
            __atomic_sub_fetch(&pool->task_count, 1, __ATOMIC_ACQ_REL);
            task->pool = NULL;
            thread_task_delete(task);
            continue;
        }
        __atomic_store_n(&task->state, TASK_FINISHED, __ATOMIC_RELAXED);
        pthread_cond_signal(&task->await_finished);
        pthread_mutex_unlock(&task->state_lock);
    }

    return NULL;
}

void _thread_pool_start_thread(struct thread_pool* pool) {
    if (pool->thread_count == pool->max_thread_count) {
        return;
    }

    pthread_t* new_threads =
        realloc(pool->threads, (pool->thread_count + 1) * sizeof(pthread_t));
    if (new_threads == NULL) {
        return;
    }
    pool->threads = new_threads;

    if (pthread_create(&pool->threads[pool->thread_count], NULL,
                       _thread_pool_worker, pool) == 0) {
        ++pool->thread_count;
    }
}

int thread_pool_push_task(struct thread_pool* pool, struct thread_task* task) {
    if (task->pool != NULL) {
        if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) != TASK_FINISHED) {
            return TPOOL_ERR_TASK_IN_POOL;
        }
    }

    if (__atomic_load_n(&pool->task_count, __ATOMIC_RELAXED) ==
        TPOOL_MAX_TASKS) {
        return TPOOL_ERR_TOO_MANY_TASKS;
    }
    size_t task_count =
        __atomic_add_fetch(&pool->task_count, 1, __ATOMIC_ACQ_REL);

    task->pool = pool;
    task->state = TASK_NEW;

    pthread_mutex_lock(&pool->tasks_lock);
    struct thread_task* previous = pool->tasks_back;
    pool->tasks_back = task;
    if (previous != NULL) {
        previous->next = task;
    }
    if (pool->tasks_front == NULL) {
        pool->tasks_front = task;
    }

    if (task_count > pool->thread_count) {
        _thread_pool_start_thread(pool);
    }
    pthread_cond_signal(&pool->await_task);
    pthread_mutex_unlock(&pool->tasks_lock);

    return 0;
}

int thread_task_new(struct thread_task** task, thread_task_f function,
                    void* arg) {
    *task = malloc(sizeof(struct thread_task));
    if (*task == NULL) {
        perror("Failed to allocate memory");
        exit(2);
    }

    (*task)->function = function;
    (*task)->arg = arg;

    (*task)->state = TASK_NEW;
    pthread_mutex_init(&(*task)->state_lock, NULL);
    (*task)->is_detached = false;

    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(&(*task)->await_finished, &attributes);
    pthread_condattr_destroy(&attributes);

    (*task)->pool = NULL;
    (*task)->next = NULL;

    return 0;
}

bool thread_task_is_finished(const struct thread_task* task) {
    return __atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_FINISHED;
}

bool thread_task_is_running(const struct thread_task* task) {
    return __atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_RUNNING;
}

int thread_task_join(struct thread_task* task, void** result) {
    if (task->pool == NULL) {
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }

    pthread_mutex_lock(&task->state_lock);
    while (true) {
        if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_FINISHED) {
            break;
        }
        pthread_cond_wait(&task->await_finished, &task->state_lock);
    }
    pthread_mutex_unlock(&task->state_lock);

    __atomic_sub_fetch(&task->pool->task_count, 1, __ATOMIC_ACQ_REL);
    task->pool = NULL;

    if (result != NULL) {
        *result = task->result;
    }
    return 0;
}

#ifdef NEED_TIMED_JOIN

int thread_task_timed_join(struct thread_task* task, double timeout,
                           void** result) {
    if (task->pool == NULL) {
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    if (timeout < 0) {
        timeout = 0;
    }

    struct timespec wait_until;
    clock_gettime(CLOCK_MONOTONIC, &wait_until);
    long nsec = timeout * NS_PER_S;
    wait_until.tv_nsec += nsec;
    if (wait_until.tv_nsec >= NS_PER_S) {
        wait_until.tv_sec += (wait_until.tv_nsec / NS_PER_S);
        wait_until.tv_nsec %= NS_PER_S;
    }

    bool has_finished = false;
    pthread_mutex_lock(&task->state_lock);
    while (true) {
        if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_FINISHED) {
            has_finished = true;
            break;
        }
        if (pthread_cond_timedwait(&task->await_finished, &task->state_lock,
                                   &wait_until) == ETIMEDOUT) {

            break;
        }
    }
    pthread_mutex_unlock(&task->state_lock);
    if (!has_finished) {
        return TPOOL_ERR_TIMEOUT;
    }

    __atomic_sub_fetch(&task->pool->task_count, 1, __ATOMIC_ACQ_REL);
    task->pool = NULL;

    if (result != NULL) {
        *result = task->result;
    }
    return 0;
}

#endif

int thread_task_delete(struct thread_task* task) {
    if (task->pool != NULL) {
        return TPOOL_ERR_TASK_IN_POOL;
    }

    pthread_mutex_destroy(&task->state_lock);
    pthread_cond_destroy(&task->await_finished);
    free(task);
    return 0;
}

#ifdef NEED_DETACH

int thread_task_detach(struct thread_task* task) {
    if (task->pool == NULL) {
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }

    pthread_mutex_lock(&task->state_lock);
    if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_FINISHED) {
        pthread_mutex_unlock(&task->state_lock);
        __atomic_sub_fetch(&task->pool->task_count, 1, __ATOMIC_ACQ_REL);
        task->pool = NULL;
        thread_task_delete(task);
        return 0;
    }

    task->is_detached = true;
    pthread_mutex_unlock(&task->state_lock);

    return 0;
}

#endif
