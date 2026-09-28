/*
 * Copyright 2026 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_shared_fence_syncfd.h"
#include "vkd3d_memory.h"
#include "vkd3d_threads.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <unistd.h>

#define KMT_DEVICE_MAX_PENDING_EDGES 1024
#define KMT_DEVICE_MAX_PENDING_SIGNALS 1024

struct kmt_epoll_data
{
    uint64_t order;
    int fd;
};

struct kmt_pending_edge
{
    uint64_t edge;
    uint64_t value;
    uint64_t fence_id;
    /* Only one of these can be >= 0.
     * For cross-process, need FD flinging to request real FD on demand.
     * For prototype purposes, we don't need this. */
    int syncfd;
    int eventfd;
};

struct kmt_pending_signal
{
    uint64_t fence_id;
    uint64_t order;
    uint64_t value;
};

/* Intended to be shared as shmem across processes.
 * For demonstration, implement as plain memory. */
struct kmt_device_shmem
{
    /* Global atomic. Used to generate IDs.
     * Also used to generate signal ordering. */
    UINT64 order;

    /* These can overflow.
     * Spamming too many event signals is allowed to return E_OUTOFMEMORY.
     * If there is no room to store pending signals we can block in signal
     * functions until there is room. Pending signals must complete in finite time (or we have device lost).
     */
    struct kmt_pending_edge pending_edges[KMT_DEVICE_MAX_PENDING_EDGES];
    struct kmt_pending_signal pending_signal[KMT_DEVICE_MAX_PENDING_SIGNALS];
    uint32_t pending_edges_count;
    uint32_t pending_signal_count;

    /* For cross-process, replace with futex based lock/condvar implementation.
     * We need to support cross process 32-bit and 64-bit sync, so we cannot use the easier APIs. */
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* This represents the payload exposed by creating a shared handle. */
struct kmt_fence_shmem_data
{
    /* Can be read atomically with acquire semantics. */
    uint64_t current_value;

    /* Must not be modified. */
    uint64_t id;

    /* TODO: We might want a refcount here? */
};

struct kmt_device_opaque
{
    int epoll_fd;
    int wake_fd;
    int poll_fd;
    pthread_t epoll_thread;

    /* Owns the blocks for shared fences. */
    pthread_mutex_t fence_lock;
    struct kmt_fence_shmem_data **fences;
    size_t fences_size;
    size_t fences_count;

    struct kmt_device_shmem *shmem;
};

struct kmt_handle_opaque
{
    struct kmt_fence_shmem_data *shmem;
};

static int wake_buffer_sort_cb(const void *a_, const void *b_)
{
    const struct kmt_epoll_data *a = a_;
    const struct kmt_epoll_data *b = b_;
    if (a->order < b->order)
        return -1;
    else if (a->order > b->order)
        return 1;
    else
        return 0;
}

static struct kmt_pending_signal *kmt_device_find_pending_signal_locked(kmt_device device, uint64_t order)
{
    struct kmt_device_shmem *shmem = device->shmem;
    size_t i;

    for (i = 0; i < shmem->pending_signal_count; i++)
        if (shmem->pending_signal[i].order == order)
            return &shmem->pending_signal[i];

    return NULL;
}

static struct kmt_fence_shmem_data *kmt_device_find_fence_shmem(kmt_device device, uint64_t fence_id)
{
    struct kmt_fence_shmem_data *shmem = NULL;
    size_t i = 0;

    pthread_mutex_lock(&device->fence_lock);
    for (i = 0; i < device->fences_count; i++)
    {
        if (device->fences[i]->id == fence_id)
        {
            shmem = device->fences[i];
            break;
        }
    }
    pthread_mutex_unlock(&device->fence_lock);
    return shmem;
}

static void kmt_device_signal_fence_immediate_locked(kmt_device device, struct kmt_fence_shmem_data *fence, uint64_t value)
{
    struct kmt_device_shmem *shmem = device->shmem;
    size_t i = 0;

    /* Probably shouldn't happen? */
    if (!fence)
        return;

    /* ID3D12Fence::GetCurrentValue() should not need to take locks or do complex server calls. */
    vkd3d_atomic_uint64_store_explicit(&fence->current_value, value, vkd3d_memory_order_release);

    while (i < shmem->pending_edges_count)
    {
        struct kmt_pending_edge *edge = &shmem->pending_edges[i];
        if (value >= edge->value)
        {
            /* Signal an eventfd, (or ntsync, or whatever primitive we need).
             * For signal order reasons, this must happen after we update the current value. */
            if (edge->eventfd >= 0)
            {
                const uint64_t sig = 1;
                write(edge->eventfd, &sig, sizeof(sig));

                /* For cross-process, we should probably just close this right away.
                 * This is an in-process event here, and we don't dup(), so ... */
            }

            /* If we materialize the wait, return -1 fd later.
             * This is supported in Vulkan import. */
            if (edge->syncfd >= 0)
            {
                close(edge->syncfd);
                edge->syncfd = -1;
            }

            *edge = shmem->pending_edges[--shmem->pending_edges_count];
        }
        else
        {
            i++;
        }
    }

    /* Don't need to wake threads here.
     * - If pending_edges_count is full, we fail calls at call site with OUTOFMEMORY.
     * - Signaling does not change if a fence is considered materialized or not.
     */
}

static void kmt_device_complete_fence(kmt_device device, uint64_t order)
{
    struct kmt_device_shmem *shmem = device->shmem;
    struct kmt_fence_shmem_data *fence;
    struct kmt_pending_signal *signal;
    uint64_t signal_value;
    uint64_t fence_id;
    bool wakeup;

    pthread_mutex_lock(&shmem->lock);

    signal = kmt_device_find_pending_signal_locked(device, order);
    assert(signal);

    /* We might have a thread waiting for space in the pending signal list. */
    wakeup = shmem->pending_signal_count == KMT_DEVICE_MAX_PENDING_SIGNALS;

    signal_value = signal->value;
    fence_id = signal->fence_id;
    *signal = shmem->pending_signal[--shmem->pending_signal_count];

    fence = kmt_device_find_fence_shmem(device, fence_id);
    kmt_device_signal_fence_immediate_locked(device, fence, signal_value);

    if (wakeup)
        pthread_cond_broadcast(&shmem->cond);

    pthread_mutex_unlock(&shmem->lock);
}

void kmt_device_signal_fence_immediate(kmt_device device, kmt_handle fence, uint64_t value)
{
    struct kmt_device_shmem *shmem = device->shmem;
    pthread_mutex_lock(&shmem->lock);
    kmt_device_signal_fence_immediate_locked(device, fence->shmem, value);
    /* Could move a thread from blocked to fully signalled right away. */
    pthread_cond_broadcast(&shmem->cond);
    pthread_mutex_unlock(&shmem->lock);
}

bool kmt_device_register_sync_file(kmt_device device, kmt_handle fence, int fd, uint64_t value)
{
    struct kmt_device_shmem *shmem = device->shmem;
    struct kmt_pending_signal *pending_signal;
    bool has_materialization = false;
    uint64_t order;
    size_t i;

    pthread_mutex_lock(&shmem->lock);

    /* This should complete in finite time, or we have a device lost situation.
     * Could add timeouts here to check for that if need be. */
    while (shmem->pending_signal_count == KMT_DEVICE_MAX_PENDING_SIGNALS)
        pthread_cond_wait(&shmem->cond, &shmem->lock);

    order = vkd3d_atomic_uint64_increment(&device->shmem->order, vkd3d_memory_order_relaxed);

    pending_signal = &shmem->pending_signal[shmem->pending_signal_count++];
    pending_signal->order = order;
    pending_signal->fence_id = fence->shmem->id;
    pending_signal->value = value;

    for (i = 0; i < shmem->pending_edges_count; i++)
    {
        struct kmt_pending_edge *edge = &shmem->pending_edges[i];
        if (value >= edge->value && fence->shmem->id == edge->fence_id &&
            edge->eventfd < 0 && edge->syncfd < 0)
        {
            /* Can now unblock a thread waiting for materialization.
             * Use the first possible materialization. */
            edge->syncfd = dup(fd);
            has_materialization = true;
        }
    }

    if (has_materialization)
        pthread_cond_broadcast(&shmem->cond);

    pthread_mutex_unlock(&shmem->lock);

    /* Server side only. For a multi-process setup we would send FD to server with order number. */
    {
        struct kmt_epoll_data *data;
        struct epoll_event ev = {0};
        int ret;

        data = vkd3d_calloc(1, sizeof(*data));
        data->order = order;
        data->fd = fd;
        ev.data.ptr = data;

        ev.events = EPOLLIN;
        ret = epoll_ctl(device->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

        if (ret < 0)
        {
            vkd3d_free(data);
            /* FIXME: Deal with errors more gracefully here. */
            return false;
        }
    }

    return true;
}

static void *kmt_device_epoll_main(void *opaque_data)
{
    struct kmt_epoll_data *wake_buffer = NULL;
    struct epoll_event *events = NULL;
    kmt_device device = opaque_data;
    size_t wake_buffer_size = 0;
    int wake_buffer_count = 0;
    size_t events_size = 0;
    bool alive = true;
    int i;

    vkd3d_set_thread_name("epoll");
    vkd3d_array_reserve((void **)&events, &events_size, 64, sizeof(*events));

    /* All waits for sync_file are contained to this event loop.
     * This guarantees global signal ordering.
     */

    while (alive)
    {
        int ret = epoll_wait(device->epoll_fd, events, events_size, -1);

        /* Spurious wakeup. */
        if (ret == -1 && errno == EINTR)
            continue;

        if (ret <= 0)
            break;

        if ((size_t)ret == events_size)
        {
            /* We might have lost some events. For signal ordering, that's not acceptable.
             * Bump the size and try again. */
            vkd3d_array_reserve((void **)&events, &events_size, events_size * 2, sizeof(*events));
            continue;
        }

        vkd3d_array_reserve((void **)&wake_buffer, &wake_buffer_size, ret, sizeof(*wake_buffer));
        wake_buffer_count = 0;

        for (i = 0; i < ret; i++)
        {
            if (!events[i].data.ptr)
            {
                struct kmt_epoll_data data = {};
                while (read(device->poll_fd, &data, sizeof(data)) == sizeof(data))
                {
                    if (data.order == 0)
                        alive = false;
                    else
                        wake_buffer[wake_buffer_count++] = data;
                }
            }
            else
            {
                struct kmt_epoll_data *ptr = events[i].data.ptr;
                wake_buffer[wake_buffer_count++] = *ptr;
                vkd3d_free(ptr);
            }
        }

        /* Ensure signal order. If a signal was registered before another, and both events are signaled,
         * we must preserve the global ordering. */
        qsort(wake_buffer, wake_buffer_count, sizeof(*wake_buffer), wake_buffer_sort_cb);

        for (i = 0; i < wake_buffer_count; i++)
        {
            const struct kmt_epoll_data *wake = &wake_buffer[i];
            kmt_device_complete_fence(device, wake->order);

            if (wake->fd >= 0)
            {
                epoll_ctl(device->epoll_fd, EPOLL_CTL_DEL, wake->fd, NULL);
                close(wake->fd);
            }
        }
    }

    return NULL;
}

kmt_device kmt_device_create(void)
{
    kmt_device device = vkd3d_calloc(1, sizeof(*device));
    int fds[2];

    if (!device)
        return NULL;

    /* The epoll loop is supposed to run on the server process.
     * Client processes should have a unix socket or something like that
     * that can be used to send the occasional sync_file. */
    if (pipe2(fds, O_CLOEXEC) < 0)
        goto err;

    /* Writer must be blocking so we avoid losing wakeups spuriously under pressure. */
    if (fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK) < 0)
        goto close_pipe;

    device->poll_fd = fds[1];
    device->wake_fd = fds[0];
    device->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    /* Replace with shmem flinging. */
    device->shmem = vkd3d_calloc(1, sizeof(*device->shmem));
    pthread_mutex_init(&device->shmem->lock, NULL);
    pthread_cond_init(&device->shmem->cond, NULL);

    pthread_mutex_init(&device->fence_lock, NULL);

    if (pthread_create(&device->epoll_thread, NULL, kmt_device_epoll_main, device))
        goto fail_thread;

    return device;

fail_thread:
    close(device->epoll_fd);
close_pipe:
    close(fds[0]);
    close(fds[1]);
err:
    vkd3d_free(device);
    return NULL;
}

void kmt_device_destroy(kmt_device device)
{
    struct kmt_epoll_data dummy = {0};
    size_t i;

    write(device->wake_fd, &dummy, sizeof(dummy));
    pthread_join(device->epoll_thread, NULL);

    pthread_mutex_destroy(&device->fence_lock);
    pthread_mutex_destroy(&device->shmem->lock);
    pthread_cond_destroy(&device->shmem->cond);
    for (i = 0; i < device->fences_count; i++)
        vkd3d_free(device->fences[i]);
    vkd3d_free(device->fences);

    close(device->epoll_fd);
    close(device->poll_fd);
    close(device->wake_fd);
    vkd3d_free(device);
}

static void kmt_device_register_fence(kmt_device device, struct kmt_fence_shmem_data *shmem)
{
    shmem->id = vkd3d_atomic_uint64_increment(&device->shmem->order, vkd3d_memory_order_relaxed);
    pthread_mutex_lock(&device->fence_lock);
    vkd3d_array_reserve((void **)&device->fences, &device->fences_size,
                        device->fences_count + 1, sizeof(*device->fences));
    device->fences[device->fences_count++] = shmem;
    pthread_mutex_unlock(&device->fence_lock);
}

kmt_handle kmt_device_create_fence(kmt_device device, uint64_t initial_value)
{
    struct kmt_fence_shmem_data *shmem;
    kmt_handle fence;
    fence = vkd3d_calloc(1, sizeof(*fence));
    if (!fence)
        return NULL;

    /* Cross-process style, replace this alloc with a server request for FD to shmem. */
    shmem = vkd3d_calloc(1, sizeof(*shmem));
    shmem->current_value = initial_value;
    kmt_device_register_fence(device, shmem);

    fence->shmem = shmem;
    return fence;
}

void kmt_device_destroy_fence(kmt_device device, kmt_handle fence)
{
    uint64_t id = fence->shmem->id;
    size_t i;

    /* Cross-process consideration. We cannot touch device directly here.
     * It might be possible to detect global fence destruction through other means,
     * e.g. a FD could represent ownership and if the server process detects that the FD is closed
     * on the other end, the fence reference is dropped.
     * Once all references to a fence are dropped entirely, all waiters for that fence are automatically
     * unblocked somehow.
     */
    pthread_mutex_lock(&device->fence_lock);
    for (i = 0; i < device->fences_count; i++)
    {
        if (device->fences[i]->id == id)
        {
            device->fences[i] = device->fences[--device->fences_count];
            break;
        }
    }
    pthread_mutex_unlock(&device->fence_lock);

    /* Replace with munmap. */
    vkd3d_free(fence->shmem);
    vkd3d_free(fence);
}
