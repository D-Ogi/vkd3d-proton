/* SPDX-License-Identifier: MIT
 *
 * Host test of the admission rules of the pipeline compile limiter (vkd3d_compile_admission.h) and of the V14 private
 * data of bc250_vkd3d_engine.h: urgent creations (BD-226). No GPU, no device, no thread. Prints PASSED or FAILED and
 * returns 0 or 1.
 */

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bc250_vkd3d_engine.h"
#include "vkd3d_compile_admission.h"

static unsigned int failures;

#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint32_t thermal_slots_value;

static uint32_t thermal_slots(void *ctx)
{
    (void)ctx;
    return thermal_slots_value;
}

static enum vkd3d_compile_decision decide(const struct vkd3d_compile_admission *a, bool exempt, uint32_t thread_id,
        bool has_ticket, uint64_t ticket)
{
    return vkd3d_compile_admission_decide(a, exempt, vkd3d_compile_admission_is_urgent(a, thread_id), has_ticket,
            ticket, thermal_slots, NULL);
}

/* Decides and, when the decision admits, admits. Returns the decision. */
static enum vkd3d_compile_decision try_admit(struct vkd3d_compile_admission *a, bool exempt, uint32_t thread_id,
        bool has_ticket, uint64_t ticket)
{
    enum vkd3d_compile_decision decision = decide(a, exempt, thread_id, has_ticket, ticket);

    if (vkd3d_compile_decision_admits(decision))
        vkd3d_compile_admission_admit(a, decision, has_ticket, ticket);
    return decision;
}

/* Ordinary callers go in ticket order, within the bound and the thermal band. */
static void test_order_and_bound(void)
{
    struct vkd3d_compile_admission a;
    uint64_t t[4];
    unsigned int i;

    vkd3d_compile_admission_init(&a, 2);
    thermal_slots_value = 2;
    for (i = 0; i < 4; ++i)
        t[i] = vkd3d_compile_admission_take_ticket(&a);
    /* A later ticket never goes before an older one. */
    CHECK(decide(&a, false, 0, true, t[1]) == VKD3D_COMPILE_WAIT_ORDER);
    CHECK(try_admit(&a, false, 0, true, t[0]) == VKD3D_COMPILE_ADMIT);
    CHECK(try_admit(&a, false, 0, true, t[1]) == VKD3D_COMPILE_ADMIT);
    CHECK(a.in_flight == 2);
    CHECK(decide(&a, false, 0, true, t[2]) == VKD3D_COMPILE_WAIT_BOUND);
    CHECK(vkd3d_compile_admission_has_waiters(&a));
    vkd3d_compile_admission_release(&a);
    CHECK(try_admit(&a, false, 0, true, t[2]) == VKD3D_COMPILE_ADMIT);
    /* The thermal band: one slot in flight is all that the band allows. */
    vkd3d_compile_admission_release(&a);
    thermal_slots_value = 1;
    CHECK(decide(&a, false, 0, true, t[3]) == VKD3D_COMPILE_WAIT_THERMAL);
    vkd3d_compile_admission_release(&a);
    CHECK(try_admit(&a, false, 0, true, t[3]) == VKD3D_COMPILE_ADMIT);
    CHECK(!vkd3d_compile_admission_has_waiters(&a));
    vkd3d_compile_admission_release(&a);
    CHECK(a.in_flight == 0);
    /* A release below zero stays at zero. */
    vkd3d_compile_admission_release(&a);
    CHECK(a.in_flight == 0);
}

/* A draw-time variant goes at once, past the bound and the band, and does not change the order. */
static void test_exempt(void)
{
    struct vkd3d_compile_admission a;
    uint64_t t0, t1, t2;

    vkd3d_compile_admission_init(&a, 2);
    thermal_slots_value = 1;
    t0 = vkd3d_compile_admission_take_ticket(&a);
    t1 = vkd3d_compile_admission_take_ticket(&a);
    t2 = vkd3d_compile_admission_take_ticket(&a);
    CHECK(try_admit(&a, false, 0, true, t0) == VKD3D_COMPILE_ADMIT);
    CHECK(decide(&a, false, 0, true, t1) == VKD3D_COMPILE_WAIT_THERMAL);
    CHECK(try_admit(&a, true, 0, false, 0) == VKD3D_COMPILE_ADMIT_EXEMPT);
    CHECK(try_admit(&a, true, 0, false, 0) == VKD3D_COMPILE_ADMIT_EXEMPT);
    CHECK(a.in_flight == 3);
    CHECK(a.next_admit == t1);
    vkd3d_compile_admission_release(&a);
    vkd3d_compile_admission_release(&a);
    vkd3d_compile_admission_release(&a);
    CHECK(try_admit(&a, false, 0, true, t1) == VKD3D_COMPILE_ADMIT);
    CHECK(decide(&a, false, 0, true, t2) == VKD3D_COMPILE_WAIT_THERMAL);
}

/* The urgent thread table: duplicates, clears, a full table and thread 0. */
static void test_marks(void)
{
    struct vkd3d_compile_admission a;
    uint32_t i;

    vkd3d_compile_admission_init(&a, 2);
    CHECK(!vkd3d_compile_admission_mark(&a, 0, true));
    CHECK(!vkd3d_compile_admission_is_urgent(&a, 0));
    CHECK(vkd3d_compile_admission_mark(&a, 100, true));
    CHECK(!vkd3d_compile_admission_mark(&a, 100, true));
    CHECK(a.urgent_count == 1);
    CHECK(vkd3d_compile_admission_is_urgent(&a, 100));
    CHECK(!vkd3d_compile_admission_mark(&a, 101, false));
    CHECK(vkd3d_compile_admission_mark(&a, 100, false));
    CHECK(!vkd3d_compile_admission_is_urgent(&a, 100));
    CHECK(a.urgent_count == 0);
    for (i = 1; i <= VKD3D_COMPILE_URGENT_MAX; ++i)
        CHECK(vkd3d_compile_admission_mark(&a, i, true));
    CHECK(!vkd3d_compile_admission_mark(&a, VKD3D_COMPILE_URGENT_MAX + 1, true));
    CHECK(!vkd3d_compile_admission_is_urgent(&a, VKD3D_COMPILE_URGENT_MAX + 1));
    /* A clear in the middle keeps the others. */
    CHECK(vkd3d_compile_admission_mark(&a, 3, false));
    CHECK(!vkd3d_compile_admission_is_urgent(&a, 3));
    for (i = 1; i <= VKD3D_COMPILE_URGENT_MAX; ++i)
    {
        if (i != 3)
            CHECK(vkd3d_compile_admission_is_urgent(&a, i));
    }
}

/* BD-226: the shell's pool had eleven threads, so the job the frame waited for could hold the last of eleven
 * tickets, with the thermal band at one slot. Without a mark it went after ten other compiles; with a mark it goes
 * at once, and the ten others still go in their order after it. */
static void test_waited_job(void)
{
    enum { JOBS = 11, WAITED = JOBS - 1, WAITED_THREAD = 4242 };
    struct vkd3d_compile_admission a;
    unsigned int i, before = 0;
    uint64_t t[JOBS];

    /* Without the mark. */
    vkd3d_compile_admission_init(&a, 2);
    thermal_slots_value = 1;
    for (i = 0; i < JOBS; ++i)
        t[i] = vkd3d_compile_admission_take_ticket(&a);
    for (i = 0; i < JOBS; ++i)
    {
        if (vkd3d_compile_decision_admits(decide(&a, false, WAITED_THREAD, true, t[WAITED])))
            break;
        CHECK(try_admit(&a, false, 0, true, t[i]) == VKD3D_COMPILE_ADMIT);
        vkd3d_compile_admission_release(&a);
        ++before;
    }
    CHECK(before == WAITED);

    /* With the mark: the waited job already holds its ticket and waits. */
    vkd3d_compile_admission_init(&a, 2);
    thermal_slots_value = 1;
    for (i = 0; i < JOBS; ++i)
        t[i] = vkd3d_compile_admission_take_ticket(&a);
    CHECK(try_admit(&a, false, 0, true, t[0]) == VKD3D_COMPILE_ADMIT);
    CHECK(!vkd3d_compile_decision_admits(decide(&a, false, WAITED_THREAD, true, t[WAITED])));
    CHECK(vkd3d_compile_admission_mark(&a, WAITED_THREAD, true));
    CHECK(try_admit(&a, false, WAITED_THREAD, true, t[WAITED]) == VKD3D_COMPILE_ADMIT_URGENT);
    CHECK(a.in_flight == 2);
    CHECK(a.skipped_count == 1);
    /* The mark ends with the creation. */
    CHECK(vkd3d_compile_admission_mark(&a, WAITED_THREAD, false));
    vkd3d_compile_admission_release(&a);
    /* The others keep their order and the band. */
    CHECK(decide(&a, false, 0, true, t[2]) == VKD3D_COMPILE_WAIT_ORDER);
    vkd3d_compile_admission_release(&a);
    for (i = 1; i < WAITED; ++i)
    {
        CHECK(try_admit(&a, false, 0, true, t[i]) == VKD3D_COMPILE_ADMIT);
        CHECK(decide(&a, false, 0, true, i + 1 < WAITED ? t[i + 1] : t[i]) != VKD3D_COMPILE_ADMIT);
        vkd3d_compile_admission_release(&a);
    }
    /* The order passed over the waited job's ticket: nobody waits, and the next ticket goes. */
    CHECK(a.skipped_count == 0);
    CHECK(a.next_admit == JOBS);
    CHECK(!vkd3d_compile_admission_has_waiters(&a));
    t[0] = vkd3d_compile_admission_take_ticket(&a);
    CHECK(try_admit(&a, false, 0, true, t[0]) == VKD3D_COMPILE_ADMIT);
}

/* An urgent caller that comes marked takes no ticket and goes at once; the ticket order does not see it. */
static void test_urgent_on_arrival(void)
{
    struct vkd3d_compile_admission a;
    uint64_t t0, t1;

    vkd3d_compile_admission_init(&a, 1);
    thermal_slots_value = 1;
    t0 = vkd3d_compile_admission_take_ticket(&a);
    t1 = vkd3d_compile_admission_take_ticket(&a);
    CHECK(try_admit(&a, false, 0, true, t0) == VKD3D_COMPILE_ADMIT);
    CHECK(vkd3d_compile_admission_mark(&a, 7, true));
    CHECK(try_admit(&a, false, 7, false, 0) == VKD3D_COMPILE_ADMIT_URGENT);
    CHECK(a.skipped_count == 0);
    CHECK(a.next_admit == t1);
    vkd3d_compile_admission_release(&a);
    vkd3d_compile_admission_release(&a);
    CHECK(try_admit(&a, false, 0, true, t1) == VKD3D_COMPILE_ADMIT);
}

/* An urgent ticket whose turn it is goes in order and leaves nothing in "skipped". */
static void test_urgent_in_turn(void)
{
    struct vkd3d_compile_admission a;
    uint64_t t0;

    vkd3d_compile_admission_init(&a, 1);
    thermal_slots_value = 1;
    t0 = vkd3d_compile_admission_take_ticket(&a);
    CHECK(vkd3d_compile_admission_mark(&a, 9, true));
    CHECK(try_admit(&a, false, 9, true, t0) == VKD3D_COMPILE_ADMIT_URGENT);
    CHECK(a.skipped_count == 0);
    CHECK(a.next_admit == 1);
}

/* With "skipped" full an urgent ticket keeps its place in the order. */
static void test_skipped_full(void)
{
    struct vkd3d_compile_admission a;
    uint64_t t[VKD3D_COMPILE_SKIPPED_MAX + 2];
    uint32_t i;

    vkd3d_compile_admission_init(&a, 1);
    thermal_slots_value = 1;
    for (i = 0; i < VKD3D_COMPILE_SKIPPED_MAX + 2; ++i)
        t[i] = vkd3d_compile_admission_take_ticket(&a);
    /* Tickets 1 to SKIPPED_MAX go out of order, one thread each. */
    for (i = 1; i <= VKD3D_COMPILE_SKIPPED_MAX; ++i)
    {
        CHECK(vkd3d_compile_admission_mark(&a, 1000 + i, true));
        CHECK(try_admit(&a, false, 1000 + i, true, t[i]) == VKD3D_COMPILE_ADMIT_URGENT);
        CHECK(vkd3d_compile_admission_mark(&a, 1000 + i, false));
        vkd3d_compile_admission_release(&a);
    }
    CHECK(a.skipped_count == VKD3D_COMPILE_SKIPPED_MAX);
    CHECK(vkd3d_compile_admission_mark(&a, 5000, true));
    CHECK(decide(&a, false, 5000, true, t[VKD3D_COMPILE_SKIPPED_MAX + 1]) == VKD3D_COMPILE_WAIT_ORDER);
    /* Ticket 0 goes; the order passes every skipped ticket and reaches the last one, which goes as urgent in turn. */
    CHECK(try_admit(&a, false, 0, true, t[0]) == VKD3D_COMPILE_ADMIT);
    CHECK(a.skipped_count == 0);
    CHECK(a.next_admit == t[VKD3D_COMPILE_SKIPPED_MAX + 1]);
    CHECK(try_admit(&a, false, 5000, true, t[VKD3D_COMPILE_SKIPPED_MAX + 1]) == VKD3D_COMPILE_ADMIT_URGENT);
    CHECK(!vkd3d_compile_admission_has_waiters(&a));
}

static void put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = value & 0xff;
    bytes[1] = value >> 8 & 0xff;
    bytes[2] = value >> 16 & 0xff;
    bytes[3] = value >> 24 & 0xff;
}

/* The V14 payload: the engine header's structure and GUID, and the engine's decoder. */
static void test_v14_contract(void)
{
    static const GUID engine_guid = BC250_VKD3D_COMPILE_URGENCY_GUID_INIT;
    static const GUID limiter_guid = VKD3D_COMPILE_URGENCY_GUID_INIT;
    BC250_VKD3D_COMPILE_URGENCY urgency;
    uint32_t thread_id = 0;
    uint8_t bytes[16];
    bool urgent = false;

    CHECK(!memcmp(&engine_guid, &limiter_guid, sizeof(GUID)));
    CHECK(sizeof(BC250_VKD3D_COMPILE_URGENCY) == VKD3D_COMPILE_URGENCY_SIZE);
    CHECK(offsetof(BC250_VKD3D_COMPILE_URGENCY, Size) == 0);
    CHECK(offsetof(BC250_VKD3D_COMPILE_URGENCY, ThreadId) == 4);
    CHECK(offsetof(BC250_VKD3D_COMPILE_URGENCY, Urgent) == 8);

    urgency.Size = sizeof(urgency);
    urgency.ThreadId = 0x12345678;
    urgency.Urgent = 1;
    CHECK(vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));
    CHECK(thread_id == 0x12345678 && urgent);
    urgency.Urgent = 0;
    CHECK(vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));
    CHECK(!urgent);
    /* Any nonzero value is urgent. */
    urgency.Urgent = 2;
    CHECK(vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));
    CHECK(urgent);

    CHECK(!vkd3d_compile_urgency_decode(NULL, sizeof(urgency), &thread_id, &urgent));
    CHECK(!vkd3d_compile_urgency_decode(&urgency, sizeof(urgency) - 1, &thread_id, &urgent));
    urgency.Size = sizeof(urgency) - 4;
    CHECK(!vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));
    urgency.Size = sizeof(urgency) + 4;
    CHECK(!vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));
    urgency.Size = sizeof(urgency);
    urgency.ThreadId = 0;
    CHECK(!vkd3d_compile_urgency_decode(&urgency, sizeof(urgency), &thread_id, &urgent));

    /* A later revision's larger payload: the added field is ignored. */
    put32(bytes, 16);
    put32(bytes + 4, 77);
    put32(bytes + 8, 1);
    put32(bytes + 12, 0xffffffff);
    CHECK(vkd3d_compile_urgency_decode(bytes, sizeof(bytes), &thread_id, &urgent));
    CHECK(thread_id == 77 && urgent);
}

int main(void)
{
    test_order_and_bound();
    test_exempt();
    test_marks();
    test_waited_job();
    test_urgent_on_arrival();
    test_urgent_in_turn();
    test_skipped_full();
    test_v14_contract();
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
