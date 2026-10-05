/* The FreeRTOS subset firmware/main/pet_audio.c uses, on POSIX threads. Each
 * task is a detached thread; one tick is one millisecond. Mutex misuse that
 * would hang or corrupt the device (a second take by the holder, a give by
 * another task) aborts the test instead. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct fake_queue {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    size_t length;
    size_t item_size;
    size_t head;
    size_t count;
    unsigned char *items;
};

struct fake_semaphore {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool taken;
    pthread_t owner;
};

struct fake_task {
    pthread_t thread;
    TaskFunction_t function;
    void *arg;
    pthread_mutex_t lock;
    pthread_cond_t notified;
    uint32_t notifications;
};

static _Thread_local struct fake_task *t_current;

static void fatal(const char *message)
{
    fprintf(stderr, "fake_rtos: %s\n", message);
    abort();
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* Condition variables time out on CLOCK_REALTIME, the one clock every POSIX
 * host supports there; the waits are milliseconds long. */
static struct timespec deadline_after(TickType_t ticks)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ticks / 1000;
    deadline.tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

/* Waits for a change; false once the deadline has passed. */
static bool wait_change(pthread_cond_t *changed, pthread_mutex_t *lock, TickType_t ticks,
                        const struct timespec *deadline)
{
    if (ticks == portMAX_DELAY) {
        pthread_cond_wait(changed, lock);
        return true;
    }
    return pthread_cond_timedwait(changed, lock, deadline) != ETIMEDOUT;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = { .tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000L };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct fake_queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->items = calloc(length, item_size);
    if (!queue->items) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->changed, NULL);
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    struct timespec deadline = deadline_after(wait);
    pthread_mutex_lock(&queue->lock);
    while (queue->count == queue->length) {
        if (!wait || !wait_change(&queue->changed, &queue->lock, wait, &deadline)) {
            if (queue->count == queue->length) {
                pthread_mutex_unlock(&queue->lock);
                return pdFALSE;
            }
        }
    }
    size_t slot = (queue->head + queue->count) % queue->length;
    memcpy(queue->items + slot * queue->item_size, item, queue->item_size);
    queue->count++;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    struct timespec deadline = deadline_after(wait);
    pthread_mutex_lock(&queue->lock);
    while (!queue->count) {
        if (!wait || !wait_change(&queue->changed, &queue->lock, wait, &deadline)) {
            if (!queue->count) {
                pthread_mutex_unlock(&queue->lock);
                return pdFALSE;
            }
        }
    }
    memcpy(item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    queue->count--;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->lock);
    UBaseType_t count = (UBaseType_t)queue->count;
    pthread_mutex_unlock(&queue->lock);
    return count;
}

UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->lock);
    UBaseType_t spaces = (UBaseType_t)(queue->length - queue->count);
    pthread_mutex_unlock(&queue->lock);
    return spaces;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_semaphore *semaphore = calloc(1, sizeof(*semaphore));
    if (!semaphore) return NULL;
    pthread_mutex_init(&semaphore->lock, NULL);
    pthread_cond_init(&semaphore->changed, NULL);
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    struct timespec deadline = deadline_after(wait);
    pthread_mutex_lock(&semaphore->lock);
    if (semaphore->taken && pthread_equal(semaphore->owner, pthread_self())) {
        fatal("a task took a mutex it already holds (deadlock on the device)");
    }
    while (semaphore->taken) {
        if (!wait || !wait_change(&semaphore->changed, &semaphore->lock, wait, &deadline)) {
            if (semaphore->taken) {
                pthread_mutex_unlock(&semaphore->lock);
                return pdFALSE;
            }
        }
    }
    semaphore->taken = true;
    semaphore->owner = pthread_self();
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    pthread_mutex_lock(&semaphore->lock);
    if (!semaphore->taken || !pthread_equal(semaphore->owner, pthread_self())) {
        fatal("a mutex was given by a task that does not hold it");
    }
    semaphore->taken = false;
    pthread_cond_broadcast(&semaphore->changed);
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

static void *task_main(void *arg)
{
    t_current = arg;
    t_current->function(t_current->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack_depth,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle,
                                   BaseType_t core)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)core;
    struct fake_task *task = calloc(1, sizeof(*task));
    if (!task) return pdFAIL;
    task->function = function;
    task->arg = arg;
    pthread_mutex_init(&task->lock, NULL);
    pthread_cond_init(&task->notified, NULL);
    if (handle) *handle = task;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int created = pthread_create(&task->thread, &attributes, task_main, task);
    pthread_attr_destroy(&attributes);
    return created == 0 ? pdPASS : pdFAIL;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait)
{
    struct fake_task *task = t_current;
    if (!task) fatal("ulTaskNotifyTake outside a task");
    struct timespec deadline = deadline_after(wait);
    pthread_mutex_lock(&task->lock);
    while (!task->notifications) {
        if (!wait || !wait_change(&task->notified, &task->lock, wait, &deadline)) {
            if (!task->notifications) break;
        }
    }
    uint32_t value = task->notifications;
    if (value) task->notifications = clear_on_exit ? 0 : value - 1;
    pthread_mutex_unlock(&task->lock);
    return value;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    pthread_mutex_lock(&task->lock);
    task->notifications++;
    pthread_cond_broadcast(&task->notified);
    pthread_mutex_unlock(&task->lock);
    return pdPASS;
}
