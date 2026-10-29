/*
 * Copyright 2026 the amdgpu-wddm fork of vkd3d-proton
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

/* amdgpu-wddm fork: the admission rules of the pipeline compile limiter (struct vkd3d_pipeline_compile_limiter in
 * vkd3d_private.h), without a lock, a clock or a device, so that a host test (libs/ddi/compile_admission_test.c) can
 * drive them. The limiter calls these functions under its lock.
 *
 * Three kinds of caller:
 * - Ordinary: a pool compile. It takes a ticket and goes in ticket order, within the fixed bound and the thermal band.
 * - Exempt: a variant compiled at draw time. It takes no ticket and goes at once, past the bound and the band.
 * - Urgent: a creation that the frame waits for (BD-226). The D3D12 driver marks the thread that makes it
 *   (bc250_vkd3d_engine.h, V14), and it goes at once, past the bound and the band, as an exempt caller does. A caller
 *   that is urgent when it arrives takes no ticket. A caller that becomes urgent while it waits already holds a
 *   ticket: it goes out of order, and its ticket is kept in "skipped", so that the ticket order passes over it when
 *   it comes to that ticket. With no room left in "skipped" the caller keeps its place in the order, which is what
 *   it had before it was marked.
 *
 * Before BD-226 a pool compile that the frame waited for stood in the ticket order behind up to ten other pool
 * compiles, one at a time in the thermal band: in Clair Obscur: Expedition 33 the render thread waited up to 831 ms
 * for one pipeline inside a 1,358 ms frame (OWN3, 2026-10-23), with the shell's pool showing every waited job as
 * already started. */

#ifndef __VKD3D_COMPILE_ADMISSION_H
#define __VKD3D_COMPILE_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

/* Threads marked urgent at the same time. A frame has one or two threads that wait for pipelines, and a mark lasts
 * until the creation ends, so a few entries are plenty; a mark that finds the table full is dropped, and that
 * creation keeps its place in the order. */
#define VKD3D_COMPILE_URGENT_MAX 16u
/* Tickets admitted out of order and not yet passed by the ticket order. */
#define VKD3D_COMPILE_SKIPPED_MAX 64u

struct vkd3d_compile_admission
{
    uint32_t slots;             /* the fixed bound */
    uint32_t in_flight;         /* admitted and not yet released, of every kind */
    uint64_t next_ticket;       /* the next ordinary caller's ticket */
    uint64_t next_admit;        /* the oldest ticket not yet admitted */
    uint32_t urgent_count;
    uint32_t urgent_threads[VKD3D_COMPILE_URGENT_MAX];
    uint32_t skipped_count;
    uint64_t skipped[VKD3D_COMPILE_SKIPPED_MAX];
};

enum vkd3d_compile_decision
{
    VKD3D_COMPILE_WAIT_BOUND,   /* every slot of the bound is in use */
    VKD3D_COMPILE_WAIT_ORDER,   /* an older ticket goes first */
    VKD3D_COMPILE_WAIT_THERMAL, /* the thermal band allows fewer compiles than are in flight */
    VKD3D_COMPILE_ADMIT,        /* in order, within the bound and the band */
    VKD3D_COMPILE_ADMIT_EXEMPT, /* at once: a draw-time variant */
    VKD3D_COMPILE_ADMIT_URGENT, /* at once: a creation the frame waits for */
};

static inline bool vkd3d_compile_decision_admits(enum vkd3d_compile_decision decision)
{
    return decision >= VKD3D_COMPILE_ADMIT;
}

static inline void vkd3d_compile_admission_init(struct vkd3d_compile_admission *a, uint32_t slots)
{
    uint32_t i;

    a->slots = slots;
    a->in_flight = 0;
    a->next_ticket = 0;
    a->next_admit = 0;
    a->urgent_count = 0;
    for (i = 0; i < VKD3D_COMPILE_URGENT_MAX; ++i)
        a->urgent_threads[i] = 0;
    a->skipped_count = 0;
    for (i = 0; i < VKD3D_COMPILE_SKIPPED_MAX; ++i)
        a->skipped[i] = 0;
}

static inline bool vkd3d_compile_admission_is_urgent(const struct vkd3d_compile_admission *a, uint32_t thread_id)
{
    uint32_t i;

    if (!thread_id)
        return false;
    for (i = 0; i < a->urgent_count; ++i)
    {
        if (a->urgent_threads[i] == thread_id)
            return true;
    }
    return false;
}

/* Marks thread_id urgent or clears its mark. Returns whether the table changed; a mark that finds the table full,
 * or the thread already marked, changes nothing. Thread 0 is no thread. */
static inline bool vkd3d_compile_admission_mark(struct vkd3d_compile_admission *a, uint32_t thread_id, bool urgent)
{
    uint32_t i;

    if (!thread_id)
        return false;
    for (i = 0; i < a->urgent_count; ++i)
    {
        if (a->urgent_threads[i] != thread_id)
            continue;
        if (urgent)
            return false;
        a->urgent_threads[i] = a->urgent_threads[--a->urgent_count];
        a->urgent_threads[a->urgent_count] = 0;
        return true;
    }
    if (!urgent || a->urgent_count >= VKD3D_COMPILE_URGENT_MAX)
        return false;
    a->urgent_threads[a->urgent_count++] = thread_id;
    return true;
}

/* An ordinary caller's ticket. */
static inline uint64_t vkd3d_compile_admission_take_ticket(struct vkd3d_compile_admission *a)
{
    return a->next_ticket++;
}

/* May the caller go now? has_ticket: an ordinary caller, or an urgent one that was marked after it took its ticket.
 * thermal_slots is consulted only for an ordinary caller whose turn it is within the bound, because reading it may
 * read the die temperature; NULL means no band. */
static inline enum vkd3d_compile_decision vkd3d_compile_admission_decide(const struct vkd3d_compile_admission *a,
        bool exempt, bool urgent, bool has_ticket, uint64_t ticket,
        uint32_t (*thermal_slots)(void *ctx), void *ctx)
{
    if (exempt)
        return VKD3D_COMPILE_ADMIT_EXEMPT;
    /* An urgent ticket that is not the next one needs room in "skipped"; without it the caller waits its turn. */
    if (urgent && (!has_ticket || ticket == a->next_admit || a->skipped_count < VKD3D_COMPILE_SKIPPED_MAX))
        return VKD3D_COMPILE_ADMIT_URGENT;
    if (a->in_flight >= a->slots)
        return VKD3D_COMPILE_WAIT_BOUND;
    if (!has_ticket || ticket != a->next_admit)
        return VKD3D_COMPILE_WAIT_ORDER;
    if (thermal_slots && a->in_flight >= thermal_slots(ctx))
        return VKD3D_COMPILE_WAIT_THERMAL;
    return VKD3D_COMPILE_ADMIT;
}

/* The ticket order passes over tickets that went out of order. */
static inline void vkd3d_compile_admission_pass_skipped(struct vkd3d_compile_admission *a)
{
    bool found;
    uint32_t i;

    do
    {
        found = false;
        for (i = 0; i < a->skipped_count; ++i)
        {
            if (a->skipped[i] != a->next_admit)
                continue;
            a->skipped[i] = a->skipped[--a->skipped_count];
            a->skipped[a->skipped_count] = 0;
            a->next_admit++;
            found = true;
            break;
        }
    } while (found);
}

/* Admits the caller after a decision that admits. A ticket of an urgent caller that is not the next one goes into
 * "skipped" (vkd3d_compile_admission_decide made sure there is room). */
static inline void vkd3d_compile_admission_admit(struct vkd3d_compile_admission *a,
        enum vkd3d_compile_decision decision, bool has_ticket, uint64_t ticket)
{
    a->in_flight++;
    if (!has_ticket || decision == VKD3D_COMPILE_ADMIT_EXEMPT)
        return;
    if (ticket == a->next_admit)
        a->next_admit++;
    else
        a->skipped[a->skipped_count++] = ticket;
    vkd3d_compile_admission_pass_skipped(a);
}

static inline void vkd3d_compile_admission_release(struct vkd3d_compile_admission *a)
{
    if (a->in_flight)
        a->in_flight--;
}

/* Whether ordinary callers wait whose turn may have come: tickets handed out and not yet admitted. */
static inline bool vkd3d_compile_admission_has_waiters(const struct vkd3d_compile_admission *a)
{
    return a->next_ticket != a->next_admit;
}

/* V14 of libs/ddi/bc250_vkd3d_engine.h: the private data that marks a creation urgent. The engine header is the
 * contract (BC250_VKD3D_COMPILE_URGENCY_GUID_INIT, BC250_VKD3D_COMPILE_URGENCY); libvkd3d does not include it, so
 * the GUID and the layout are repeated here, and libs/ddi/compile_admission_test.c fails when the two differ. */
#define VKD3D_COMPILE_URGENCY_GUID_INIT \
        {0xdcbd70e7, 0x16da, 0x474e, {0x8f, 0xe9, 0x3a, 0x60, 0xc0, 0xcf, 0x2b, 0x71}}
/* Size, ThreadId, Urgent: three little-endian 32-bit fields. */
#define VKD3D_COMPILE_URGENCY_SIZE 12u

/* Decodes a V14 payload. False for a NULL payload, a size below VKD3D_COMPILE_URGENCY_SIZE, a Size field below it or
 * above data_size, or thread 0. A larger Size (a later revision) is accepted, and its new fields are ignored. */
static inline bool vkd3d_compile_urgency_decode(const void *data, uint32_t data_size, uint32_t *thread_id,
        bool *urgent)
{
    const uint8_t *bytes = data;
    uint32_t fields[3];
    uint32_t i;

    if (!data || data_size < VKD3D_COMPILE_URGENCY_SIZE)
        return false;
    for (i = 0; i < 3; ++i)
    {
        fields[i] = (uint32_t)bytes[4 * i] | (uint32_t)bytes[4 * i + 1] << 8
                | (uint32_t)bytes[4 * i + 2] << 16 | (uint32_t)bytes[4 * i + 3] << 24;
    }
    if (fields[0] < VKD3D_COMPILE_URGENCY_SIZE || fields[0] > data_size || !fields[1])
        return false;
    *thread_id = fields[1];
    *urgent = !!fields[2];
    return true;
}

#endif /* __VKD3D_COMPILE_ADMISSION_H */
