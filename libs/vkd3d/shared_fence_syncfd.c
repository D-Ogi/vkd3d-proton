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
    uint64_t fence_id;
    uint64_t order;
    int fd;
};

struct kmt_pending_edge
{
    uint64_t edge;
    uint64_t value;
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

static void *kmt_device_epoll_main(void* data)
{
    kmt_device device = data;

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
