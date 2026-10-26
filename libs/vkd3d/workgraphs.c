/*
 * Copyright 2024 Hans-Kristian Arntzen for Valve Corporation
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
#include "vkd3d_private.h"
#include "vkd3d_string.h"

#define WG_TRACE TRACE

#define MAX_WORKGRAPH_LEVELS 32
#define WG_DIVIDER (32u * 1024u)

/* In thread nodes, there's a maximum of 8 output records per thread,
 * and the internal implementation supports 256 outputs per wave, so we should clamp
 * number of threads to 32 to guarantee it works. Alternatively, we can of course
 * check dynamically, but that becomes very messy, very quickly. */
#define THREAD_COALESCE_COUNT 32u
/* Size of each payload ring of a work graph state object. */
#define VKD3D_WORKGRAPH_PAYLOAD_RING_SIZE (128ull * 1024 * 1024)
/* amdgpu-wddm fork: after each payload ring, the sink for the records of an allocation that does not fit
 * (struct vkd3d_workgraph_output_guard). One allocation is at most L records and 64 KiB - 15 bytes, with
 * L = min((64 KiB - 15) / stride, 256) (dxil-spirv, build_allocate_node_records), and the record addressing clamps
 * the record index to L - 1 (emit_build_output_payload_offset). A dropped allocation writes from the start of the
 * sink, so it ends at most L x stride after it. An allocation that fits ends at or before the capacity; the records
 * of a lane start at or before the capacity (a trailing lane with count 0 starts at it), so a write with an index
 * past the count (which the application must not do) ends at most L x stride after the capacity. Both are at most
 * 64 KiB - 15 bytes. This holds for validated record types and layouts, not for malformed shader metadata. */
#define VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE (64ull * 1024)
/* The node output allocator computes payload offsets in 32 bits. */
STATIC_ASSERT(VKD3D_WORKGRAPH_PAYLOAD_RING_SIZE + VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE <= UINT32_MAX);
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_lost_records) <= VKD3D_WORKGRAPH_ABANDONED_OFFSET);
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_drain_control) == 76 * sizeof(uint32_t));
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_pool_level_args) == 144);
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_pool_admission) == 32);
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_workgroups_args) <= 64 && sizeof(struct vkd3d_workgraph_drain_args) <= 64);
STATIC_ASSERT(sizeof(struct vkd3d_workgraph_abandoned) == 20 * sizeof(uint32_t));
/* The packed offsets of the payloads, after the required scratch memory: their size with MinSizeInBytes and with
 * MaxSizeInBytes of backing memory. The sizes are arbitrary. */
#define VKD3D_WORKGRAPH_PACKED_OFFSETS_MIN_SIZE (64ull * 1024 * 1024)
#define VKD3D_WORKGRAPH_PACKED_OFFSETS_MAX_SIZE (256ull * 1024 * 1024)
/* 64k is arbitrary, but reasonable. */
#define VKD3D_WORKGRAPH_BACKING_GRANULARITY D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT

/* amdgpu-wddm fork: the components of a record pool of the draining scheduler: payload bytes, then packed entries and
 * unrolled offsets, which W counts in records. Pool L (1 <= L < num_levels) holds the input records of level L, which
 * level L - 1 writes. A record is retained as a whole, so there is no record header yet. The pools are computed at state
 * object creation; execution does not use them yet. */
enum d3d12_wg_pool_component
{
    D3D12_WG_POOL_PAYLOAD,
    D3D12_WG_POOL_ENTRIES,
    D3D12_WG_POOL_OFFSETS,
    D3D12_WG_POOL_COMPONENT_COUNT,
};

/* The bytes of one unit of each component (the payload is in bytes), and the alignment of each component region. */
static const uint32_t d3d12_wg_pool_unit_bytes[D3D12_WG_POOL_COMPONENT_COUNT] = { 1, 8, 4 };
static const uint32_t d3d12_wg_pool_alignment[D3D12_WG_POOL_COMPONENT_COUNT] = { 16, 8, 4 };

/* One producer item of a thread launch node is one wave of up to 64 threads. */
#define D3D12_WG_POOL_THREAD_ITEM 64u
/* The node output allocator charges each empty record 4 bytes of the payload capacity (dxil-spirv). */
#define D3D12_WG_POOL_EMPTY_RECORD_BYTES 4u

struct d3d12_wg_drain_region
{
    VkDeviceSize offset;
    VkDeviceSize size;
};

/* amdgpu-wddm fork: the regions of the backing memory for one backing size (d3d12_wg_drain_layout). The producers of
 * a level address their pool with one base and a 32-bit payload offset, and find the output guard 32 bytes before the
 * packed entries (dxil-spirv). So each pool half has one payload region with its sink directly after it, the packed
 * entries start where the fixed bookkeeping (which ends with the output guard) ends, and the entries and the offsets
 * are shared by the pools: the entries of pool L are not used after the payload expander of level L, and the offsets
 * not after level L. */
struct d3d12_wg_drain_layout
{
    VkDeviceSize backing_size; /* The size that the layout uses: the given size, at most pool_max_size. */
    struct d3d12_wg_drain_region fixed;
    struct d3d12_wg_drain_region entries;
    struct d3d12_wg_drain_region offsets;
    struct d3d12_wg_drain_region empty_state;
    struct d3d12_wg_drain_region payload[MAX_WORKGRAPH_LEVELS][2];
    struct d3d12_wg_drain_region sink[MAX_WORKGRAPH_LEVELS][2];
    struct d3d12_wg_drain_region staging;
};

struct d3d12_wg_state_object_program;
static bool d3d12_wg_drain_layout(const struct d3d12_wg_state_object_program *program, VkDeviceSize backing_size,
        struct d3d12_wg_drain_layout *layout);

/* amdgpu-wddm fork, lab flavour only: AMDGPU_WDDM_WG_FAIL=<point> makes the creation of a work graph state object
 * fail at that point, as if an allocation failed there, so that a test can run the cleanup of a partly built
 * program. The points are "slots" (the recursion depth slots), "program" (the per-slot arrays of a program) and
 * "pipeline:<n>" (before the pipelines of slot n of a program are created; the slots of the
 * earlier levels have their pipelines then). */
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
static bool d3d12_wg_fault_injected(const char *point, unsigned int index)
{
    char value[64], expected[64];

    if (!vkd3d_get_env_var("AMDGPU_WDDM_WG_FAIL", value, sizeof(value)))
        return false;
    if (index == UINT_MAX)
        snprintf(expected, sizeof(expected), "%s", point);
    else
        snprintf(expected, sizeof(expected), "%s:%u", point, index);
    if (strcmp(value, expected))
        return false;

    ERR("Work graph fault injection at %s.\n", expected);
    return true;
}
#else
static inline bool d3d12_wg_fault_injected(const char *point, unsigned int index)
{
    return false;
}
#endif

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
/* amdgpu-wddm fork, lab flavour only: GetPrivateData of a work graph state object with this GUID gives two UINT64:
 * the DispatchGraph calls that lost records, and the lost records (test_workgraph_multi_gpu_input_lost_records).
 * With a data size of three UINT64 or more, the third is 1 if the lost record count is a lower bound. */
static const GUID amdgpu_wddm_wg_status_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x05 } };

/* amdgpu-wddm fork, lab flavour only: GetPrivateData with this GUID gives struct amdgpu_wddm_wg_canaries of the
 * first program, for test_workgraph_output_bounds. The ranges exist only with AMDGPU_WDDM_WG_LIMITS=canary=1. */
static const GUID amdgpu_wddm_wg_canary_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x06 } };

/* amdgpu-wddm fork, lab flavour only: GetPrivateData with this GUID gives struct amdgpu_wddm_wg_drain of the first
 * program, for test_workgraph_drain_rounds. control_offset is the offset of struct vkd3d_workgraph_drain_control in
 * the backing memory, valid only if rounds is not 0. The last byte is the version of the struct (7: the round frame,
 * 10: with abandoned_records, 11: with the pool states). */
static const GUID amdgpu_wddm_wg_drain_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x0b } };

struct amdgpu_wddm_wg_drain
{
    uint32_t rounds;
    uint32_t pending_rounds;
    uint64_t control_offset;
    uint32_t abandoned_events;
    uint32_t abandoned_reasons;
    uint32_t abandoned_last_reason;
    uint32_t abandoned_flags;
    uint32_t hook_calls;
    uint32_t violations;
    /* The records that the record pools held at close, for all executions (whole records, struct
     * vkd3d_workgraph_abandoned). Entry losses are in the lost records, not here. */
    uint64_t abandoned_records;
    /* The pool states in the backing memory: pool_states of struct vkd3d_workgraph_pool_state, one for each level,
     * pool_state_stride bytes apart. Valid only if rounds is not 0. */
    uint64_t pool_state_offset;
    uint32_t pool_state_stride;
    uint32_t pool_states;
};

/* amdgpu-wddm fork, lab flavour only: GetPrivateData with this GUID gives struct amdgpu_wddm_wg_pools of the first
 * program, for test_workgraph_drain_layout: W and the record bound of each pool, Min and Max of the pool layout, and
 * the layouts with Min - 16 bytes (none), Min, Min + one granule + 8 bytes, Max and Max + one granule of backing
 * memory. The empty-record state has no size yet, so Min is not the final Min of the scheduler. The last byte is the
 * version of the struct (8: the layout of P2, 9: one payload region per pool half, shared entries and offsets). */
static const GUID amdgpu_wddm_wg_pool_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x09 } };

struct amdgpu_wddm_wg_region
{
    uint64_t offset;
    uint64_t size;
};

struct amdgpu_wddm_wg_pools
{
    uint32_t valid;
    uint32_t pools; /* The number of levels: pool L for 1 <= L < pools. */
    uint32_t execution; /* 1 if DispatchGraph uses the pool layout. */
    uint32_t padding;
    uint64_t fixed_size;
    uint64_t staging_size;
    uint64_t sink_size;
    uint64_t min_size;
    uint64_t max_size;
    uint64_t w[MAX_WORKGRAPH_LEVELS][D3D12_WG_POOL_COMPONENT_COUNT];
    uint64_t bound[MAX_WORKGRAPH_LEVELS][D3D12_WG_POOL_COMPONENT_COUNT];
    struct
    {
        uint64_t size; /* The backing size asked for. */
        uint64_t used; /* The backing size that the layout uses, or 0 if there is no layout for that size. */
        struct amdgpu_wddm_wg_region fixed;
        struct amdgpu_wddm_wg_region entries;
        struct amdgpu_wddm_wg_region offsets;
        struct amdgpu_wddm_wg_region empty_state;
        struct amdgpu_wddm_wg_region payload[MAX_WORKGRAPH_LEVELS][2];
        struct amdgpu_wddm_wg_region sink[MAX_WORKGRAPH_LEVELS][2];
        struct amdgpu_wddm_wg_region staging;
    } layouts[5];
};

/* amdgpu-wddm fork, lab flavour only: GetPrivateData with this GUID gives struct amdgpu_wddm_wg_slots of the first
 * program, for tests that put nodes at given slots of the distributor. With data NULL, it gives the size only. The
 * distributor runs one subgroup of subgroup_size invocations (the required subgroup size of its pipeline) and goes
 * through the slots in chunks of subgroup_size. wg_divider is 1 if the distributor splits the workgroups of each node
 * (VKD3D_WORKGRAPH_MAX_WGX_NO_PRIMARY_EXECUTION_THRESHOLD, or AMDGPU_WDDM_WG_DIVIDER=1 at device creation): the
 * shader specialization and the CPU side use the same value. node[i] is the node of node index i (GetNodeIndex): its
 * slot in the distributor and in the pool states, and the slot of the node whose input it shares, or ~0. The last byte
 * is the version of the struct (12). */
static const GUID amdgpu_wddm_wg_slot_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x0c } };

struct amdgpu_wddm_wg_slots
{
    uint32_t subgroup_size;
    uint32_t wg_divider;
    uint32_t slots; /* The slots of the distributor: one for each node and recursion depth. */
    uint32_t nodes; /* The entries of node[]. */
    struct
    {
        uint32_t slot;
        uint32_t share_slot;
    } node[];
};
#endif

/* amdgpu-wddm fork, lab flavour: the canary ranges of a program, offsets from the start of the backing memory.
 * Initialization fills them with VKD3D_WORKGRAPH_CANARY_PATTERN. Nothing writes them after that, so a test that
 * reads the backing memory back finds a write past a sink or past the unrolled offsets. sinks[] are the two payload
 * sinks (the sink of payload ring 0 and of payload ring 1), so that a test can see which sink got the writes of the
 * allocations that did not fit. */
#define VKD3D_WORKGRAPH_CANARY_SIZE (4096u)
#define VKD3D_WORKGRAPH_CANARY_PATTERN (0xc4a7c0deu)
#define VKD3D_WORKGRAPH_CANARY_COUNT 3
struct amdgpu_wddm_wg_canaries
{
    uint32_t count;
    uint32_t pattern;
    struct
    {
        uint64_t offset;
        uint64_t size;
    } ranges[VKD3D_WORKGRAPH_CANARY_COUNT], sinks[2];
};

/* amdgpu-wddm fork, lab flavour only:
 * AMDGPU_WDDM_WG_MULTI_INPUT=staging=<bytes>,staging_max=<bytes>,large=<bytes>,groups=<n> sets the staging size with
 * MinSizeInBytes and with MaxSizeInBytes of backing memory, the large input size and the number of classify and copy
 * workgroups of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT for the work graphs created after it is set. Each part is
 * optional. Small values let a test cross every limit of these passes with a few thousand records. */
struct d3d12_wg_multi_input_config
{
    VkDeviceSize staging_size;
    VkDeviceSize staging_max_size;
    /* AMDGPU_WDDM_WG_LIMITS=ring=<bytes>,packed=<entries> (lab flavour): the size of the payload rings and of the
     * unrolled offsets, and a smaller limit for the packed offset entries of a level. */
    VkDeviceSize ring_size;
    uint32_t packed_limit;
    /* canary=1: a canary range after each payload sink and after the unrolled offsets. */
    bool canaries;
    /* rounds=<n> (0 to VKD3D_WORKGRAPH_DRAIN_MAX_ROUNDS): the extra rounds of the draining scheduler.
     * pending=<k> (test hook): the pending dispatches of the rounds below k report waiting work. */
    uint32_t drain_rounds;
    uint32_t drain_pending;
    /* pool_records=<n>, pool_stride=<bytes> (test hook): each output with a payload counts n records of that stride in
     * the record pools, so that a test reaches the 64-bit bounds and Max = Min. Execution does not use the pools. */
    uint32_t pool_records;
    uint32_t pool_stride;
    /* pools=0: the extra rounds run on the payload rings, also if the record pools are valid. */
    bool pools;
    uint32_t large_size;
    uint32_t groups;
};

static void d3d12_wg_get_multi_input_config(struct d3d12_wg_multi_input_config *config)
{
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    char value[128], *p, *next;
    unsigned long long n;
#endif

    config->staging_size = VKD3D_WORKGRAPH_MULTI_INPUT_STAGING_SIZE;
    config->staging_max_size = VKD3D_WORKGRAPH_MULTI_INPUT_STAGING_MAX_SIZE;
    config->ring_size = VKD3D_WORKGRAPH_PAYLOAD_RING_SIZE;
    config->packed_limit = UINT32_MAX;
    config->canaries = false;
    config->drain_rounds = 0;
    config->drain_pending = 0;
    config->pool_records = 0;
    config->pool_stride = 0;
    config->pools = true;
    config->large_size = VKD3D_WORKGRAPH_MULTI_INPUT_LARGE_SIZE;
    config->groups = VKD3D_WORKGRAPH_MULTI_INPUT_GROUPS;

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (vkd3d_get_env_var("AMDGPU_WDDM_WG_LIMITS", value, sizeof(value)))
    {
        for (p = value; p && *p; p = next)
        {
            if ((next = strchr(p, ',')))
                *next++ = '\0';

            /* The payload offsets are 16-byte aligned. Values out of range keep the default. */
            if (!strncmp(p, "ring=", 5) && (n = strtoull(p + 5, NULL, 0)) >= 64
                    && n <= VKD3D_WORKGRAPH_PAYLOAD_RING_SIZE)
                config->ring_size = n & ~15ull;
            else if (!strncmp(p, "packed=", 7) && (n = strtoull(p + 7, NULL, 0)) <= UINT32_MAX)
                config->packed_limit = n;
            else if (!strcmp(p, "canary=1"))
                config->canaries = true;
            else if (!strncmp(p, "rounds=", 7) && (n = strtoull(p + 7, NULL, 0)) <= VKD3D_WORKGRAPH_DRAIN_MAX_ROUNDS)
                config->drain_rounds = n;
            else if (!strncmp(p, "pending=", 8) && (n = strtoull(p + 8, NULL, 0)) <= VKD3D_WORKGRAPH_DRAIN_MAX_ROUNDS + 1)
                config->drain_pending = n;
            else if (!strncmp(p, "pool_records=", 13) && (n = strtoull(p + 13, NULL, 0)) <= UINT32_MAX)
                config->pool_records = n;
            else if (!strncmp(p, "pool_stride=", 12) && (n = strtoull(p + 12, NULL, 0)) <= UINT32_MAX)
                config->pool_stride = n;
            else if (!strcmp(p, "pools=0"))
                config->pools = false;
            else
                WARN("Ignoring %s in AMDGPU_WDDM_WG_LIMITS.\n", p);
        }

        WARN("Work graph limits: payload ring %"PRIu64" bytes, packed offset limit %u, canaries %u, "
                "extra rounds %u, pending hook %u, pool records %u, pool stride %u, pools %u.\n", config->ring_size,
                config->packed_limit, config->canaries, config->drain_rounds, config->drain_pending, config->pool_records,
                config->pool_stride, config->pools);
    }

    if (!vkd3d_get_env_var("AMDGPU_WDDM_WG_MULTI_INPUT", value, sizeof(value)))
        return;

    for (p = value; p && *p; p = next)
    {
        if ((next = strchr(p, ',')))
            *next++ = '\0';

        /* The merge slots start at 16-byte offsets. Values out of range keep the default. */
        if (!strncmp(p, "staging=", 8) && (n = strtoull(p + 8, NULL, 0)) >= 16
                && n <= VKD3D_WORKGRAPH_MULTI_INPUT_STAGING_SIZE)
            config->staging_size = n & ~15ull;
        else if (!strncmp(p, "staging_max=", 12) && (n = strtoull(p + 12, NULL, 0)) >= 16
                && n <= VKD3D_WORKGRAPH_MULTI_INPUT_STAGING_MAX_SIZE)
            config->staging_max_size = n & ~15ull;
        else if (!strncmp(p, "large=", 6) && (n = strtoull(p + 6, NULL, 0)) >= 4
                && n <= VKD3D_WORKGRAPH_MULTI_INPUT_LARGE_SIZE)
            config->large_size = n;
        else if (!strncmp(p, "groups=", 7) && (n = strtoull(p + 7, NULL, 0)) >= 1
                && n <= VKD3D_WORKGRAPH_MULTI_INPUT_GROUPS)
            config->groups = n;
        else
            WARN("Ignoring %s in AMDGPU_WDDM_WG_MULTI_INPUT.\n", p);
    }

    /* The staging range does not get smaller with more backing memory. */
    if (config->staging_max_size < config->staging_size)
        config->staging_max_size = config->staging_size;

    WARN("Multi GPU input: staging %"PRIu64" to %"PRIu64" bytes, large %u bytes, %u groups.\n",
            config->staging_size, config->staging_max_size, config->large_size, config->groups);
#endif
}

/* amdgpu-wddm fork: "scratch:<n>" fails scratch allocation n (counted from 0) of a DispatchGraph call. Fault injection
 * is for tests that record on one thread, so one counter for all command lists is enough. */
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
static unsigned int d3d12_wg_scratch_ordinal;
#define d3d12_wg_scratch_fault_reset() (d3d12_wg_scratch_ordinal = 0)
#define d3d12_wg_scratch_fault_injected() d3d12_wg_fault_injected("scratch", d3d12_wg_scratch_ordinal++)
#else
#define d3d12_wg_scratch_fault_reset() ((void)0)
#define d3d12_wg_scratch_fault_injected() false
#endif

/* amdgpu-wddm fork: a failed scratch allocation marks the command list as out of memory, so that Close fails with
 * E_OUTOFMEMORY and the partly recorded graph never executes. */
static bool d3d12_command_list_workgraph_allocate_scratch(struct d3d12_command_list *list,
        enum vkd3d_scratch_pool_kind kind, VkDeviceSize size, VkDeviceSize alignment,
        struct vkd3d_scratch_allocation *allocation)
{
    if (d3d12_wg_scratch_fault_injected() ||
            !d3d12_command_allocator_allocate_scratch_memory(list->allocator, kind, size, alignment, ~0u, allocation))
    {
        d3d12_command_list_mark_out_of_memory(list, "work graph scratch memory");
        return false;
    }

    return true;
}

/* This is arbitrary, but we have a trade-off to make.
 * If amplification count is low, we spawn very few redundant threads,
 * but occupancy might be too low if number of records is low.
 * If number of records is very high however, it's probably okay to have a single workgroup
 * per record looping over the amplification grid, and having too large of an amplifcation
 * will hurt us. The shader emitting amplification rate can dynamically adjust between 1 and 1024 rate as needed. */
#define MAX_AMPLIFICATION_RATE 1024u

/* 48 bytes per node. */
struct d3d12_workgraph_indirect_command
{
    uint32_t primary_execute[3];
    uint32_t primary_linear_offset; /* Read by node as input metadata. */
    uint32_t secondary_execute[3];
    uint32_t secondary_linear_offset; /* Read by node as input metadata. */
    uint32_t end_elements; /* Read by node as input metadata in coalesce / thread mode. */
    uint32_t linear_offset_atomic; /* Used by expander to write unrolled data. */
    uint32_t padding[2];
};

/* The first 16 bytes are reserved for payload expander execution. */
#define WG_INDIRECT_COMMAND_OFFSET 16

struct d3d12_workgraph_node_shared_execution
{
    unsigned int node_pipeline_index;
    unsigned int node_payload_index;
};

struct d3d12_workgraph_level_execution
{
    unsigned int *nodes;
    size_t nodes_size;
    size_t nodes_count;

    struct d3d12_workgraph_node_shared_execution *shared_nodes;
    size_t shared_nodes_size;
    size_t shared_nodes_count;
};

struct d3d12_wg_state_object_pipeline
{
    /* This matters for entry point, otherwise, not. */
    VkPipeline vk_static_cpu_node_entry_pipeline;
    VkPipeline vk_cpu_node_entry_pipeline;
    VkPipeline vk_gpu_node_entry_pipeline;
    VkPipeline vk_non_entry_pipeline;
    /* layout is part of modules array */

    /* These can be overridden per graph in theory. */
    const WCHAR *name;
    UINT array_index;
};

struct d3d12_wg_payload_expander_meta
{
    uint8_t group_components;
    uint8_t group_components_u32;
    uint8_t rw_group_tracking;
    uint8_t group_compact_broadcast;
    uint16_t payload_stride;
    int16_t grid_offset_or_count;
};

/* Many similarities to ray-tracing state objects, but the implementations are vastly different
 * to the point that there will just be more of a headache to try merging the two implementations into one.
 * This object represents a single work graph. A state object may hold many work graphs. */
struct d3d12_wg_state_object_program
{
    const WCHAR *name;
    /* Need to add nodes with all the overrides as well. */
    struct d3d12_workgraph_level_execution levels[MAX_WORKGRAPH_LEVELS];
    unsigned int num_levels;

    struct d3d12_wg_state_object_pipeline *pipelines;
    /* size is equal to number of entry points. */
    unsigned int num_pipelines;

    /* Meta shader. distribute_workgroups.comp */
    struct vkd3d_workgraph_meta_pipeline_info workgroup_distributor;
    struct vkd3d_workgraph_meta_pipeline_info gpu_input_setup;
    struct vkd3d_workgraph_meta_pipeline_info payload_offset_expander;
    struct vkd3d_workgraph_meta_pipeline_info complete_compaction;
    /* amdgpu-wddm fork: D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT. */
    struct vkd3d_workgraph_meta_pipeline_info multi_gpu_input_prepare;
    struct vkd3d_workgraph_meta_pipeline_info multi_gpu_input_copy;

    bool compact_broadcast_nodes_with_max_grid;

    VkDeviceSize counters_scratch_offset;
    VkDeviceSize counters_scratch_size;
    VkDeviceSize indirect_commands_scratch_base_offset;
    VkDeviceSize indirect_commands_scratch_node_offset;
    VkDeviceSize indirect_commands_scratch_base_size;
    VkDeviceSize dividers_scratch_offset;
    VkDeviceSize dividers_scratch_size;
    VkDeviceSize share_mapping_scratch_offset;
    VkDeviceSize share_mapping_scratch_size;
    VkDeviceSize payload_expander_scratch_offset;
    VkDeviceSize payload_expander_scratch_size;
    /* amdgpu-wddm fork: the staging range of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT, with MinSizeInBytes and with
     * MaxSizeInBytes of backing memory. It is at the end of the backing memory, so each execution context has its
     * own (d3d12_command_list_workgraph_multi_input_staging). */
    VkDeviceSize multi_input_staging_min_size;
    VkDeviceSize multi_input_staging_max_size;
    /* amdgpu-wddm fork: MinSizeInBytes and MaxSizeInBytes of D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS. */
    VkDeviceSize min_backing_size;
    VkDeviceSize max_backing_size;
    /* amdgpu-wddm fork: the two payload rings and the unrolled offsets of the levels. They are in the backing
     * memory, so that each execution context has its own (WorkGraphs.md, Backing memory). Before, the state
     * object had one set, which executions of the graph on parallel queues shared. */
    VkDeviceSize payload0_scratch_offset;
    VkDeviceSize payload0_scratch_size;
    VkDeviceSize payload1_scratch_offset;
    VkDeviceSize payload1_scratch_size;
    VkDeviceSize unrolled_offsets_scratch_offset;
    VkDeviceSize unrolled_offsets_scratch_size;
    /* amdgpu-wddm fork: struct vkd3d_workgraph_drain_control, before the output guard, only if drain_rounds is not 0.
     * drain_rounds is the number of extra rounds, drain_pending the lab test hook (AMDGPU_WDDM_WG_LIMITS). */
    VkDeviceSize drain_control_scratch_offset;
    VkDeviceSize drain_control_scratch_size;
    uint32_t drain_rounds;
    uint32_t drain_pending;
    struct vkd3d_workgraph_meta_pipeline_info drain;
    /* amdgpu-wddm fork: the record pools of the draining scheduler. pool_w[L] is W of pool L, the componentwise
     * maximum of the W of the producer items of level L - 1. pool_bound[L] is the largest demand on pool L of one
     * record of level L - 1, all its groups and readers. pool_valid is false if the metadata gave no bound; the pools
     * are then not used. pool_sink_size is the sink after each payload region, with the lab canary.
     * pool_ignored_overrides is true if a node override that the engine does not apply (output budgets, shared input,
     * new name) is set: the metadata then does not describe the graph of the application, and the pools are not valid. */
    bool pool_valid;
    bool pool_ignored_overrides;
    /* pool_execution: the lab drain mode runs on the pools. The backing memory then has the pool layout, and the
     * requirements are the Min and Max of the pools. pool_admission is the admission of each slot, pool_state and
     * pool_admit are the pool states and the admitted counts in the fixed bookkeeping. */
    bool pool_execution;
    struct vkd3d_workgraph_pool_admission *pool_admission;
    VkDeviceSize pool_state_scratch_offset;
    VkDeviceSize pool_state_scratch_size;
    VkDeviceSize pool_admit_scratch_offset;
    VkDeviceSize pool_admit_scratch_size;
    uint32_t pool_state_stride;
    struct vkd3d_workgraph_meta_pipeline_info pool_pass;
    uint32_t pool_test_records;
    uint32_t pool_test_stride;
    uint64_t pool_w[MAX_WORKGRAPH_LEVELS][D3D12_WG_POOL_COMPONENT_COUNT];
    uint64_t pool_bound[MAX_WORKGRAPH_LEVELS][D3D12_WG_POOL_COMPONENT_COUNT];
    VkDeviceSize pool_fixed_size;
    VkDeviceSize pool_staging_size;
    VkDeviceSize pool_sink_size;
    VkDeviceSize pool_min_size;
    VkDeviceSize pool_max_size;
    /* amdgpu-wddm fork: struct vkd3d_workgraph_output_guard, the last range of the required scratch memory. */
    VkDeviceSize output_guard_scratch_offset;
    VkDeviceSize output_guard_scratch_size;
    VkDeviceSize payload_ring_size;
    uint32_t packed_limit;
    /* amdgpu-wddm fork, lab flavour: the canary ranges (AMDGPU_WDDM_WG_LIMITS=canary=1), offsets in the backing
     * memory. canary_count is 0 without them. */
    uint32_t canary_count;
    VkDeviceSize canary_offsets[VKD3D_WORKGRAPH_CANARY_COUNT];
    uint32_t multi_input_large_size;
    uint32_t multi_input_groups;
    VkDeviceSize required_scratch_size;

    int32_t *coalesce_dividers_or_amp;
    uint32_t *share_mapping;
    struct d3d12_wg_payload_expander_meta *payload_expander_meta;

    const D3D12_NODE_ID *explicit_entry_points;
    size_t explicit_entry_point_count;

    const D3D12_NODE *explicit_defined_nodes;
    size_t explicit_defined_nodes_count;

    /* amdgpu-wddm fork: local root arguments table index per slot, after auto-assignment. */
    uint32_t *local_root_table_indices;

    /* amdgpu-wddm fork: recursion depth slots. Slot i < num_nodes is entry point i at recursion depth 0.
     * A node with [NodeMaxRecursionDepth(R)] gets R more slots, for depth 1 to R, from recursion_slot_base.
     * A record of a recursive node goes to the slot of its depth, so that one dispatch of a slot holds
     * records of one depth only, also when the node is reached at different levels of the graph.
     * num_pipelines is the number of slots. */
    unsigned int num_nodes;
    uint32_t *slot_entry;
    uint32_t *slot_depth;
    uint32_t *recursion_slot_base;

    /* amdgpu-wddm fork: the nodes of the graph (entry point indices) in the order of population, which is the
     * node index of GetNodeID and GetNodeIndex. node_in_graph has one entry per entry point. */
    D3D12_WORK_GRAPH_FLAGS flags;
    unsigned int *node_order;
    unsigned int node_order_count;
    bool *node_in_graph;
};

struct d3d12_wg_state_object_module
{
    VkShaderModule vk_module;
    struct d3d12_root_signature *root_signature;
    struct vkd3d_fused_root_signature_mappings *mappings;
};

struct d3d12_wg_state_object_data
{
    struct d3d12_wg_state_object_program *programs;
    size_t programs_size;
    size_t programs_count;

    struct d3d12_state_object_association *associations;
    size_t associations_size;
    size_t associations_count;

    struct d3d12_root_signature **subobject_root_signatures;
    size_t subobject_root_signatures_size;
    size_t subobject_root_signatures_count;

    /* Used to finalize compilation later. */
    const struct D3D12_DXIL_LIBRARY_DESC **dxil_libraries;
    size_t dxil_libraries_size;
    size_t dxil_libraries_count;

    /* Map 1:1 with VkShaderModule. */
    struct vkd3d_shader_library_entry_point *entry_points;
    size_t entry_points_size;
    size_t entry_points_count;

    struct vkd3d_shader_library_subobject *subobjects;
    size_t subobjects_size;
    size_t subobjects_count;

    /* amdgpu-wddm fork: node inputs in export order, recorded before the entry points are sorted. */
    const struct vkd3d_shader_node_input_data **export_order;
    size_t export_order_count;
};

static void d3d12_wg_state_object_free_programs(
        struct d3d12_wg_state_object_program *programs, size_t count, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    size_t i, j;
    for (i = 0; i < count; i++)
    {
        vkd3d_free((void *)programs[i].name);
        for (j = 0; j < programs[i].num_levels; j++)
        {
            vkd3d_free(programs[i].levels[j].nodes);
            vkd3d_free(programs[i].levels[j].shared_nodes);
        }

        /* amdgpu-wddm fork: num_pipelines is set before the pipelines are allocated, and the creation of a
         * state object can fail between the two. Every other per-slot array is freed without a look at it. */
        for (j = 0; programs[i].pipelines && j < programs[i].num_pipelines; j++)
        {
            VK_CALL(vkDestroyPipeline(device->vk_device, programs[i].pipelines[j].vk_non_entry_pipeline, NULL));
            VK_CALL(vkDestroyPipeline(device->vk_device, programs[i].pipelines[j].vk_cpu_node_entry_pipeline, NULL));
            VK_CALL(vkDestroyPipeline(device->vk_device, programs[i].pipelines[j].vk_static_cpu_node_entry_pipeline, NULL));
            VK_CALL(vkDestroyPipeline(device->vk_device, programs[i].pipelines[j].vk_gpu_node_entry_pipeline, NULL));
            vkd3d_free((void *)programs[i].pipelines[j].name);
        }
        vkd3d_free(programs[i].pipelines);
        vkd3d_free(programs[i].coalesce_dividers_or_amp);
        vkd3d_free(programs[i].share_mapping);
        vkd3d_free(programs[i].payload_expander_meta);
        vkd3d_free(programs[i].pool_admission);
        vkd3d_free(programs[i].local_root_table_indices);
        vkd3d_free(programs[i].slot_entry);
        vkd3d_free(programs[i].slot_depth);
        vkd3d_free(programs[i].recursion_slot_base);
        vkd3d_free(programs[i].node_order);
        vkd3d_free(programs[i].node_in_graph);
    }
    vkd3d_free(programs);
}

static void d3d12_wg_state_object_cleanup_data(
        struct d3d12_wg_state_object_data *data, struct d3d12_device *device)
{
    size_t i;

    d3d12_wg_state_object_free_programs(data->programs, data->programs_count, device);

    vkd3d_shader_dxil_free_library_entry_points(data->entry_points, data->entry_points_count);
    vkd3d_shader_dxil_free_library_subobjects(data->subobjects, data->subobjects_count);
    vkd3d_free(data->dxil_libraries);

    for (i = 0; i < data->associations_count; i++)
    {
        if ((data->associations[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE ||
                data->associations[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE) &&
                data->associations[i].root_signature)
        {
            d3d12_root_signature_dec_ref(data->associations[i].root_signature);
        }

        /* Should only free export if we have deep duplication of associations. */
    }

    for (i = 0; i < data->subobject_root_signatures_count; i++)
        d3d12_root_signature_dec_ref(data->subobject_root_signatures[i]);
    vkd3d_free(data->subobject_root_signatures);

    vkd3d_free(data->associations);
    vkd3d_free(data->export_order);
}

/* amdgpu-wddm fork: a work graph flag of the system D3D12 driver (bc250-win, engine-ddi/work-graphs.cpp), which
 * D3D12 does not define. The DDI gives the driver the final graph (WorkGraphs.md, D3D12DDI_WORK_GRAPH_DESC_0108):
 * every node with its final properties, and the entry points in the order of their entry point indices. The driver
 * passes every node as an explicitly defined node with overrides, and the entry points in that order. With the flag:
 * - the entry points are pEntrypoints, in that order, so that the entry point indices of DispatchGraph, also those
 *   in GPU memory, are the indices of the runtime;
 * - a node that the engine makes an entry point, but that pEntrypoints does not list, is refused;
 * - no local root arguments table index is assigned automatically, because the runtime assigned them;
 * - a rename, a shared input or an output override that differs from the shader is refused (E_NOTIMPL), because the
 *   engine applies none of them. An override that is equal to the shader has no effect. */
#define VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM 0x80000000u

static HRESULT d3d12_wg_state_object_parse_subobject(
        struct d3d12_wg_state_object_data *data, struct d3d12_device *device,
        const D3D12_STATE_SUBOBJECT *obj, unsigned int association_priority)
{
    unsigned int i;

    switch (obj->Type)
    {
        case D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH:
        {
            const D3D12_WORK_GRAPH_DESC *wg_desc = obj->pDesc;
            struct d3d12_wg_state_object_program *program;

            /* amdgpu-wddm fork: a graph can also have only the listed and explicitly defined nodes. */
            if (wg_desc->Flags & ~(D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES |
                    VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM))
            {
                FIXME("Unsupported work graph flags %#x.\n", wg_desc->Flags);
                return E_NOTIMPL;
            }

            /* amdgpu-wddm fork: a graph of the DDI lists every node and every entry point. */
            if ((wg_desc->Flags & VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM) &&
                    ((wg_desc->Flags & D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES) ||
                    !wg_desc->NumEntrypoints || !wg_desc->NumExplicitlyDefinedNodes))
            {
                ERR("Work graph %s of the DDI without its entry points and nodes, flags %#x.\n",
                        debugstr_w(wg_desc->ProgramName), wg_desc->Flags);
                return E_INVALIDARG;
            }

            vkd3d_array_reserve((void **)&data->programs, &data->programs_size,
                    data->programs_count + 1, sizeof(*data->programs));
            program = &data->programs[data->programs_count++];
            memset(program, 0, sizeof(*program));

            if (wg_desc->NumEntrypoints != 0)
            {
                program->explicit_entry_points = wg_desc->pEntrypoints;
                program->explicit_entry_point_count = wg_desc->NumEntrypoints;
            }

            if (wg_desc->NumExplicitlyDefinedNodes != 0)
            {
                program->explicit_defined_nodes = wg_desc->pExplicitlyDefinedNodes;
                program->explicit_defined_nodes_count = wg_desc->NumExplicitlyDefinedNodes;
            }

            program->name = vkd3d_wstrdup(wg_desc->ProgramName);
            program->flags = wg_desc->Flags;
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        {
            /* LOCAL_ROOT_SIGNATURE and GLOBAL_ROOT_SIGNATURE alias. */
            const D3D12_LOCAL_ROOT_SIGNATURE *rs = obj->pDesc;

            /* This is only chosen as default if there is nothing else.
             * Conflicting definitions seem to cause runtime to choose something
             * arbitrary. Just override the low priority default.
             * A high priority default association takes precedence if it exists. */
            vkd3d_array_reserve((void **)&data->associations, &data->associations_size,
                    data->associations_count + 1,
                    sizeof(*data->associations));

            /* Root signatures being exported to NULL takes priority as the default local RS.
             * They do however, take precedence over DXIL exported subobjects ... */
            data->associations[data->associations_count].export = NULL;
            data->associations[data->associations_count].kind =
                    obj->Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ?
                            VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE :
                            VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE;
            data->associations[data->associations_count].root_signature =
                    impl_from_ID3D12RootSignature(rs->pLocalRootSignature);
            data->associations[data->associations_count].priority = association_priority;

            /* Hold reference in case we need to duplicate the data structure due to compile defer. */
            if (data->associations[data->associations_count].root_signature)
                d3d12_root_signature_inc_ref(data->associations[data->associations_count].root_signature);
            data->associations_count++;
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        {
            const D3D12_DXIL_LIBRARY_DESC *lib = obj->pDesc;
            if (vkd3d_shader_dxil_append_library_entry_points_and_subobjects(lib, data->dxil_libraries_count,
                    &data->entry_points, &data->entry_points_size,
                    &data->entry_points_count,
                    &data->subobjects, &data->subobjects_size,
                    &data->subobjects_count) != VKD3D_OK)
            {
                ERR("Failed to parse DXIL library.\n");
                return E_OUTOFMEMORY;
            }
            vkd3d_array_reserve((void**)&data->dxil_libraries, &data->dxil_libraries_size,
                    data->dxil_libraries_count + 1, sizeof(*data->dxil_libraries));
            data->dxil_libraries[data->dxil_libraries_count++] = lib;
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        {
            /* amdgpu-wddm fork: the dependency flags change nothing for an executable work graph. Additions to a
             * work graph state object are not implemented, so a state object that allows them is refused. */
            const D3D12_STATE_OBJECT_CONFIG *config = obj->pDesc;

            if (config->Flags & ~(D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                    D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS))
            {
                ERR("Unsupported state object flags %#x for a work graph.\n", config->Flags);
                return E_NOTIMPL;
            }
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
        {
            /* amdgpu-wddm fork: the device has one node, so a node mask changes nothing. */
            const D3D12_NODE_MASK *mask = obj->pDesc;

            if (mask->NodeMask > 1)
            {
                ERR("Node mask %#x for a work graph on a device with one node.\n", mask->NodeMask);
                return E_INVALIDARG;
            }
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            /* amdgpu-wddm fork: an association to a root signature subobject of the description. The system D3D12
             * driver gives one for each root signature that the runtime associates with an export. */
            const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *association = obj->pDesc;
            const D3D12_STATE_SUBOBJECT *target = association->pSubobjectToAssociate;
            unsigned int num_associations = max(association->NumExports, 1);
            struct d3d12_state_object_association *entry;
            const D3D12_LOCAL_ROOT_SIGNATURE *rs;

            if (!target || (target->Type != D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE &&
                    target->Type != D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE))
            {
                ERR("Association with subobject type %d is not supported for a work graph.\n",
                        target ? (int)target->Type : -1);
                return E_NOTIMPL;
            }

            /* LOCAL_ROOT_SIGNATURE and GLOBAL_ROOT_SIGNATURE alias. */
            rs = target->pDesc;
            if (!vkd3d_array_reserve((void **)&data->associations, &data->associations_size,
                    data->associations_count + num_associations, sizeof(*data->associations)))
                return E_OUTOFMEMORY;

            for (i = 0; i < num_associations; i++)
            {
                entry = &data->associations[data->associations_count++];
                entry->kind = target->Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ?
                        VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE :
                        VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE;
                entry->root_signature = impl_from_ID3D12RootSignature(rs->pLocalRootSignature);
                if (entry->root_signature)
                    d3d12_root_signature_inc_ref(entry->root_signature);
                entry->export = association->NumExports ? association->pExports[i] : NULL;
                entry->priority = association->NumExports ?
                        VKD3D_ASSOCIATION_PRIORITY_EXPLICIT : VKD3D_ASSOCIATION_PRIORITY_EXPLICIT_DEFAULT;
            }
            break;
        }

        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *association = obj->pDesc;
            unsigned int num_associations = max(association->NumExports, 1);
            const struct vkd3d_shader_library_subobject *subobject;
            unsigned int root_signature_index = 0;
            for (i = 0; i < data->subobjects_count; i++)
            {
                if (vkd3d_export_strequal(association->SubobjectToAssociate, data->subobjects[i].name))
                    break;

                if (data->subobjects[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE ||
                        data->subobjects[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE)
                {
                    root_signature_index++;
                }
            }

            if (i == data->subobjects_count)
            {
                ERR("Cannot find subobject %s.\n", debugstr_w(association->SubobjectToAssociate));
                return E_INVALIDARG;
            }

            subobject = &data->subobjects[i];
            vkd3d_array_reserve((void **)&data->associations, &data->associations_size,
                    data->associations_count + num_associations,
                    sizeof(*data->associations));

            for (i = 0; i < num_associations; i++)
            {
                switch (subobject->kind)
                {
                    case VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE:
                    case VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE:
                        data->associations[data->associations_count].root_signature =
                                data->subobject_root_signatures[root_signature_index];
                        if (data->associations[data->associations_count].root_signature)
                            d3d12_root_signature_inc_ref(data->associations[data->associations_count].root_signature);
                        break;

                    default:
                        ERR("Unexpected type %u for DXIL -> object association.\n", subobject->kind);
                        return E_INVALIDARG;
                }

                data->associations[data->associations_count].kind = subobject->kind;
                data->associations[data->associations_count].export =
                        association->NumExports ? association->pExports[i] : NULL;

                if (association_priority == VKD3D_ASSOCIATION_PRIORITY_DECLARED_STATE_OBJECT &&
                        association->NumExports)
                {
                    data->associations[data->associations_count].priority = VKD3D_ASSOCIATION_PRIORITY_EXPLICIT;
                }
                else if (association_priority == VKD3D_ASSOCIATION_PRIORITY_DECLARED_STATE_OBJECT)
                {
                    data->associations[data->associations_count].priority = VKD3D_ASSOCIATION_PRIORITY_EXPLICIT_DEFAULT;
                }
                else if (association->NumExports)
                {
                    data->associations[data->associations_count].priority =
                            VKD3D_ASSOCIATION_PRIORITY_DXIL_SUBOBJECT_ASSIGNMENT_EXPLICIT;
                }
                else
                {
                    data->associations[data->associations_count].priority =
                            VKD3D_ASSOCIATION_PRIORITY_DXIL_SUBOBJECT_ASSIGNMENT_DEFAULT;
                }

                TRACE("  Export: %s (prio %u)\n",
                        association->NumExports ? debugstr_w(association->pExports[i]) : "NULL",
                        data->associations[data->associations_count].priority);

                data->associations_count++;
            }

            break;
        }

        default:
            FIXME("Unimplemented workgraph subobject type %u.\n", obj->Type);
            return E_NOTIMPL;
    }

    return S_OK;
}

static uint32_t d3d12_work_graph_find_array_size_by_id(
        const struct vkd3d_shader_library_entry_point *entries,
        size_t entry_count, const char *node_id)
{
    size_t i;

    /* Can assume the entries are sorted by name and array index at this point. */
    for (i = entry_count; i; i--)
        if (entries[i - 1].node_input && strcmp(entries[i - 1].node_input->node_id, node_id) == 0)
            return entries[i - 1].node_input->node_array_index + 1;

    return UINT32_MAX;
}

static uint32_t d3d12_work_graph_find_node_by_id(
        const struct vkd3d_shader_library_entry_point *entries,
        size_t entry_count, const char *node_id, UINT node_array_index)
{
    size_t i;

    for (i = 0; i < entry_count; i++)
    {
        if (entries[i].node_input &&
                entries[i].node_input->node_array_index == node_array_index &&
                strcmp(entries[i].node_input->node_id, node_id) == 0)
        {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
}

static uint32_t d3d12_work_graph_find_node_by_id_wchar(
        const struct vkd3d_shader_library_entry_point *entries,
        size_t entry_count, LPCWSTR node_id, UINT node_array_index)
{
    size_t i;

    for (i = 0; i < entry_count; i++)
    {
        if (entries[i].node_input &&
                entries[i].node_input->node_array_index == node_array_index &&
                vkd3d_export_strequal_mixed(node_id, entries[i].node_input->node_id))
        {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
}

static HRESULT d3d12_wg_state_object_program_add_node_to_level(
        struct d3d12_wg_state_object_program *program,
        struct d3d12_wg_state_object_data *data,
        unsigned int entry_point_index,
        unsigned int level,
        bool is_recursing);

static HRESULT d3d12_wg_state_object_program_add_outputs_to_level(
        struct d3d12_wg_state_object_program *program,
        struct d3d12_wg_state_object_data *data,
        unsigned int entry_point_index,
        unsigned int level)
{
    const struct vkd3d_shader_node_output_data *output;
    struct vkd3d_shader_library_entry_point *entry;
    uint32_t node_array_size, node_index;
    unsigned int i, j;
    HRESULT hr;

    entry = &data->entry_points[program->slot_entry[entry_point_index]];

    for (i = 0; i < entry->node_outputs_count; i++)
    {
        output = &entry->node_outputs[i];

        /* Checked on the outside for now as well. */
        if (output->node_array_size == UINT32_MAX)
        {
            FIXME("Unbounded array size is not supported.\n");
            return E_NOTIMPL;
        }

        node_array_size = output->node_array_size ? output->node_array_size : 1;

        for (j = 0; j < node_array_size; j++)
        {
            node_index = d3d12_work_graph_find_node_by_id(
                    data->entry_points, data->entry_points_count,
                    output->node_id, output->node_array_index + j);

            /* We handle recursion separately. */
            if (node_index == program->slot_entry[entry_point_index])
                continue;

            if (node_index == UINT32_MAX)
            {
                /* It's okay if we don't find the input in sparse mode. */
                if (output->sparse_array)
                    continue;
                return E_INVALIDARG;
            }

            if (FAILED(hr = d3d12_wg_state_object_program_add_node_to_level(
                    program, data, node_index, level, false)))
            {
                return hr;
            }
        }
    }

    return S_OK;
}

static HRESULT d3d12_wg_state_object_program_add_node_to_level(
        struct d3d12_wg_state_object_program *program,
        struct d3d12_wg_state_object_data *data,
        unsigned int entry_point_index,
        unsigned int level,
        bool is_recursing)
{
    struct vkd3d_shader_library_entry_point *entry;
    struct d3d12_workgraph_level_execution *exec;
    unsigned int i, j;
    HRESULT hr;

    if (level >= MAX_WORKGRAPH_LEVELS)
    {
        /* amdgpu-wddm fork: ERR, because the release flavour removes FIXME lines. */
        ERR("Work graph is deeper than %u nodes.\n", MAX_WORKGRAPH_LEVELS);
        return E_INVALIDARG;
    }

    entry = &data->entry_points[program->slot_entry[entry_point_index]];

    exec = &program->levels[level];
    for (i = 0; i < exec->nodes_count; i++)
        if (exec->nodes[i] == entry_point_index)
            return S_OK;

    if (!vkd3d_array_reserve((void **)&exec->nodes, &exec->nodes_size,
            exec->nodes_count + 1, sizeof(*exec->nodes)))
        return E_OUTOFMEMORY;
    exec->nodes[exec->nodes_count++] = entry_point_index;
    program->num_levels = max(program->num_levels, level + 1);

    if (!is_recursing)
    {
        /* amdgpu-wddm fork: depth i + 1 of this chain runs from its own slot. */
        for (i = 0; i < entry->node_input->recursion_factor; i++)
        {
            if (FAILED(hr = d3d12_wg_state_object_program_add_node_to_level(
                    program, data, program->recursion_slot_base[entry_point_index] + i,
                    level + 1 + i, true)))
            {
                return hr;
            }
        }
    }

    if (FAILED(hr = d3d12_wg_state_object_program_add_outputs_to_level(program, data, entry_point_index, level + 1)))
        return hr;

    for (i = 0; i < data->entry_points_count; i++)
    {
        /* Fish for shared inputs. */
        const struct vkd3d_shader_node_input_data *candidate = data->entry_points[i].node_input;
        if (!candidate || !program->node_in_graph[i])
            continue;

        if (candidate->node_share_input_id &&
                strcmp(candidate->node_share_input_id, entry->node_input->node_id) == 0 &&
                candidate->node_share_input_array_index == entry->node_input->node_array_index)
        {
            for (j = 0; j < exec->shared_nodes_count; j++)
                if (exec->shared_nodes[j].node_pipeline_index == i)
                    break;

            if (j == exec->shared_nodes_count)
            {
                if (!vkd3d_array_reserve((void **)&exec->shared_nodes, &exec->shared_nodes_size,
                        exec->shared_nodes_count + 1, sizeof(*exec->shared_nodes)))
                    return E_OUTOFMEMORY;
                exec->shared_nodes[exec->shared_nodes_count].node_pipeline_index = i;
                exec->shared_nodes[exec->shared_nodes_count].node_payload_index = entry_point_index;
                exec->shared_nodes_count++;

                /* A shared input node may also have its own outputs. */
                if (FAILED(hr = d3d12_wg_state_object_program_add_outputs_to_level(program, data, i, level + 1)))
                    return hr;
            }
        }
    }

    return S_OK;
}

static int entry_point_compare_func(const void *a_, const void *b_)
{
    const struct vkd3d_shader_library_entry_point *a = a_;
    const struct vkd3d_shader_library_entry_point *b = b_;
    const char *a_name, *b_name;
    int32_t a_index;
    int32_t b_index;
    int sort_key;

    a_name = (a->node_input && a->node_input->node_id) ? a->node_input->node_id : "";
    b_name = (b->node_input && b->node_input->node_id) ? b->node_input->node_id : "";
    a_index = a->node_input ? a->node_input->node_array_index : 0;
    b_index = b->node_input ? b->node_input->node_array_index : 0;

    sort_key = strcmp(a_name, b_name);
    if (sort_key != 0)
        return sort_key;
    else if (a_index != b_index)
        return a_index < b_index ? -1 : 1;
    else
        return 0;
}

/* amdgpu-wddm fork: the export order. The libraries are in subobject order, and the node shaders of one library are
 * in the order of its RDAT function table. The entry points of a library come in dx.entryPoints order, which DXC
 * sorts by name. */
struct d3d12_wg_export_rank
{
    const struct vkd3d_shader_library_entry_point *entry;
    size_t position;
};

static int d3d12_wg_export_rank_compare(const void *a_, const void *b_)
{
    const struct d3d12_wg_export_rank *a = a_;
    const struct d3d12_wg_export_rank *b = b_;

    if (a->entry->identifier != b->entry->identifier)
        return a->entry->identifier < b->entry->identifier ? -1 : 1;
    if (a->entry->export_index != b->entry->export_index)
        return a->entry->export_index < b->entry->export_index ? -1 : 1;
    if (a->position != b->position)
        return a->position < b->position ? -1 : 1;
    return 0;
}

static HRESULT d3d12_wg_state_object_rearrange_entry_points(struct d3d12_wg_state_object_data *data)
{
    struct d3d12_wg_export_rank *ranks;
    struct vkd3d_shader_library_entry_point *new_entries;
    const struct vkd3d_shader_library_entry_point *entry;
    const struct vkd3d_shader_library_entry_point *next;
    size_t padded_entry_points = 0;
    size_t padded_entry_offset = 0;
    size_t i;

    /* amdgpu-wddm fork: the sort loses the export order, which the default local root
     * arguments table indices need (WorkGraphs.md, D3D12_WORK_GRAPH_DESC). */
    if (!(data->export_order = vkd3d_calloc(max(data->entry_points_count, 1), sizeof(*data->export_order))))
        return E_OUTOFMEMORY;
    if (!(ranks = vkd3d_calloc(max(data->entry_points_count, 1), sizeof(*ranks))))
        return E_OUTOFMEMORY;
    for (i = 0; i < data->entry_points_count; i++)
    {
        ranks[i].entry = &data->entry_points[i];
        ranks[i].position = i;
    }
    qsort(ranks, data->entry_points_count, sizeof(*ranks), d3d12_wg_export_rank_compare);
    for (i = 0; i < data->entry_points_count; i++)
        if (ranks[i].entry->node_input)
            data->export_order[data->export_order_count++] = ranks[i].entry->node_input;
    vkd3d_free(ranks);

    /* Rearrange and figure out how large unbounded arrays are, and place arrayed entry points
     * in order so that we can index linearly. */
    qsort(data->entry_points, data->entry_points_count, sizeof(*data->entry_points), entry_point_compare_func);

    for (i = 0; i < data->entry_points_count; i++)
    {
        if (i + 1 == data->entry_points_count)
        {
            if (data->entry_points[i].node_input)
                padded_entry_points += data->entry_points[i].node_input->node_array_index + 1;
        }
        else
        {
            entry = &data->entry_points[i];
            next = &data->entry_points[i + 1];

            if (entry->node_input && next->node_input)
            {
                if (strcmp(entry->node_input->node_id, next->node_input->node_id) != 0)
                    padded_entry_points += entry->node_input->node_array_index + 1;
                else if (entry->node_input->node_array_index == next->node_input->node_array_index)
                    return E_INVALIDARG;
            }
        }
    }

    /* No need to do anything fancy. */
    if (padded_entry_points == data->entry_points_count)
        return S_OK;

    new_entries = vkd3d_malloc(sizeof(*new_entries) * padded_entry_points);

    for (i = 0; i < data->entry_points_count; i++)
    {
        const struct vkd3d_shader_node_input_data *node_input = data->entry_points[i].node_input;
        const struct vkd3d_shader_node_input_data *prev_input = NULL;
        size_t pad_count = 0;

        if (!node_input)
            continue;

        if (i != 0)
            prev_input = data->entry_points[i - 1].node_input;

        if (i == 0 || !prev_input || strcmp(node_input->node_id, prev_input->node_id) != 0)
            pad_count = node_input->node_array_index;
        else
            pad_count = node_input->node_array_index - prev_input->node_array_index - 1;

        if (pad_count)
        {
            memset(&new_entries[padded_entry_offset], 0, pad_count * sizeof(*new_entries));
            padded_entry_offset += pad_count;
        }

        new_entries[padded_entry_offset++] = data->entry_points[i];
    }

    assert(padded_entry_offset == padded_entry_points);

    /* The entries themselves are pilfered. */
    vkd3d_free(data->entry_points);
    data->entry_points = new_entries;
    data->entry_points_count = padded_entry_points;

    return S_OK;
}

static const struct vkd3d_shader_library_entry_point *
d3d12_wg_state_object_find_exported_entry_point(
        struct d3d12_wg_state_object_data *data, LPCWSTR shader)
{
    size_t i;
    for (i = 0; i < data->entry_points_count; i++)
        if (vkd3d_export_equal(shader, &data->entry_points[i]))
            return &data->entry_points[i];
    return NULL;
}

/* amdgpu-wddm fork: the node ID of an override against the ID that the shader declares. */
static bool d3d12_wg_node_id_equal(const D3D12_NODE_ID *id, const char *name, uint32_t array_index)
{
    return name && *name && id->Name && vkd3d_export_strequal_mixed(id->Name, name) &&
            id->ArrayIndex == array_index;
}

/* amdgpu-wddm fork: with VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM the overrides are the final properties of the
 * node. The engine applies no rename, no shared input and no output override, so each of them must be what the shader
 * declares. A smaller output budget than the declared one is accepted: the engine sizes for the declared one. */
static HRESULT d3d12_wg_state_object_check_ddi_overrides(const struct vkd3d_shader_library_entry_point *entry,
        const D3D12_NODE_ID *new_name, const D3D12_NODE_ID *share_input_of,
        UINT num_output_overrides, const D3D12_NODE_OUTPUT_OVERRIDES *output_overrides)
{
    const struct vkd3d_shader_node_input_data *input = entry->node_input;
    const struct vkd3d_shader_node_output_data *output;
    const D3D12_NODE_OUTPUT_OVERRIDES *o;
    UINT i;

    if (new_name && !d3d12_wg_node_id_equal(new_name, input->node_id, input->node_array_index))
    {
        ERR("Node %s[%u] is renamed to %s[%u], which is not implemented.\n", input->node_id,
                input->node_array_index, debugstr_w(new_name->Name), new_name->ArrayIndex);
        return E_NOTIMPL;
    }

    if (share_input_of && !d3d12_wg_node_id_equal(share_input_of, input->node_share_input_id,
            input->node_share_input_array_index))
    {
        ERR("Node %s[%u] shares the input of %s[%u], which the shader does not declare.\n", input->node_id,
                input->node_array_index, debugstr_w(share_input_of->Name), share_input_of->ArrayIndex);
        return E_NOTIMPL;
    }

    for (i = 0; i < num_output_overrides; i++)
    {
        o = &output_overrides[i];
        if (o->OutputIndex >= entry->node_outputs_count)
        {
            ERR("Node %s[%u] has no output %u.\n", input->node_id, input->node_array_index, o->OutputIndex);
            return E_INVALIDARG;
        }

        output = &entry->node_outputs[o->OutputIndex];
        if ((o->pNewName && !d3d12_wg_node_id_equal(o->pNewName, output->node_id, output->node_array_index)) ||
                (o->pAllowSparseNodes && *o->pAllowSparseNodes && !output->sparse_array) ||
                (o->pMaxRecords && *o->pMaxRecords > output->max_records))
        {
            ERR("Output %u of node %s[%u] differs from the shader, which is not implemented.\n", o->OutputIndex,
                    input->node_id, input->node_array_index);
            return E_NOTIMPL;
        }
    }

    return S_OK;
}

static HRESULT d3d12_wg_state_object_apply_node_overrides(
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program)
{
    const bool ddi = !!(program->flags & VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM);
    const struct vkd3d_shader_library_entry_point *entry;
    HRESULT hr;
    size_t i;

    for (i = 0; i < program->explicit_defined_nodes_count; i++)
    {
        const D3D12_SHADER_NODE *shader_node;
        const D3D12_NODE *node;

        node = &program->explicit_defined_nodes[i];
        if (node->NodeType != D3D12_NODE_TYPE_SHADER)
        {
            /* Not supported in current workgraph spec. Would be app error to try this. */
            WARN("Attempting to use non-supported D3D12_NODE_TYPE_PROGRAM.\n");
            return E_INVALIDARG;
        }

        shader_node = &node->Shader;

        TRACE("Adding overrides for export %s.\n", debugstr_w(shader_node->Shader));

        entry = d3d12_wg_state_object_find_exported_entry_point(data, shader_node->Shader);
        if (!entry)
        {
            FIXME("Could not find shader with export name %s.\n", debugstr_w(shader_node->Shader));
            return E_INVALIDARG;
        }

        if (!entry->node_input)
        {
            FIXME("Override node does not have a node input structure associated with it.\n");
            return E_INVALIDARG;
        }

        switch (shader_node->OverridesType)
        {
            case D3D12_NODE_OVERRIDES_TYPE_BROADCASTING_LAUNCH:
            {
                const D3D12_BROADCASTING_LAUNCH_OVERRIDES *override = shader_node->pBroadcastingLaunchOverrides;
                if (ddi)
                {
                    if (FAILED(hr = d3d12_wg_state_object_check_ddi_overrides(entry, override->pNewName,
                            override->pShareInputOf, override->NumOutputOverrides, override->pOutputOverrides)))
                        return hr;
                }
                else
                {
                    if (override->NumOutputOverrides || override->pShareInputOf || override->pNewName)
                        program->pool_ignored_overrides = true;
                    if (override->NumOutputOverrides)
                        FIXME("Output overrides not supported yet.\n");
                    if (override->pShareInputOf)
                        FIXME("ShaderInputOf overrides not supported yet.\n");
                    if (override->pNewName)
                        FIXME("NameView overrides not supported yet.\n");
                }

                if (override->pDispatchGrid)
                {
                    memcpy(entry->node_input->broadcast_grid, override->pDispatchGrid, sizeof(UINT) * 3);
                    entry->node_input->dispatch_grid_is_upper_bound = false;
                    TRACE("Overriding export %s dispatch grid [%u, %u, %u]\n",
                            debugstr_w(shader_node->Shader),
                            entry->node_input->broadcast_grid[0],
                            entry->node_input->broadcast_grid[1],
                            entry->node_input->broadcast_grid[2]);
                }

                if (override->pMaxDispatchGrid)
                {
                    memcpy(entry->node_input->broadcast_grid, override->pMaxDispatchGrid, sizeof(UINT) * 3);
                    entry->node_input->dispatch_grid_is_upper_bound = true;
                    TRACE("Overriding export %s max dispatch grid [%u, %u, %u]\n",
                            debugstr_w(shader_node->Shader),
                            entry->node_input->broadcast_grid[0],
                            entry->node_input->broadcast_grid[1],
                            entry->node_input->broadcast_grid[2]);
                }

                if (override->pProgramEntry)
                {
                    entry->node_input->is_program_entry = *override->pProgramEntry;
                    TRACE("Overriding export %s IsProgramEntry %u\n",
                            debugstr_w(shader_node->Shader), entry->node_input->is_program_entry);
                }

                if (override->pLocalRootArgumentsTableIndex)
                {
                    entry->node_input->local_root_arguments_table_index = *override->pLocalRootArgumentsTableIndex;
                    TRACE("Overriding export %s LocalRootArgumentsTableIndex %u\n",
                            debugstr_w(shader_node->Shader), entry->node_input->local_root_arguments_table_index);
                }

                break;
            }

            /* These three are exactly the same.
             * It's a union of pointers, so should be no issue. */
            case D3D12_NODE_OVERRIDES_TYPE_COALESCING_LAUNCH:
            case D3D12_NODE_OVERRIDES_TYPE_THREAD_LAUNCH:
            case D3D12_NODE_OVERRIDES_TYPE_COMMON_COMPUTE:
            {
                const D3D12_COALESCING_LAUNCH_OVERRIDES *override = shader_node->pCoalescingLaunchOverrides;
                if (ddi)
                {
                    if (FAILED(hr = d3d12_wg_state_object_check_ddi_overrides(entry, override->pNewName,
                            override->pShareInputOf, override->NumOutputOverrides, override->pOutputOverrides)))
                        return hr;
                }
                else
                {
                    if (override->NumOutputOverrides || override->pShareInputOf || override->pNewName)
                        program->pool_ignored_overrides = true;
                    if (override->NumOutputOverrides)
                        FIXME("Output overrides not supported yet.\n");
                    if (override->pShareInputOf)
                        FIXME("ShaderInputOf overrides not supported yet.\n");
                    if (override->pNewName)
                        FIXME("NameView overrides not supported yet.\n");
                }

                if (override->pProgramEntry)
                {
                    entry->node_input->is_program_entry = *override->pProgramEntry;
                    TRACE("Overriding export %s IsProgramEntry %u\n",
                            debugstr_w(shader_node->Shader), entry->node_input->is_program_entry);
                }

                if (override->pLocalRootArgumentsTableIndex)
                {
                    entry->node_input->local_root_arguments_table_index = *override->pLocalRootArgumentsTableIndex;
                    TRACE("Overriding export %s LocalRootArgumentsTableIndex %u\n",
                            debugstr_w(shader_node->Shader), entry->node_input->local_root_arguments_table_index);
                }

                break;
            }

            case D3D12_NODE_OVERRIDES_TYPE_NONE:
                break;

            default:
                FIXME("Unrecognized node override type %u.\n", shader_node->OverridesType);
                return E_INVALIDARG;
        }
    }

    return S_OK;
}

/* amdgpu-wddm fork: see the recursion depth slots in struct d3d12_wg_state_object_program. */
static HRESULT d3d12_wg_state_object_program_init_slots(
        struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_state_object_data *data)
{
    unsigned int num_slots, slot, depth;
    uint64_t total;
    uint32_t factor;
    size_t i;

    /* A graph has at most 0xffffff node slots, recursion depths included, and a chain of at most 32 nodes
     * (WorkGraphs.md, Node count limits). Reject other values before the slot arrays are sized from them. */
    total = data->entry_points_count;
    for (i = 0; i < data->entry_points_count; i++)
    {
        if (!data->entry_points[i].node_input)
            continue;
        factor = data->entry_points[i].node_input->recursion_factor;
        if (factor > MAX_WORKGRAPH_LEVELS)
        {
            ERR("Node recursion depth %u is above %u.\n", factor, MAX_WORKGRAPH_LEVELS);
            return E_INVALIDARG;
        }
        total += factor;
    }
    if (total > 0xffffff)
    {
        ERR("Work graph has %"PRIu64" node slots, more than 0xffffff.\n", total);
        return E_INVALIDARG;
    }
    num_slots = total;

    program->num_nodes = data->entry_points_count;
    program->num_pipelines = num_slots;
    program->slot_entry = vkd3d_malloc(max(num_slots, 1) * sizeof(*program->slot_entry));
    program->slot_depth = vkd3d_malloc(max(num_slots, 1) * sizeof(*program->slot_depth));
    program->recursion_slot_base = vkd3d_malloc(max(data->entry_points_count, 1) *
            sizeof(*program->recursion_slot_base));
    if (d3d12_wg_fault_injected("slots", UINT_MAX))
    {
        vkd3d_free(program->slot_depth);
        program->slot_depth = NULL;
    }
    if (!program->slot_entry || !program->slot_depth || !program->recursion_slot_base)
        return E_OUTOFMEMORY;

    for (i = 0; i < data->entry_points_count; i++)
    {
        program->slot_entry[i] = i;
        program->slot_depth[i] = 0;
        program->recursion_slot_base[i] = UINT32_MAX;
    }

    slot = data->entry_points_count;
    for (i = 0; i < data->entry_points_count; i++)
    {
        factor = data->entry_points[i].node_input ? data->entry_points[i].node_input->recursion_factor : 0;
        if (!factor)
            continue;

        program->recursion_slot_base[i] = slot;
        for (depth = 1; depth <= factor; depth++, slot++)
        {
            program->slot_entry[slot] = i;
            program->slot_depth[slot] = depth;
        }
    }

    return S_OK;
}

/* amdgpu-wddm fork: the slot that holds the records of entry point entry_index at recursion depth depth. */
static uint32_t d3d12_wg_state_object_program_depth_slot(
        const struct d3d12_wg_state_object_program *program, uint32_t entry_index, uint32_t depth)
{
    return depth ? program->recursion_slot_base[entry_index] + depth - 1 : entry_index;
}

/* amdgpu-wddm fork: graph population (WorkGraphs.md, D3D12_WORK_GRAPH_DESC). The order in which the nodes are
 * added is the node order of GetNodeID and GetNodeIndex, and the entry points keep it. A node is added once. */
static void d3d12_wg_state_object_population_add(struct d3d12_wg_state_object_program *program,
        unsigned int entry_index)
{
    if (program->node_in_graph[entry_index])
        return;

    program->node_in_graph[entry_index] = true;
    program->node_order[program->node_order_count++] = entry_index;
}

/* Adds the node, then its outputs, depth first, in the order the shader declares them. An array output adds
 * its elements in index order; a sparse array can have missing elements. The outputs of a node are added only
 * once, also when the node was in the graph before (an explicitly defined node that is not an entry point). */
static HRESULT d3d12_wg_state_object_population_expand(const struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program, bool *expanded, unsigned int entry_index)
{
    const struct vkd3d_shader_library_entry_point *entry = &data->entry_points[entry_index];
    const struct vkd3d_shader_node_output_data *output;
    uint32_t node_array_size, node_index;
    HRESULT hr;
    size_t i, j;

    d3d12_wg_state_object_population_add(program, entry_index);
    if (expanded[entry_index])
        return S_OK;
    expanded[entry_index] = true;

    for (i = 0; i < entry->node_outputs_count; i++)
    {
        output = &entry->node_outputs[i];
        if (output->node_array_size == UINT32_MAX)
        {
            FIXME("Cannot deal with unbounded node arrays yet.\n");
            return E_NOTIMPL;
        }

        node_array_size = output->node_array_size ? output->node_array_size : 1;

        for (j = 0; j < node_array_size; j++)
        {
            node_index = d3d12_work_graph_find_node_by_id(data->entry_points, data->entry_points_count,
                    output->node_id, output->node_array_index + j);
            if (node_index == UINT32_MAX)
            {
                /* It's okay if we don't find the input in sparse mode. */
                if (output->sparse_array)
                    continue;
                WARN("NodeID %s[%zu] was not found.\n", output->node_id, output->node_array_index + j);
                return E_INVALIDARG;
            }

            if (FAILED(hr = d3d12_wg_state_object_population_expand(data, program, expanded, node_index)))
                return hr;
        }
    }

    return S_OK;
}

static bool d3d12_wg_state_object_program_node_in_levels(const struct d3d12_wg_state_object_program *program,
        unsigned int entry_index)
{
    unsigned int i, j;

    for (i = 0; i < program->num_levels; i++)
    {
        for (j = 0; j < program->levels[i].nodes_count; j++)
            if (program->slot_entry[program->levels[i].nodes[j]] == entry_index)
                return true;
        for (j = 0; j < program->levels[i].shared_nodes_count; j++)
            if (program->levels[i].shared_nodes[j].node_pipeline_index == entry_index)
                return true;
    }

    return false;
}

/* amdgpu-wddm fork: the nodes are added first from pExplicitlyDefinedNodes, then from pEntrypoints, then with
 * D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES from every node shader in export order. An explicitly defined
 * node adds its outputs only if it is an entry point. The entry points are the listed nodes, the nodes that are
 * entry points in the shader or in an override, and the nodes that no other node in the graph writes to. Without
 * the flag, such an unwritten node must be a listed node or an entry point. The levels are then built from the
 * entry points in the node order. */
static HRESULT d3d12_wg_state_object_populate_program(struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program)
{
    const struct vkd3d_shader_node_input_data *node_input;
    const struct vkd3d_shader_library_entry_point *defined;
    size_t count = data->entry_points_count;
    struct vkd3d_shader_node_output_data *output;
    bool *expanded = NULL, *listed = NULL;
    uint32_t node_array_size, node_index;
    bool *written = NULL;
    unsigned int entry;
    HRESULT hr = S_OK;
    size_t i, j, k;

    program->node_order = vkd3d_calloc(max(count, 1), sizeof(*program->node_order));
    program->node_in_graph = vkd3d_calloc(max(count, 1), sizeof(*program->node_in_graph));
    expanded = vkd3d_calloc(max(count, 1), sizeof(*expanded));
    listed = vkd3d_calloc(max(count, 1), sizeof(*listed));
    written = vkd3d_calloc(max(count, 1), sizeof(*written));
    if (!program->node_order || !program->node_in_graph || !expanded || !listed || !written)
    {
        hr = E_OUTOFMEMORY;
        goto out;
    }

    /* The size of an array output without [NodeArraySize] comes from the nodes that exist. */
    for (i = 0; i < count; i++)
    {
        for (j = 0; j < data->entry_points[i].node_outputs_count; j++)
        {
            output = &data->entry_points[i].node_outputs[j];
            if (output->node_array_size == UINT32_MAX)
            {
                output->node_array_size = d3d12_work_graph_find_array_size_by_id(
                        data->entry_points, data->entry_points_count, output->node_id);
            }
        }
    }

    for (i = 0; i < program->explicit_defined_nodes_count; i++)
    {
        /* d3d12_wg_state_object_apply_node_overrides() checked the node type and the export. */
        defined = d3d12_wg_state_object_find_exported_entry_point(data,
                program->explicit_defined_nodes[i].Shader.Shader);
        entry = defined - data->entry_points;
        if (defined->node_input->is_program_entry)
            hr = d3d12_wg_state_object_population_expand(data, program, expanded, entry);
        else
            d3d12_wg_state_object_population_add(program, entry);
        if (FAILED(hr))
            goto out;
    }

    for (i = 0; i < program->explicit_entry_point_count; i++)
    {
        node_index = d3d12_work_graph_find_node_by_id_wchar(data->entry_points, data->entry_points_count,
                program->explicit_entry_points[i].Name, program->explicit_entry_points[i].ArrayIndex);
        if (node_index == UINT32_MAX)
        {
            WARN("Couldn't find node index for %s[%u].\n", debugstr_w(program->explicit_entry_points[i].Name),
                    program->explicit_entry_points[i].ArrayIndex);
            hr = E_INVALIDARG;
            goto out;
        }

        listed[node_index] = true;
        if (FAILED(hr = d3d12_wg_state_object_population_expand(data, program, expanded, node_index)))
            goto out;
    }

    if (program->flags & D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES)
    {
        for (i = 0; i < data->export_order_count; i++)
        {
            for (j = 0; j < count; j++)
            {
                if (data->entry_points[j].node_input == data->export_order[i])
                {
                    if (FAILED(hr = d3d12_wg_state_object_population_expand(data, program, expanded, j)))
                        goto out;
                    break;
                }
            }
        }
    }

    if (!program->node_order_count)
    {
        WARN("Work graph %s has no nodes.\n", debugstr_w(program->name));
        hr = E_INVALIDARG;
        goto out;
    }

    /* The writers are the nodes in the graph, other than the node itself. */
    for (i = 0; i < count; i++)
    {
        if (!program->node_in_graph[i])
            continue;

        for (j = 0; j < data->entry_points[i].node_outputs_count; j++)
        {
            output = &data->entry_points[i].node_outputs[j];
            node_array_size = output->node_array_size ? output->node_array_size : 1;
            for (k = 0; k < node_array_size; k++)
            {
                node_index = d3d12_work_graph_find_node_by_id(data->entry_points, data->entry_points_count,
                        output->node_id, output->node_array_index + k);
                if (node_index != UINT32_MAX && node_index != i)
                    written[node_index] = true;
            }
        }
    }

    /* A node that shares the input of another node gets its records from the writers of that node. */
    for (i = 0; i < count; i++)
    {
        node_input = data->entry_points[i].node_input;
        if (!program->node_in_graph[i] || !node_input->node_share_input_id ||
                *node_input->node_share_input_id == '\0')
            continue;

        node_index = d3d12_work_graph_find_node_by_id(data->entry_points, data->entry_points_count,
                node_input->node_share_input_id, node_input->node_share_input_array_index);

        /* If we don't find the node, assume there is no sharing to care about anyway, so *shrug*. */
        if (node_index != UINT32_MAX)
            written[i] = written[node_index];
    }

    /* amdgpu-wddm fork: a graph of the DDI has the entry points of pEntrypoints, in that order (the entry point
     * indices of the runtime, also in GPU memory). Every other node of the graph must be a node that is written and
     * not an entry point, or the runtime and the engine disagree about the graph. */
    if (program->flags & VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM)
    {
        for (i = 0; i < program->node_order_count; i++)
        {
            entry = program->node_order[i];
            node_input = data->entry_points[entry].node_input;
            if (!listed[entry] && (node_input->is_program_entry || !written[entry]))
            {
                ERR("Node %s[%u] is an entry point that the DDI does not list.\n",
                        node_input->node_id, node_input->node_array_index);
                hr = E_INVALIDARG;
                goto out;
            }
        }

        for (i = 0; i < program->explicit_entry_point_count; i++)
        {
            node_index = d3d12_work_graph_find_node_by_id_wchar(data->entry_points, data->entry_points_count,
                    program->explicit_entry_points[i].Name, program->explicit_entry_points[i].ArrayIndex);
            if (FAILED(hr = d3d12_wg_state_object_program_add_node_to_level(program, data, node_index, 0, false)))
                goto out;
        }

        if (program->levels[0].nodes_count != program->explicit_entry_point_count)
        {
            ERR("Work graph %s lists an entry point twice.\n", debugstr_w(program->name));
            hr = E_INVALIDARG;
            goto out;
        }
    }

    for (i = 0; i < program->node_order_count && !(program->flags & VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM); i++)
    {
        entry = program->node_order[i];
        node_input = data->entry_points[entry].node_input;
        if (!listed[entry] && !node_input->is_program_entry && written[entry])
            continue;

        if (!listed[entry] && !node_input->is_program_entry &&
                !(program->flags & D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES))
        {
            WARN("Node %s[%u] is not an entry point, and no node writes to it.\n",
                    node_input->node_id, node_input->node_array_index);
            hr = E_INVALIDARG;
            goto out;
        }

        if (FAILED(hr = d3d12_wg_state_object_program_add_node_to_level(program, data, entry, 0, false)))
            goto out;
    }

    for (i = 0; i < program->node_order_count; i++)
    {
        entry = program->node_order[i];
        if (!d3d12_wg_state_object_program_node_in_levels(program, entry))
        {
            WARN("Node %s[%u] cannot be reached from an entry point.\n",
                    data->entry_points[entry].node_input->node_id,
                    data->entry_points[entry].node_input->node_array_index);
            hr = E_INVALIDARG;
            goto out;
        }
    }

out:
    vkd3d_free(expanded);
    vkd3d_free(listed);
    vkd3d_free(written);
    return hr;
}

/* amdgpu-wddm fork: a node with a local root signature and no table index gets the lowest unused
 * index 0, 1, 2 and so on, in the order the node is added to the graph (WorkGraphs.md,
 * [NodeLocalRootArgumentsTableIndex(index)] and D3D12_WORK_GRAPH_DESC). Explicit indices stay. */
static HRESULT d3d12_wg_state_object_assign_local_root_table_indices(
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program)
{
    const struct d3d12_state_object_association *local_rs_assoc;
    size_t count = data->entry_points_count;
    uint32_t *indices, candidate;
    const unsigned int *order;
    const bool *in_program;
    uint32_t node_index;
    size_t order_count;
    size_t i, j;
    bool used;

    if (!(indices = vkd3d_malloc(max(program->num_pipelines, 1) * sizeof(*indices))))
        return E_OUTOFMEMORY;
    program->local_root_table_indices = indices;
    for (i = 0; i < program->num_pipelines; i++)
        indices[i] = UINT32_MAX;

    /* The population order, see d3d12_wg_state_object_populate_program(). */
    in_program = program->node_in_graph;
    order = program->node_order;
    order_count = program->node_order_count;

    for (i = 0; i < count; i++)
        if (in_program[i] && data->entry_points[i].node_input)
            indices[i] = data->entry_points[i].node_input->local_root_arguments_table_index;

    /* The runtime assigned the indices of a graph of the DDI: a node without one has no local root signature. */
    if (program->flags & VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM)
        order_count = 0;

    for (i = 0; i < order_count; i++)
    {
        node_index = order[i];
        if (indices[node_index] != UINT32_MAX)
            continue;

        local_rs_assoc = d3d12_state_object_find_association(VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE,
                data->associations, data->associations_count, NULL, 0, &data->entry_points[node_index], NULL);
        if (!local_rs_assoc || !local_rs_assoc->root_signature)
            continue;

        for (candidate = 0; candidate != UINT32_MAX; candidate++)
        {
            used = false;
            for (j = 0; j < count && !used; j++)
                used = in_program[j] && indices[j] == candidate;
            if (!used)
                break;
        }

        indices[node_index] = candidate;
        TRACE("Node %s[%u] gets local root arguments table index %u.\n",
                data->entry_points[node_index].node_input->node_id,
                data->entry_points[node_index].node_input->node_array_index, candidate);
    }

    /* A depth slot uses the table entry of its node. */
    for (i = count; i < program->num_pipelines; i++)
        indices[i] = indices[program->slot_entry[i]];

    return S_OK;
}

static HRESULT d3d12_wg_state_object_parse_subobjects(
        struct d3d12_wg_state_object_data *data, struct d3d12_device *device,
        const D3D12_STATE_OBJECT_DESC *desc)
{
    struct d3d12_root_signature *root_signature;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < desc->NumSubobjects; i++)
    {
        const D3D12_STATE_SUBOBJECT *obj = &desc->pSubobjects[i];
        if (obj->Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                obj->Type != D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK)
        {
            if (FAILED(hr = d3d12_wg_state_object_parse_subobject(data, device, obj,
                    VKD3D_ASSOCIATION_PRIORITY_DECLARED_STATE_OBJECT)))
                return hr;
        }
    }

    /* Make sure all child state has been parsed. */
    for (i = 0; i < data->subobjects_count; i++)
    {
        D3D12_GLOBAL_ROOT_SIGNATURE obj_root_signature;
        D3D12_STATE_SUBOBJECT obj;
        obj.pDesc = NULL;

        switch (data->subobjects[i].kind)
        {
            case VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE:
            case VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE:
                /* No DXBC header here, just raw root signature binary. */
                if (FAILED(hr = d3d12_root_signature_create_raw(device,
                        data->subobjects[i].data.payload.data,
                        data->subobjects[i].data.payload.size, &root_signature)))
                    return hr;

                d3d12_root_signature_inc_ref(root_signature);
                ID3D12RootSignature_Release(&root_signature->ID3D12RootSignature_iface);

                obj_root_signature.pGlobalRootSignature = &root_signature->ID3D12RootSignature_iface;
                obj.Type = data->subobjects[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE ?
                        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE :
                        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
                obj.pDesc = &obj_root_signature;

                vkd3d_array_reserve((void**)&data->subobject_root_signatures, &data->subobject_root_signatures_size,
                        data->subobject_root_signatures_count + 1, sizeof(*data->subobject_root_signatures));
                data->subobject_root_signatures[data->subobject_root_signatures_count++] = root_signature;
                break;

            default:
                break;
        }

        if (obj.pDesc && FAILED(hr = d3d12_wg_state_object_parse_subobject(
                data, device, &obj, VKD3D_ASSOCIATION_PRIORITY_DXIL_SUBOBJECT)))
            return hr;
    }

    for (i = 0; i < desc->NumSubobjects; i++)
    {
        const D3D12_STATE_SUBOBJECT *obj = &desc->pSubobjects[i];
        /* Now we can parse DXIL subobject -> export associations. */
        if (obj->Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION)
        {
            if (FAILED(hr = d3d12_wg_state_object_parse_subobject(data, device, obj,
                    VKD3D_ASSOCIATION_PRIORITY_DECLARED_STATE_OBJECT)))
                return hr;
        }
    }

    /* Finally, parse subobject version of DXIL subobject to export. */
    for (i = 0; i < data->subobjects_count; i++)
    {
        if (data->subobjects[i].kind == VKD3D_SHADER_SUBOBJECT_KIND_SUBOBJECT_TO_EXPORTS_ASSOCIATION)
        {
            D3D12_STATE_SUBOBJECT obj;
            obj.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
            obj.pDesc = &data->subobjects[i].data.association;
            if (FAILED(hr = d3d12_wg_state_object_parse_subobject(data, device, &obj,
                    VKD3D_ASSOCIATION_PRIORITY_DXIL_SUBOBJECT)))
                return hr;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_QueryInterface(ID3D12StateObject *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (!object)
        return E_POINTER;

    if (IsEqualGUID(riid, &IID_ID3D12StateObject)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12StateObject_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_ID3D12StateObjectProperties) || IsEqualGUID(riid, &IID_ID3D12StateObjectProperties1))
    {
        struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);
        ID3D12StateObjectProperties1_AddRef(&state_object->ID3D12StateObjectProperties1_iface);
        *object = &state_object->ID3D12StateObjectProperties1_iface;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_ID3D12WorkGraphProperties))
    {
        struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);
        ID3D12WorkGraphProperties_AddRef(&state_object->ID3D12WorkGraphProperties_iface);
        *object = &state_object->ID3D12WorkGraphProperties_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static inline struct d3d12_wg_state_object *impl_from_ID3D12StateObjectProperties(d3d12_state_object_properties_iface *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_wg_state_object, ID3D12StateObjectProperties1_iface);
}

static inline struct d3d12_wg_state_object *impl_from_ID3D12WorkGraphProperties(d3d12_work_graph_properties_iface *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_wg_state_object, ID3D12WorkGraphProperties_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_properties_QueryInterface(d3d12_state_object_properties_iface *iface,
        REFIID riid, void **object)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12StateObjectProperties(iface);
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);
    return d3d12_wg_state_object_QueryInterface(&state_object->ID3D12StateObject_iface, riid, object);
}

static HRESULT STDMETHODCALLTYPE d3d12_work_graph_properties_QueryInterface(d3d12_work_graph_properties_iface *iface,
        REFIID riid, void **object)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12WorkGraphProperties(iface);
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);
    return d3d12_wg_state_object_QueryInterface(&state_object->ID3D12StateObject_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d12_wg_state_object_AddRef(ID3D12StateObject *iface)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);
    unsigned int refcount = InterlockedIncrement(&state_object->refcount);

    TRACE("%p increasing refcount to %u.\n", state_object, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_wg_state_object_properties_AddRef(d3d12_state_object_properties_iface *iface)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12StateObjectProperties(iface);
    unsigned int refcount = InterlockedIncrement(&state_object->refcount);

    TRACE("%p increasing refcount to %u.\n", state_object, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_work_graph_properties_AddRef(d3d12_work_graph_properties_iface *iface)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12WorkGraphProperties(iface);
    unsigned int refcount = InterlockedIncrement(&state_object->refcount);

    TRACE("%p increasing refcount to %u.\n", state_object, refcount);

    return refcount;
}

static void d3d12_wg_state_object_cleanup_allocation(struct d3d12_wg_state_object_ring *ring, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    if (ring->allocation.vk_memory)
        vkd3d_free_device_memory(device, &ring->allocation);
    VK_CALL(vkDestroyBuffer(device->vk_device, ring->vk_buffer, NULL));
}

static void d3d12_wg_state_object_cleanup(struct d3d12_wg_state_object *state_object)
{
    const struct vkd3d_vk_device_procs *vk_procs = &state_object->device->vk_procs;
    unsigned int i;

    d3d12_wg_state_object_free_programs(state_object->programs, state_object->programs_count, state_object->device);
    vkd3d_shader_dxil_free_library_entry_points(state_object->entry_points, state_object->entry_points_count);

    for (i = 0; i < state_object->modules_count; i++)
    {
        VK_CALL(vkDestroyShaderModule(state_object->device->vk_device, state_object->modules[i].vk_module, NULL));
        vkd3d_free(state_object->modules[i].mappings);
        if (state_object->modules[i].root_signature)
            d3d12_root_signature_dec_ref(state_object->modules[i].root_signature);
    }
    vkd3d_free(state_object->modules);

    d3d12_wg_state_object_cleanup_allocation(&state_object->lost_records, state_object->device);
}

/* amdgpu-wddm fork: reads the lost record counts that the GPU wrote. The executions can still run. */
static void d3d12_wg_state_object_read_lost_records(struct d3d12_wg_state_object *state_object,
        uint64_t *events, uint64_t *records, bool *lower_bound)
{
    volatile struct vkd3d_workgraph_lost_records *lost = state_object->lost_records_host;
    uint32_t hi, lo;

    *events = 0;
    *records = 0;
    *lower_bound = false;
    if (!lost)
        return;

    do
    {
        hi = lost->records_hi;
        lo = lost->records_lo;
    } while (hi != lost->records_hi);

    *events = lost->events;
    *records = ((uint64_t)hi << 32) | lo;
    *lower_bound = !!(lost->flags & VKD3D_WORKGRAPH_LOST_RECORDS_LOWER_BOUND);
}

/* amdgpu-wddm fork: an ERR line when more records were lost since the last line. This runs when the
 * application records the next DispatchGraph with the state object and when it releases the state object, so it
 * is late, but the loss is in the log, and with the debugger log of the system route also in the debugger. */
static void d3d12_wg_state_object_report_lost_records(struct d3d12_wg_state_object *state_object)
{
    uint64_t events, records;
    uint32_t reported;
    bool lower_bound;

    d3d12_wg_state_object_read_lost_records(state_object, &events, &records, &lower_bound);
    reported = vkd3d_atomic_uint32_load_explicit(&state_object->lost_events_reported, vkd3d_memory_order_relaxed);
    if ((uint32_t)events == reported)
        return;
    if (vkd3d_atomic_uint32_compare_exchange(&state_object->lost_events_reported, reported, (uint32_t)events,
            vkd3d_memory_order_relaxed, vkd3d_memory_order_relaxed) != reported)
        return;

    ERR("Work graph %p: %"PRIu64" times records did not fit, %s%"PRIu64" records were not launched in total. The "
            "merged inputs of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT did not fit in the staging range, or the node "
            "outputs of a level did not fit in the payload memory.%s\n",
            (void *)state_object, events, lower_bound ? "at least " : "", records,
            lower_bound ? " A thread asked for more records than one allocation can hold, which is not valid, so "
            "the count is a lower bound." : "");
}

/* amdgpu-wddm fork: an ERR line when close abandoned work since the last line, at the same points as the lost
 * records. The item counts are 128-bit, printed in hexadecimal, so that they are exact. */
static void d3d12_wg_state_object_report_abandoned(struct d3d12_wg_state_object *state_object)
{
    volatile struct vkd3d_workgraph_abandoned *abandoned = state_object->abandoned_host;
    static const char * const kinds[] = { "groups", "coalescing slots", "threads" };
    uint32_t events, reported, reasons;
    bool exhausted, fault;
    char items[3][64];
    unsigned int i;

    if (!abandoned)
        return;
    events = abandoned->events;
    reported = vkd3d_atomic_uint32_load_explicit(&state_object->abandoned_events_reported, vkd3d_memory_order_relaxed);
    if (events == reported)
        return;
    if (vkd3d_atomic_uint32_compare_exchange(&state_object->abandoned_events_reported, reported, events,
            vkd3d_memory_order_relaxed, vkd3d_memory_order_relaxed) != reported)
        return;

    reasons = abandoned->reasons;
    for (i = 0; i < ARRAY_SIZE(items); i++)
    {
        snprintf(items[i], sizeof(items[i]), "%s 0x%08x%08x%08x%08x", kinds[i], abandoned->items[i][3],
                abandoned->items[i][2], abandoned->items[i][1], abandoned->items[i][0]);
    }
    exhausted = !!(reasons & (1u << VKD3D_WORKGRAPH_DRAIN_REASON_EXHAUSTED));
    fault = !!(reasons & (1u << VKD3D_WORKGRAPH_DRAIN_REASON_FAULT));
    ERR("Work graph %p: %u times work was abandoned when the rounds ended (%s%s%s). Waiting work: %s, %s, %s, "
            "input records not taken %"PRIu64", abandoned %"PRIu64" records in the record pools.\n", (void *)state_object,
            events, exhausted ? "the last round had waiting work" : "", exhausted && fault ? ", " : "",
            fault ? "a round made no progress" : "",
            items[0], items[1], items[2], ((uint64_t)abandoned->inputs_hi << 32) | abandoned->inputs_lo,
            ((uint64_t)abandoned->records_hi << 32) | abandoned->records_lo);
}

static void d3d12_wg_state_object_dec_ref(struct d3d12_wg_state_object *state_object)
{
    unsigned int refcount = InterlockedDecrement(&state_object->internal_refcount);

    TRACE("%p decreasing internal refcount to %u.\n", state_object, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = state_object->device;
        d3d12_wg_state_object_report_lost_records(state_object);
        d3d12_wg_state_object_report_abandoned(state_object);
        vkd3d_private_store_destroy(&state_object->private_store);
        d3d12_wg_state_object_cleanup(state_object);
        vkd3d_free(state_object);
        d3d12_device_release(device);
    }
}

static ULONG d3d12_wg_state_object_release(struct d3d12_wg_state_object *state_object)
{
    unsigned int refcount = InterlockedDecrement(&state_object->refcount);

    TRACE("%p decreasing refcount to %u.\n", state_object, refcount);

    if (!refcount)
        d3d12_wg_state_object_dec_ref(state_object);
    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_wg_state_object_Release(ID3D12StateObject *iface)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);
    return d3d12_wg_state_object_release(state_object);
}

static ULONG STDMETHODCALLTYPE d3d12_wg_state_object_properties_Release(d3d12_state_object_properties_iface *iface)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12StateObjectProperties(iface);
    return d3d12_wg_state_object_release(state_object);
}

static ULONG STDMETHODCALLTYPE d3d12_work_graph_properties_Release(d3d12_work_graph_properties_iface *iface)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12WorkGraphProperties(iface);
    return d3d12_wg_state_object_release(state_object);
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_GetPrivateData(ID3D12StateObject *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    /* amdgpu-wddm fork, lab flavour: the lost record counts, for tests. */
    if (IsEqualGUID(guid, &amdgpu_wddm_wg_status_guid))
    {
        uint64_t counts[3];
        bool lower_bound;
        UINT size;

        if (!data_size)
            return E_INVALIDARG;
        if (data && *data_size < 2 * sizeof(counts[0]))
            return E_INVALIDARG;
        size = data && *data_size < sizeof(counts) ? 2 * sizeof(counts[0]) : sizeof(counts);
        if (data)
        {
            d3d12_wg_state_object_read_lost_records(state_object, &counts[0], &counts[1], &lower_bound);
            counts[2] = lower_bound;
            memcpy(data, counts, size);
        }
        *data_size = size;
        return S_OK;
    }

    /* amdgpu-wddm fork, lab flavour: the canary ranges of the first program, for tests. */
    if (IsEqualGUID(guid, &amdgpu_wddm_wg_canary_guid))
    {
        struct amdgpu_wddm_wg_canaries canaries;
        unsigned int i;

        if (!data_size)
            return E_INVALIDARG;
        if (data && *data_size < sizeof(canaries))
            return E_INVALIDARG;
        if (data)
        {
            memset(&canaries, 0, sizeof(canaries));
            canaries.pattern = VKD3D_WORKGRAPH_CANARY_PATTERN;
            if (state_object->programs_count)
            {
                canaries.count = state_object->programs[0].canary_count;
                for (i = 0; i < canaries.count; i++)
                {
                    canaries.ranges[i].offset = state_object->programs[0].canary_offsets[i];
                    canaries.ranges[i].size = VKD3D_WORKGRAPH_CANARY_SIZE;
                }
                canaries.sinks[0].offset = state_object->programs[0].payload0_scratch_offset +
                        state_object->programs[0].payload_ring_size;
                canaries.sinks[1].offset = state_object->programs[0].payload1_scratch_offset +
                        state_object->programs[0].payload_ring_size;
                canaries.sinks[0].size = canaries.sinks[1].size = VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE;
            }
            memcpy(data, &canaries, sizeof(canaries));
        }
        *data_size = sizeof(canaries);
        return S_OK;
    }

    /* amdgpu-wddm fork, lab flavour: the round frame of the first program, for tests. */
    if (IsEqualGUID(guid, &amdgpu_wddm_wg_drain_guid))
    {
        volatile struct vkd3d_workgraph_abandoned *abandoned = state_object->abandoned_host;
        struct amdgpu_wddm_wg_drain drain;

        if (!data_size)
            return E_INVALIDARG;
        if (data && *data_size < sizeof(drain))
            return E_INVALIDARG;
        if (data)
        {
            memset(&drain, 0, sizeof(drain));
            if (state_object->programs_count)
            {
                drain.rounds = state_object->programs[0].drain_rounds;
                drain.pending_rounds = state_object->programs[0].drain_pending;
                drain.control_offset = state_object->programs[0].drain_control_scratch_offset;
                drain.pool_state_offset = state_object->programs[0].pool_state_scratch_offset;
                drain.pool_state_stride = state_object->programs[0].pool_state_stride;
                drain.pool_states = state_object->programs[0].num_levels;
            }
            if (abandoned)
            {
                drain.abandoned_events = abandoned->events;
                drain.abandoned_reasons = abandoned->reasons;
                drain.abandoned_last_reason = abandoned->last_reason;
                drain.abandoned_flags = abandoned->flags;
                drain.abandoned_records = ((uint64_t)abandoned->records_hi << 32) | abandoned->records_lo;
            }
            drain.hook_calls = vkd3d_atomic_uint32_load_explicit(&state_object->drain_hook_calls,
                    vkd3d_memory_order_relaxed);
            drain.violations = vkd3d_atomic_uint32_load_explicit(&state_object->drain_violations,
                    vkd3d_memory_order_relaxed);
            memcpy(data, &drain, sizeof(drain));
        }
        *data_size = sizeof(drain);
        return S_OK;
    }

    /* amdgpu-wddm fork, lab flavour: the record pools of the first program, for tests. */
    if (IsEqualGUID(guid, &amdgpu_wddm_wg_pool_guid))
    {
        const struct d3d12_wg_state_object_program *program;
        struct amdgpu_wddm_wg_pools *pools;
        struct d3d12_wg_drain_layout layout;
        unsigned int n, level, half;

        if (!data_size)
            return E_INVALIDARG;
        if (data && *data_size < sizeof(*pools))
            return E_INVALIDARG;
        if (data)
        {
            /* The struct is large, so it is written in place. */
            pools = data;
            memset(pools, 0, sizeof(*pools));
            if (state_object->programs_count && state_object->programs[0].pool_valid)
            {
                program = &state_object->programs[0];
                pools->valid = 1;
                pools->pools = program->num_levels;
                pools->execution = program->pool_execution;
                pools->fixed_size = program->pool_fixed_size;
                pools->staging_size = program->pool_staging_size;
                pools->sink_size = program->pool_sink_size;
                pools->min_size = program->pool_min_size;
                pools->max_size = program->pool_max_size;
                memcpy(pools->w, program->pool_w, sizeof(pools->w));
                memcpy(pools->bound, program->pool_bound, sizeof(pools->bound));
                pools->layouts[0].size = program->pool_min_size - 16;
                pools->layouts[1].size = program->pool_min_size;
                pools->layouts[2].size = program->pool_min_size + VKD3D_WORKGRAPH_BACKING_GRANULARITY + 8;
                pools->layouts[3].size = program->pool_max_size;
                pools->layouts[4].size = program->pool_max_size + VKD3D_WORKGRAPH_BACKING_GRANULARITY;
                for (n = 0; n < ARRAY_SIZE(pools->layouts); n++)
                {
                    if (!d3d12_wg_drain_layout(program, pools->layouts[n].size, &layout))
                        continue;
                    pools->layouts[n].used = layout.backing_size;
#define copy_region(dst, src) ((dst).offset = (src).offset, (dst).size = (src).size)
                    copy_region(pools->layouts[n].fixed, layout.fixed);
                    copy_region(pools->layouts[n].entries, layout.entries);
                    copy_region(pools->layouts[n].offsets, layout.offsets);
                    copy_region(pools->layouts[n].empty_state, layout.empty_state);
                    for (level = 0; level < MAX_WORKGRAPH_LEVELS; level++)
                    {
                        for (half = 0; half < 2; half++)
                        {
                            copy_region(pools->layouts[n].payload[level][half], layout.payload[level][half]);
                            copy_region(pools->layouts[n].sink[level][half], layout.sink[level][half]);
                        }
                    }
                    copy_region(pools->layouts[n].staging, layout.staging);
#undef copy_region
                }
            }
        }
        *data_size = sizeof(*pools);
        return S_OK;
    }

    /* amdgpu-wddm fork, lab flavour: the slots of the nodes of the first program, for tests. */
    if (IsEqualGUID(guid, &amdgpu_wddm_wg_slot_guid))
    {
        const struct d3d12_wg_state_object_program *program;
        struct amdgpu_wddm_wg_slots *slots;
        unsigned int i, entry, nodes;
        size_t size;

        if (!data_size)
            return E_INVALIDARG;
        program = state_object->programs_count ? &state_object->programs[0] : NULL;
        nodes = program ? program->node_order_count : 0;
        size = sizeof(*slots) + (size_t)nodes * sizeof(slots->node[0]);
        if (size > UINT_MAX)
            return E_INVALIDARG;
        if (data && *data_size < size)
            return E_INVALIDARG;
        if (data)
        {
            slots = data;
            memset(slots, 0, size);
            slots->subgroup_size = state_object->device->device_info.vulkan_1_3_properties.maxSubgroupSize;
            slots->wg_divider = state_object->device->meta_ops.workgraph.require_wg_divider;
            slots->slots = program ? program->num_pipelines : 0;
            slots->nodes = nodes;
            for (i = 0; i < nodes; i++)
            {
                entry = program->node_order[i];
                slots->node[i].slot = entry;
                slots->node[i].share_slot = entry < program->num_pipelines && program->share_mapping ?
                        program->share_mapping[entry] : UINT32_MAX;
            }
        }
        *data_size = size;
        return S_OK;
    }
#endif

    return vkd3d_get_private_data(&state_object->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_SetPrivateData(ID3D12StateObject *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&state_object->private_store, guid, data_size, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_SetPrivateDataInterface(ID3D12StateObject *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&state_object->private_store, guid, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_wg_state_object_GetDevice(ID3D12StateObject *iface,
        REFIID iid, void **device)
{
    struct d3d12_wg_state_object *state_object = wg_impl_from_ID3D12StateObject(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(state_object->device, iid, device);
}

static CONST_VTBL struct ID3D12StateObjectVtbl d3d12_wg_state_object_vtbl =
{
    /* IUnknown methods */
    d3d12_wg_state_object_QueryInterface,
    d3d12_wg_state_object_AddRef,
    d3d12_wg_state_object_Release,
    /* ID3D12Object methods */
    d3d12_wg_state_object_GetPrivateData,
    d3d12_wg_state_object_SetPrivateData,
    d3d12_wg_state_object_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_wg_state_object_GetDevice,
};

/* These only apply to RT PSOs. */
static void * STDMETHODCALLTYPE d3d12_wg_state_object_properties_GetShaderIdentifier(d3d12_state_object_properties_iface *iface,
        LPCWSTR export_name)
{
    TRACE("iface %p, export_name %s.\n", iface, debugstr_w(export_name));
    return NULL;
}

static UINT64 STDMETHODCALLTYPE d3d12_wg_state_object_properties_GetShaderStackSize(d3d12_state_object_properties_iface *iface,
        LPCWSTR export_name)
{
    TRACE("iface %p, export_name %s.\n", iface, debugstr_w(export_name));
    return UINT32_MAX;
}

static UINT64 STDMETHODCALLTYPE d3d12_wg_state_object_properties_GetPipelineStackSize(d3d12_state_object_properties_iface *iface)
{
    TRACE("iface %p\n", iface);
    return UINT32_MAX;
}

static void STDMETHODCALLTYPE d3d12_wg_state_object_properties_SetPipelineStackSize(d3d12_state_object_properties_iface *iface,
        UINT64 stack_size_in_bytes)
{
    TRACE("iface %p, stack_size_in_bytes %llu!\n", iface, (unsigned long long)stack_size_in_bytes);
}

static D3D12_PROGRAM_IDENTIFIER * STDMETHODCALLTYPE d3d12_wg_state_object_properties_GetProgramIdentifier(
        d3d12_state_object_properties_iface *iface,
        D3D12_PROGRAM_IDENTIFIER *ret,
        LPCWSTR pProgramName)
{
    struct d3d12_wg_state_object *state_object = impl_from_ID3D12StateObjectProperties(iface);
    size_t i;

    TRACE("iface %p, ret %p, pProgramName %s!\n", iface, ret, debugstr_w(pProgramName));

    memset(ret, 0, sizeof(*ret));
    for (i = 0; i < state_object->programs_count; i++)
    {
        if (vkd3d_export_strequal(state_object->programs[i].name, pProgramName))
        {
            ret->OpaqueData[0] = i;
            ret->OpaqueData[1] = (UINT64)(uintptr_t)state_object; /* Allows sanity checking. */
            break;
        }
    }

    return ret;
}

static CONST_VTBL struct ID3D12StateObjectProperties1Vtbl d3d12_wg_state_object_properties_vtbl =
{
    /* IUnknown methods */
    d3d12_wg_state_object_properties_QueryInterface,
    d3d12_wg_state_object_properties_AddRef,
    d3d12_wg_state_object_properties_Release,
    /* ID3D12StateObjectProperties methods */
    d3d12_wg_state_object_properties_GetShaderIdentifier,
    d3d12_wg_state_object_properties_GetShaderStackSize,
    d3d12_wg_state_object_properties_GetPipelineStackSize,
    d3d12_wg_state_object_properties_SetPipelineStackSize,
    /* ID3D12StateObjectProperties1 methods */
    d3d12_wg_state_object_properties_GetProgramIdentifier,
};

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetNumWorkGraphs(
        d3d12_work_graph_properties_iface *iface)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    TRACE("iface %p\n", iface);
    return object->programs_count;
}

static LPCWSTR STDMETHODCALLTYPE d3d12_work_graph_properties_GetProgramName(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    TRACE("iface %p, WorkGraphIndex %u\n", iface, WorkGraphIndex);
    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return NULL;
    }

    return object->programs[WorkGraphIndex].name;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetWorkGraphIndex(
        d3d12_work_graph_properties_iface *iface,
        LPCWSTR pProgramName)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    size_t i;

    TRACE("iface %p, pProgramName %s\n", iface, debugstr_w(pProgramName));

    for (i = 0; i < object->programs_count; i++)
        if (vkd3d_export_strequal(object->programs[i].name, pProgramName))
            return (UINT)i;

    return -1;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetNumNodes(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    TRACE("iface %p, WorkGraphIndex %u\n", iface, WorkGraphIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    /* amdgpu-wddm fork: the nodes in the order of population (WorkGraphs.md, GetNodeID). */
    program = &object->programs[WorkGraphIndex];
    return program->node_order_count;
}

static unsigned int d3d12_work_graph_properties_node_index_to_entry(
        const struct d3d12_wg_state_object_program *program, UINT NodeIndex)
{
    return NodeIndex < program->node_order_count ? program->node_order[NodeIndex] : -1u;
}

static const struct d3d12_wg_state_object_pipeline *d3d12_work_graph_properties_node_index_pipeline(
        const struct d3d12_wg_state_object_program *program, UINT NodeIndex)
{
    unsigned int index = d3d12_work_graph_properties_node_index_to_entry(program, NodeIndex);
    if (index == -1u)
        return NULL;
    else
        return &program->pipelines[index];
}

static D3D12_NODE_ID * STDMETHODCALLTYPE d3d12_work_graph_properties_GetNodeID(
        d3d12_work_graph_properties_iface *iface,
        D3D12_NODE_ID *ret,
        UINT WorkGraphIndex,
        UINT NodeIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_pipeline *pipeline;
    TRACE("iface %p, ret %p, WorkGraphIndex %u, NodeIndex %u\n", iface, ret, WorkGraphIndex, NodeIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        ret->Name = NULL;
        ret->ArrayIndex = 0;
        return ret;
    }

    pipeline = d3d12_work_graph_properties_node_index_pipeline(&object->programs[WorkGraphIndex], NodeIndex);

    if (pipeline)
    {
        ret->Name = pipeline->name;
        ret->ArrayIndex = pipeline->array_index;
    }
    else
    {
        ret->Name = NULL;
        ret->ArrayIndex = 0;
    }

    return ret;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetNodeIndex(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        D3D12_NODE_ID NodeID)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    unsigned int entry, i;
    TRACE("iface %p, WorkGraphIndex %u, NodeID.Name %s, NodeID.ArrayIndex %u\n",
            iface, WorkGraphIndex, debugstr_w(NodeID.Name), NodeID.ArrayIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    /* amdgpu-wddm fork: the inverse of GetNodeID. */
    program = &object->programs[WorkGraphIndex];
    for (i = 0; i < program->node_order_count; i++)
    {
        entry = program->node_order[i];
        if (vkd3d_export_strequal(NodeID.Name, program->pipelines[entry].name) &&
                NodeID.ArrayIndex == program->pipelines[entry].array_index)
        {
            return i;
        }
    }

    return -1u;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetNodeLocalRootArgumentsTableIndex(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        UINT NodeIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    unsigned int entry;
    TRACE("iface %p, WorkGraphIndex %u, NodeIndex %u\n", iface, WorkGraphIndex, NodeIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    entry = d3d12_work_graph_properties_node_index_to_entry(&object->programs[WorkGraphIndex], NodeIndex);

    if (entry == -1u)
        return -1u;
    else
        return object->programs[WorkGraphIndex].local_root_table_indices[entry];
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetNumEntrypoints(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    TRACE("iface %p, WorkGraphIndex %u\n", iface, WorkGraphIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    program = &object->programs[WorkGraphIndex];
    return program->levels[0].nodes_count;
}

static D3D12_NODE_ID * STDMETHODCALLTYPE d3d12_work_graph_properties_GetEntrypointID(
        d3d12_work_graph_properties_iface *iface,
        D3D12_NODE_ID *ret,
        UINT WorkGraphIndex,
        UINT EntrypointIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    TRACE("iface %p, ret %p, WorkGraphIndex %u, EntrypointIndex %u\n", iface, ret, WorkGraphIndex, EntrypointIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        /* amdgpu-wddm fork: {NULL, 0}, not a NULL pointer, which the caller dereferences. */
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        ret->Name = NULL;
        ret->ArrayIndex = 0;
        return ret;
    }

    program = &object->programs[WorkGraphIndex];
    if (EntrypointIndex >= program->levels[0].nodes_count)
    {
        ERR("EntrypointIndex %u out of range.\n", EntrypointIndex);
        ret->Name = NULL;
        ret->ArrayIndex = 0;
        return ret;
    }

    ret->Name = program->pipelines[program->levels[0].nodes[EntrypointIndex]].name;
    ret->ArrayIndex = program->pipelines[program->levels[0].nodes[EntrypointIndex]].array_index;
    return ret;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetEntrypointIndex(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        D3D12_NODE_ID NodeID)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    unsigned int i;

    TRACE("iface %p, WorkGraphIndex %u, NodeID.Name %s, NodeID.ArrayIndex %u!\n",
            iface, WorkGraphIndex, debugstr_w(NodeID.Name), NodeID.ArrayIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    program = &object->programs[WorkGraphIndex];

    for (i = 0; i < program->levels[0].nodes_count; i++)
    {
        unsigned int node_index = program->levels[0].nodes[i];
        if (vkd3d_export_strequal(program->pipelines[node_index].name, NodeID.Name) &&
                program->pipelines[node_index].array_index == NodeID.ArrayIndex)
        {
            return i;
        }
    }

    return -1;
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetEntrypointRecordSizeInBytes(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        UINT EntrypointIndex)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    const struct d3d12_wg_state_object_program *program;
    unsigned int node_index;
    TRACE("iface %p, WorkGraphIndex %u, EntrypointIndex %u!\n", iface, WorkGraphIndex, EntrypointIndex);

    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        return -1u;
    }

    program = &object->programs[WorkGraphIndex];

    if (EntrypointIndex >= program->levels[0].nodes_count)
    {
        ERR("EntrypointIndex %u out of range.\n", EntrypointIndex);
        return -1u;
    }

    node_index = program->levels[0].nodes[EntrypointIndex];
    /* amdgpu-wddm fork: the application record size, not the internal stride. */
    return object->entry_points[node_index].node_input->record_size;
}

static void STDMETHODCALLTYPE d3d12_work_graph_properties_GetWorkGraphMemoryRequirements(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *pWorkGraphMemoryRequirements)
{
    struct d3d12_wg_state_object *object = impl_from_ID3D12WorkGraphProperties(iface);
    TRACE("iface %p, WorkGraphIndex %u, pWorkGraphMemoryRequirements %p\n", iface, WorkGraphIndex, pWorkGraphMemoryRequirements);
    if (WorkGraphIndex >= object->programs_count)
    {
        ERR("WorkGraphIndex %u is out of bound.\n", WorkGraphIndex);
        memset(pWorkGraphMemoryRequirements, 0, sizeof(*pWorkGraphMemoryRequirements));
        return;
    }

    pWorkGraphMemoryRequirements->MinSizeInBytes = object->programs[WorkGraphIndex].min_backing_size;
    pWorkGraphMemoryRequirements->MaxSizeInBytes = object->programs[WorkGraphIndex].max_backing_size;
    pWorkGraphMemoryRequirements->SizeGranularityInBytes = VKD3D_WORKGRAPH_BACKING_GRANULARITY;
    TRACE("Required scratch size: %"PRIu64" bytes.\n", pWorkGraphMemoryRequirements->MinSizeInBytes);
}

static UINT STDMETHODCALLTYPE d3d12_work_graph_properties_GetEntrypointRecordAlignmentInBytes(
        d3d12_work_graph_properties_iface *iface,
        UINT WorkGraphIndex,
        UINT EntrypointIndex)
{
    FIXME("iface %p, WorkGraphIndex %u, EntrypointIndex %u, stub!\n", iface, WorkGraphIndex, EntrypointIndex);
    /* Do we need to be more specific? */
    return 16;
}

static CONST_VTBL struct ID3D12WorkGraphPropertiesVtbl d3d12_work_graph_properties_vtbl =
{
    /* IUnknown methods */
    d3d12_work_graph_properties_QueryInterface,
    d3d12_work_graph_properties_AddRef,
    d3d12_work_graph_properties_Release,
    /* ID3D12WorkGraphProperties methods */
    d3d12_work_graph_properties_GetNumWorkGraphs,
    d3d12_work_graph_properties_GetProgramName,
    d3d12_work_graph_properties_GetWorkGraphIndex,
    d3d12_work_graph_properties_GetNumNodes,
    d3d12_work_graph_properties_GetNodeID,
    d3d12_work_graph_properties_GetNodeIndex,
    d3d12_work_graph_properties_GetNodeLocalRootArgumentsTableIndex,
    d3d12_work_graph_properties_GetNumEntrypoints,
    d3d12_work_graph_properties_GetEntrypointID,
    d3d12_work_graph_properties_GetEntrypointIndex,
    d3d12_work_graph_properties_GetEntrypointRecordSizeInBytes,
    d3d12_work_graph_properties_GetWorkGraphMemoryRequirements,
    d3d12_work_graph_properties_GetEntrypointRecordAlignmentInBytes,
};

struct d3d12_wg_state_object_spec_constant_tmp
{
    VkSpecializationMapEntry *map_entries;
    uint32_t *spec_data;
    size_t map_entries_size;
    size_t spec_data_count;
    size_t spec_data_size;
};

static HRESULT d3d12_wg_state_object_compile_pipeline(
        struct d3d12_wg_state_object *object,
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program,
        unsigned int entry_point_index,
        struct d3d12_wg_state_object_spec_constant_tmp *tmp)
{
    const struct vkd3d_vk_device_procs *vk_procs = &object->device->vk_procs;
    const struct vkd3d_shader_library_entry_point *entry;
    VkComputePipelineCreateInfo pipeline_info;
    VkPipelineCreateFlags2CreateInfo flags2;
    VkSpecializationInfo spec_info;
    uint32_t spec_constant_index;
    bool is_broadcast;
    unsigned int i, j;
    VkResult vr;

    enum
    {
        SPEC_IS_ENTRY_POINT = 0,
        SPEC_IS_INDIRECT_BDA_STRIDE,
        SPEC_COUNT_NON_BROADCAST,
        SPEC_IS_STATIC_BROADCAST = SPEC_COUNT_NON_BROADCAST,
        SPEC_IS_GRID_IS_UPPER_BOUND,
        SPEC_COUNT_BROADCAST
    };

    /* Already compiled PSO. */
    if (program->pipelines[entry_point_index].vk_non_entry_pipeline != VK_NULL_HANDLE)
        return S_OK;

    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.pSpecializationInfo = &spec_info;
    pipeline_info.stage.pName = "main";
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;

    entry = &data->entry_points[program->slot_entry[entry_point_index]];
    is_broadcast = entry->node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING;

    /* TODO: These can be overridden during compilation. */
    program->pipelines[entry_point_index].name = vkd3d_dup_entry_point(entry->node_input->node_id);
    program->pipelines[entry_point_index].array_index = entry->node_input->node_array_index;

    /* Set up spec constants for node outputs, etc. This will
     * have to be expanded a bit to also cover things like sparse array checks, recursion state, etc. */
    tmp->spec_data_count = entry->node_outputs_count;

    /* Workgroup size is also a spec constant. */
    if (entry->node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_THREAD)
        tmp->spec_data_count += 1;

    tmp->spec_data_count += is_broadcast ? SPEC_COUNT_BROADCAST : SPEC_COUNT_NON_BROADCAST;

    vkd3d_array_reserve((void **)&tmp->spec_data, &tmp->spec_data_size, tmp->spec_data_count, sizeof(*tmp->spec_data));
    vkd3d_array_reserve((void **)&tmp->map_entries, &tmp->map_entries_size, tmp->spec_data_count, sizeof(*tmp->map_entries));

    pipeline_info.stage.module = object->modules[program->slot_entry[entry_point_index]].vk_module;
    if (object->modules[program->slot_entry[entry_point_index]].mappings)
        pipeline_info.stage.pNext = &object->modules[program->slot_entry[entry_point_index]].mappings->mapping_info;

    memset(&flags2, 0, sizeof(flags2));
    flags2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
    flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT | pipeline_info.flags;
    pipeline_info.flags = 0;
    vk_prepend_struct(&pipeline_info, &flags2);

    spec_info.pData = tmp->spec_data;
    spec_info.dataSize = tmp->spec_data_count * sizeof(uint32_t);
    spec_info.pMapEntries = tmp->map_entries;
    spec_info.mapEntryCount = tmp->spec_data_count;

    for (i = 0; i < entry->node_outputs_count; i++)
    {
        uint32_t node_array_size = entry->node_outputs[i].node_array_size ?
                entry->node_outputs[i].node_array_size : 1;

        tmp->map_entries[i].offset = sizeof(uint32_t) * i;
        tmp->map_entries[i].size = sizeof(uint32_t);
        tmp->map_entries[i].constantID = entry->node_outputs[i].node_index_spec_constant_id;

        /* If we have sparse nodes, we may have to look through the full array to find something useful. */
        for (j = 0; j < node_array_size; j++)
        {
            tmp->spec_data[i] = d3d12_work_graph_find_node_by_id(
                    data->entry_points, data->entry_points_count,
                    entry->node_outputs[i].node_id,
                    entry->node_outputs[i].node_array_index + j);

            if (tmp->spec_data[i] != UINT32_MAX)
            {
                /* The shader will end up offseting, so compensate for that here. */
                assert(tmp->spec_data[i] >= j);
                tmp->spec_data[i] -= j;
                break;
            }
        }

        /* This should be impossible since we validated earlier. */
        assert(tmp->spec_data[i] != UINT32_MAX);

        /* amdgpu-wddm fork: a recursive output goes to the slot of the next depth. At the last depth
         * the node must not recurse (GetRemainingRecursionLevels() is 0), so the slot stays.
         * The shader adds the array element to the spec constant. In an output array that holds the node
         * itself, only the element of the node is a valid target: a node must not target another node of its
         * own array (WorkGraphs.md, Node array constraints). So the constant is set to put that element on
         * the depth slot, in uint arithmetic. */
        if (entry->node_input->recursion_factor)
        {
            for (j = 0; j < node_array_size; j++)
            {
                if (d3d12_work_graph_find_node_by_id(data->entry_points, data->entry_points_count,
                        entry->node_outputs[i].node_id, entry->node_outputs[i].node_array_index + j) ==
                        program->slot_entry[entry_point_index])
                {
                    tmp->spec_data[i] = d3d12_wg_state_object_program_depth_slot(program,
                            program->slot_entry[entry_point_index],
                            min(program->slot_depth[entry_point_index] + 1,
                                    entry->node_input->recursion_factor)) - j;
                    break;
                }
            }
        }
    }

    spec_constant_index = entry->node_outputs_count;

    if (entry->node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_THREAD)
    {
        tmp->map_entries[spec_constant_index].offset = sizeof(uint32_t) * spec_constant_index;
        tmp->map_entries[spec_constant_index].size = sizeof(uint32_t);
        tmp->map_entries[spec_constant_index].constantID = entry->node_input->thread_group_size_spec_id[0];
        tmp->spec_data[spec_constant_index] = THREAD_COALESCE_COUNT;
        spec_constant_index++;
    }

    /* TODO: node override for workgroup size */

    {
        const struct
        {
            uint32_t spec_offset;
            uint32_t spec_id;
        } entries[] = {
            { SPEC_IS_GRID_IS_UPPER_BOUND, entry->node_input->dispatch_grid_is_upper_bound_spec_id },
            { SPEC_IS_STATIC_BROADCAST, entry->node_input->is_static_broadcast_node_spec_id },
            { SPEC_IS_INDIRECT_BDA_STRIDE, entry->node_input->is_indirect_bda_stride_program_entry_spec_id },
            { SPEC_IS_ENTRY_POINT, entry->node_input->is_entry_point_spec_id },
        };

        for (i = 0; i < ARRAY_SIZE(entries); i++)
        {
            VkSpecializationMapEntry *map_entry = &tmp->map_entries[spec_constant_index + entries[i].spec_offset];

            if (!is_broadcast && entries[i].spec_offset >= SPEC_COUNT_NON_BROADCAST)
                continue;

            assert(entries[i].spec_id != UINT32_MAX);
            map_entry->offset = sizeof(uint32_t) * (spec_constant_index + entries[i].spec_offset);
            map_entry->size = sizeof(uint32_t);
            map_entry->constantID = entries[i].spec_id;
        }
    }

    /* CPU node entry, static payload. */
    if (is_broadcast)
    {
        tmp->spec_data[spec_constant_index + SPEC_IS_ENTRY_POINT] = 1;
        tmp->spec_data[spec_constant_index + SPEC_IS_INDIRECT_BDA_STRIDE] = 0;
        tmp->spec_data[spec_constant_index + SPEC_IS_GRID_IS_UPPER_BOUND] = 0;
        tmp->spec_data[spec_constant_index + SPEC_IS_STATIC_BROADCAST] = 1;

        vr = VK_CALL(vkCreateComputePipelines(object->device->vk_device,
                VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                &program->pipelines[entry_point_index].vk_static_cpu_node_entry_pipeline));

        if (vr < 0)
        {
            ERR("Failed to create pipeline, vr %d\n", vr);
            return hresult_from_vk_result(vr);
        }
    }

    /* CPU node entry, multiple payloads. */
    tmp->spec_data[spec_constant_index + SPEC_IS_INDIRECT_BDA_STRIDE] = 0;
    tmp->spec_data[spec_constant_index + SPEC_IS_ENTRY_POINT] = 1;

    if (is_broadcast)
    {
        tmp->spec_data[spec_constant_index +
                SPEC_IS_GRID_IS_UPPER_BOUND] = entry->node_input->dispatch_grid_is_upper_bound;
        tmp->spec_data[spec_constant_index + SPEC_IS_STATIC_BROADCAST] = 0;
    }

    vr = VK_CALL(vkCreateComputePipelines(object->device->vk_device,
            VK_NULL_HANDLE, 1, &pipeline_info, NULL,
            &program->pipelines[entry_point_index].vk_cpu_node_entry_pipeline));
    if (vr < 0)
    {
        ERR("Failed to create pipeline, vr %d\n", vr);
        return hresult_from_vk_result(vr);
    }

    /* GPU entry */
    tmp->spec_data[spec_constant_index + SPEC_IS_INDIRECT_BDA_STRIDE] = 1;
    tmp->spec_data[spec_constant_index + SPEC_IS_ENTRY_POINT] = 1;

    if (is_broadcast)
    {
        tmp->spec_data[spec_constant_index +
                SPEC_IS_GRID_IS_UPPER_BOUND] = entry->node_input->dispatch_grid_is_upper_bound;
        tmp->spec_data[spec_constant_index + SPEC_IS_STATIC_BROADCAST] = 0;
    }

    vr = VK_CALL(vkCreateComputePipelines(object->device->vk_device,
            VK_NULL_HANDLE, 1, &pipeline_info, NULL,
            &program->pipelines[entry_point_index].vk_gpu_node_entry_pipeline));
    if (vr < 0)
    {
        ERR("Failed to create pipeline, vr %d\n", vr);
        return hresult_from_vk_result(vr);
    }

    /* Non-entry node */
    tmp->spec_data[spec_constant_index + SPEC_IS_ENTRY_POINT] = 0;
    tmp->spec_data[spec_constant_index + SPEC_IS_INDIRECT_BDA_STRIDE] = 0;

    if (is_broadcast)
    {
        tmp->spec_data[spec_constant_index +
                SPEC_IS_GRID_IS_UPPER_BOUND] = entry->node_input->dispatch_grid_is_upper_bound;
        tmp->spec_data[spec_constant_index + SPEC_IS_STATIC_BROADCAST] = 0;
    }

    vr = VK_CALL(vkCreateComputePipelines(object->device->vk_device,
            VK_NULL_HANDLE, 1, &pipeline_info, NULL,
            &program->pipelines[entry_point_index].vk_non_entry_pipeline));
    if (vr < 0)
    {
        ERR("Failed to create pipeline, vr %d\n", vr);
        return hresult_from_vk_result(vr);
    }

    assert(spec_constant_index + (is_broadcast ? SPEC_COUNT_BROADCAST : SPEC_COUNT_NON_BROADCAST) == tmp->spec_data_count);

    return S_OK;
}

static bool d3d12_wg_state_object_program_can_compact_broadcast_nodes(
        struct d3d12_wg_state_object *object,
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program)
{
    struct vkd3d_shader_node_input_data *node_input;
    bool can_compact = false;
    uint32_t node_index;
    uint32_t level, i;

    /* Entry points are irrelevant. */
    for (level = 1; level < program->num_levels; level++)
    {
        /* If we have sparse nodes which share inputs from a MaxBroadcastGrid node, we cannot compact,
         * since we have to know the dispatch counts early. */
        for (i = 0; i < program->levels[level].shared_nodes_count; i++)
        {
            node_index = program->levels[level].shared_nodes[i].node_payload_index;
            node_input = data->entry_points[program->slot_entry[node_index]].node_input;
            if (node_input && node_input->dispatch_grid_is_upper_bound)
                return false;
        }

        for (i = 0; i < program->levels[level].nodes_count; i++)
        {
            node_index = program->levels[level].nodes[i];
            node_input = data->entry_points[program->slot_entry[node_index]].node_input;
            if (node_input && node_input->dispatch_grid_is_upper_bound)
                can_compact = true;
        }
    }

    return can_compact;
}

static bool d3d12_wg_add_u64(uint64_t *sum, uint64_t value)
{
    if (value > UINT64_MAX - *sum)
        return false;
    *sum += value;
    return true;
}

/* amdgpu-wddm fork: W of one producer item of a slot: the most that one group (one wave of a thread launch node) can
 * write into the next pool. For each output declaration with a target that has a payload: MaxRecords x the largest
 * round16(stride) of its targets payload bytes (the allocator reserves round16(count x stride) per allocation, and
 * the worst case is one record per allocation), and MaxRecords of each other component. An empty target has no
 * payload and no packed entry, but the allocator charges 4 bytes of the payload capacity for each record, so it adds
 * 4 x MaxRecords payload bytes. The distributor counts empty records in the linear offsets of the level, so an empty
 * target also adds MaxRecords unrolled offsets. A recursive output writes into the slot of the next depth, and adds nothing if the
 * slot is at the last depth. The shader metadata has no MaxRecordsSharedWith: such an output has max_records 0, and is
 * charged the largest max_records of the slot's outputs, which is at least the shared budget. Each declaration of a
 * shared set is counted on its own, which gives a larger W, never a smaller one. Returns false if a sum does not fit in
 * 64 bits. */
static bool d3d12_wg_state_object_program_item_w(const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_state_object_data *data, unsigned int slot, uint64_t *w)
{
    const struct vkd3d_shader_library_entry_point *entry = &data->entry_points[program->slot_entry[slot]];
    const struct vkd3d_shader_node_input_data *target;
    const struct vkd3d_shader_node_output_data *output;
    uint32_t node_array_size, node_index, largest = 0;
    uint64_t records, stride, max_stride, payload;
    unsigned int i, j, c;
    bool empty;

    for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
        w[c] = 0;
    if (!entry->node_input)
        return true;

    for (i = 0; i < entry->node_outputs_count; i++)
        largest = max(largest, entry->node_outputs[i].max_records);

    for (i = 0; i < entry->node_outputs_count; i++)
    {
        output = &entry->node_outputs[i];
        if (output->node_array_size == UINT32_MAX)
            return false;
        node_array_size = output->node_array_size ? output->node_array_size : 1;

        max_stride = 0;
        empty = false;
        for (j = 0; j < node_array_size; j++)
        {
            node_index = d3d12_work_graph_find_node_by_id(data->entry_points, data->entry_points_count,
                    output->node_id, output->node_array_index + j);
            /* A missing member of a sparse array. The level builder refuses the others. */
            if (node_index == UINT32_MAX)
                continue;
            target = data->entry_points[node_index].node_input;
            if (node_index == program->slot_entry[slot] && program->slot_depth[slot] >= target->recursion_factor)
                continue;
            stride = align64(target->payload_stride, 16);
            if (stride && program->pool_test_stride)
                stride = align64(program->pool_test_stride, 16);
            if (!stride)
                empty = true;
            max_stride = max(max_stride, stride);
        }
        if (!max_stride && !empty)
            continue;

        records = output->max_records ? output->max_records : largest;
        if (program->pool_test_records)
            records = program->pool_test_records;
        payload = records * (max_stride ? max_stride : D3D12_WG_POOL_EMPTY_RECORD_BYTES);
        if (entry->node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_THREAD)
        {
            if (payload > UINT64_MAX / D3D12_WG_POOL_THREAD_ITEM)
                return false;
            payload *= D3D12_WG_POOL_THREAD_ITEM;
            records *= D3D12_WG_POOL_THREAD_ITEM;
        }
        if (!d3d12_wg_add_u64(&w[D3D12_WG_POOL_PAYLOAD], payload) ||
                !d3d12_wg_add_u64(&w[D3D12_WG_POOL_OFFSETS], records))
            return false;
        if (max_stride && !d3d12_wg_add_u64(&w[D3D12_WG_POOL_ENTRIES], records))
            return false;
    }

    return true;
}

/* amdgpu-wddm fork: places a region of the given size at the next aligned offset from *cursor, below limit. */
static bool d3d12_wg_drain_place(VkDeviceSize *cursor, VkDeviceSize alignment, VkDeviceSize size,
        VkDeviceSize limit, struct d3d12_wg_drain_region *region)
{
    VkDeviceSize offset;

    if (*cursor > limit || limit > UINT64_MAX / 2)
        return false;
    offset = align64(*cursor, alignment);
    if (offset > limit || size > limit - offset)
        return false;
    region->offset = offset;
    region->size = size;
    *cursor = offset + size;
    return true;
}

/* The bytes of units of a pool component, or false if they do not fit in 64 bits. */
static bool d3d12_wg_pool_bytes(unsigned int component, uint64_t units, VkDeviceSize *bytes)
{
    if (units > UINT64_MAX / d3d12_wg_pool_unit_bytes[component])
        return false;
    *bytes = units * d3d12_wg_pool_unit_bytes[component];
    return true;
}

/* amdgpu-wddm fork: the demand of one record of a slot of a level on the next pool. For each reader of the record (the
 * slot and the slots of the level that share its input): W of one item x the items of the record, which is the grid
 * product for a broadcasting reader (the NodeMaxDispatchGrid bound for a dynamic grid) and 1 for the others, because
 * one group can take the record. A record is admitted as a whole, so this space must be free to admit one record.
 * Returns false if a value does not fit in 64 bits. */
static bool d3d12_wg_state_object_program_record_demand(const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_state_object_data *data, unsigned int level, unsigned int slot, uint64_t *demand)
{
    const struct d3d12_workgraph_level_execution *exec = &program->levels[level];
    const struct vkd3d_shader_node_input_data *input;
    uint64_t w[D3D12_WG_POOL_COMPONENT_COUNT], items;
    unsigned int i, j, reader, c;

    for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
        demand[c] = 0;
    for (i = 0; i <= exec->shared_nodes_count; i++)
    {
        if (i == exec->shared_nodes_count)
            reader = slot;
        else if (exec->shared_nodes[i].node_payload_index == slot)
            reader = exec->shared_nodes[i].node_pipeline_index;
        else
            continue;
        if (!d3d12_wg_state_object_program_item_w(program, data, reader, w))
            return false;

        input = data->entry_points[program->slot_entry[reader]].node_input;
        items = 1;
        if (input && input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING)
        {
            for (j = 0; j < ARRAY_SIZE(input->broadcast_grid); j++)
            {
                if (input->broadcast_grid[j] && items > UINT64_MAX / input->broadcast_grid[j])
                    return false;
                items *= input->broadcast_grid[j];
            }
        }
        for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
        {
            if (items && w[c] > UINT64_MAX / items)
                return false;
            if (!d3d12_wg_add_u64(&demand[c], w[c] * items))
                return false;
        }
    }
    return true;
}

/* floor(a x b / c) for 0 < c < 2^63 and b <= c: the product is formed in 128 bits, and the quotient is at most a. */
static uint64_t d3d12_wg_mul_div(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t p0, p1, p2, p3, mid, hi, lo, q = 0, r = 0;
    int i;

    p0 = (a & 0xffffffffu) * (b & 0xffffffffu);
    p1 = (a & 0xffffffffu) * (b >> 32);
    p2 = (a >> 32) * (b & 0xffffffffu);
    p3 = (a >> 32) * (b >> 32);
    mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    lo = (mid << 32) | (p0 & 0xffffffffu);
    hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);

    /* Long division. The remainder is below c < 2^63, so the shift does not overflow; the quotient is below 2^64, so
     * the bits that the shift of q removes are 0. */
    for (i = 127; i >= 0; i--)
    {
        r = (r << 1) | ((i >= 64 ? hi >> (i - 64) : lo >> i) & 1);
        q <<= 1;
        if (r >= c)
        {
            r -= c;
            q |= 1;
        }
    }
    return q;
}

/* A payload region and its sink: the producers address both with 32-bit offsets from the start of the region. */
#define D3D12_WG_POOL_PAYLOAD_LIMIT ((VkDeviceSize)UINT32_MAX)
/* The regions that get a part of the arena: the packed entries, the unrolled offsets and two payload halves per pool. */
#define D3D12_WG_DRAIN_REGIONS (2 + 2 * (MAX_WORKGRAPH_LEVELS - 1))

/* The isolated size of each region that gets a part of the arena: index 0 the packed entries and 1 the unrolled
 * offsets, each for the largest W of the pools, then 2 + 2 x (L - 1) + half the payload of pool L, its W rounded up to
 * 16 bytes. The entry level is not throttled, so the guard of pool 1 limits its records to one for each 4 bytes of
 * the unrolled offsets (d3d12_wg_pool_capacity). The unrolled offsets are therefore at least as large as a payload
 * half of pool 1: in each layout, the guard of pool 1 then allows its full payload half. */
static bool d3d12_wg_drain_isolated_sizes(const struct d3d12_wg_state_object_program *program,
        VkDeviceSize *isolated, unsigned int *count)
{
    uint64_t entries = 0, offsets = 0;
    unsigned int level, half;

    for (level = 1; level < program->num_levels; level++)
    {
        entries = max(entries, program->pool_w[level][D3D12_WG_POOL_ENTRIES]);
        offsets = max(offsets, program->pool_w[level][D3D12_WG_POOL_OFFSETS]);
        if (program->pool_w[level][D3D12_WG_POOL_PAYLOAD] > UINT64_MAX - 15)
            return false;
    }
    if (!d3d12_wg_pool_bytes(D3D12_WG_POOL_ENTRIES, entries, &isolated[0]) ||
            !d3d12_wg_pool_bytes(D3D12_WG_POOL_OFFSETS, offsets, &isolated[1]))
        return false;
    if (program->num_levels > 1)
        isolated[1] = max(isolated[1], align64(program->pool_w[1][D3D12_WG_POOL_PAYLOAD], 16));
    *count = 2;
    for (level = 1; level < program->num_levels; level++)
    {
        for (half = 0; half < 2; half++)
            isolated[(*count)++] = align64(program->pool_w[level][D3D12_WG_POOL_PAYLOAD], 16);
    }
    return true;
}

/* Places the regions from offset 0, below limit, with the sizes of d3d12_wg_drain_isolated_sizes: the fixed
 * bookkeeping, the packed entries, the unrolled offsets, the empty-record state, and for each pool and each half the
 * payload region with its sink. The entry and offset counters are 32-bit, and a payload region and its sink stay
 * below the 32-bit limit. */
static bool d3d12_wg_drain_place_regions(const struct d3d12_wg_state_object_program *program,
        const VkDeviceSize *size, VkDeviceSize limit, VkDeviceSize *cursor, struct d3d12_wg_drain_layout *layout)
{
    unsigned int level, half, n = 2;

    *cursor = 0;
    if (size[0] > (VkDeviceSize)UINT32_MAX * d3d12_wg_pool_unit_bytes[D3D12_WG_POOL_ENTRIES] ||
            size[1] > (VkDeviceSize)UINT32_MAX * d3d12_wg_pool_unit_bytes[D3D12_WG_POOL_OFFSETS])
        return false;
    if (!d3d12_wg_drain_place(cursor, 64, program->pool_fixed_size, limit, &layout->fixed) ||
            !d3d12_wg_drain_place(cursor, d3d12_wg_pool_alignment[D3D12_WG_POOL_ENTRIES], size[0], limit,
            &layout->entries) ||
            !d3d12_wg_drain_place(cursor, d3d12_wg_pool_alignment[D3D12_WG_POOL_OFFSETS], size[1], limit,
            &layout->offsets) ||
            /* The empty-record counters are not there yet. */
            !d3d12_wg_drain_place(cursor, 16, 0, limit, &layout->empty_state))
        return false;
    for (level = 1; level < program->num_levels; level++)
    {
        for (half = 0; half < 2; half++, n++)
        {
            if (size[n] > D3D12_WG_POOL_PAYLOAD_LIMIT - program->pool_sink_size ||
                    !d3d12_wg_drain_place(cursor, d3d12_wg_pool_alignment[D3D12_WG_POOL_PAYLOAD], size[n], limit,
                    &layout->payload[level][half]) ||
                    !d3d12_wg_drain_place(cursor, 16, program->pool_sink_size, limit, &layout->sink[level][half]))
                return false;
        }
    }
    return true;
}

/* Checks that a region is aligned, starts at or after *end and is inside the backing size, then moves *end. */
static bool d3d12_wg_drain_check_region(const struct d3d12_wg_drain_region *region, VkDeviceSize alignment,
        VkDeviceSize backing_size, VkDeviceSize *end)
{
    if (region->offset % alignment || region->offset < *end || region->offset > backing_size ||
            region->size > backing_size - region->offset)
    {
        ERR("Work graph pool layout: the region at %#"PRIx64" (%#"PRIx64" bytes) is not valid.\n",
                region->offset, region->size);
        return false;
    }
    *end = region->offset + region->size;
    return true;
}

/* Checks that the regions of a layout are aligned, in order without overlap, and inside the backing size; that the
 * packed entries start directly after the output guard, the last 32 bytes of the fixed bookkeeping; and that each sink
 * directly follows its payload region, below the 32-bit limit. */
static bool d3d12_wg_drain_layout_check(const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_drain_layout *layout)
{
    VkDeviceSize end = 0, size = layout->backing_size;
    const struct d3d12_wg_drain_region *payload, *sink;
    unsigned int level, half;

    if (!d3d12_wg_drain_check_region(&layout->fixed, 64, size, &end) ||
            !d3d12_wg_drain_check_region(&layout->entries, d3d12_wg_pool_alignment[D3D12_WG_POOL_ENTRIES], size, &end) ||
            !d3d12_wg_drain_check_region(&layout->offsets, d3d12_wg_pool_alignment[D3D12_WG_POOL_OFFSETS], size, &end) ||
            !d3d12_wg_drain_check_region(&layout->empty_state, 16, size, &end))
        return false;
    if (layout->fixed.size < sizeof(struct vkd3d_workgraph_output_guard) ||
            layout->entries.offset != layout->fixed.offset + layout->fixed.size)
    {
        ERR("Work graph pool layout: the packed entries at %#"PRIx64" do not follow the output guard.\n",
                layout->entries.offset);
        return false;
    }
    for (level = 1; level < program->num_levels; level++)
    {
        for (half = 0; half < 2; half++)
        {
            payload = &layout->payload[level][half];
            sink = &layout->sink[level][half];
            if (!d3d12_wg_drain_check_region(payload, d3d12_wg_pool_alignment[D3D12_WG_POOL_PAYLOAD], size, &end) ||
                    !d3d12_wg_drain_check_region(sink, 16, size, &end))
                return false;
            if (sink->offset != payload->offset + payload->size ||
                    payload->size + sink->size > D3D12_WG_POOL_PAYLOAD_LIMIT)
            {
                ERR("Work graph pool layout: the sink of pool %u half %u is not valid.\n", level, half);
                return false;
            }
        }
    }
    return d3d12_wg_drain_check_region(&layout->staging, 16, size, &end);
}

/* amdgpu-wddm fork: the layout of the record pools for backing_size bytes of backing memory. From offset 0: the
 * regions of d3d12_wg_drain_place_regions, then at the end the multi GPU input staging range. The bytes between
 * them (the arena) go to the packed entries, the unrolled offsets and the payload regions in proportion to their
 * isolated sizes, so that each region holds about the same number of records. The progress of the scheduler depends
 * only on the isolated sizes. Above pool_max_size no region grows. Returns false if there is no layout for this size
 * (below pool_min_size, or the pools are not valid). */
static bool d3d12_wg_drain_layout(const struct d3d12_wg_state_object_program *program, VkDeviceSize backing_size,
        struct d3d12_wg_drain_layout *layout)
{
    VkDeviceSize isolated[D3D12_WG_DRAIN_REGIONS], size[D3D12_WG_DRAIN_REGIONS];
    VkDeviceSize used, limit, cursor, arena, total;
    unsigned int count, n;

    memset(layout, 0, sizeof(*layout));
    if (!program->pool_valid || backing_size < program->pool_min_size)
        return false;
    used = min(backing_size, program->pool_max_size);
    if (used < program->pool_staging_size)
        return false;
    limit = (used - program->pool_staging_size) & ~(VkDeviceSize)15;
    if (!d3d12_wg_drain_isolated_sizes(program, isolated, &count) ||
            !d3d12_wg_drain_place_regions(program, isolated, limit, &cursor, layout))
        return false;

    /* The regions are inside limit, so their sum does not overflow. 16 bytes per region are kept for alignment. Each
     * part is rounded down to 16 bytes, and stays below the 32-bit limit of its region. */
    total = 0;
    for (n = 0; n < count; n++)
        total += isolated[n];
    arena = limit - cursor;
    arena = arena > 16 * count ? arena - 16 * count : 0;
    for (n = 0; n < count; n++)
    {
        size[n] = isolated[n];
        if (total)
            size[n] += d3d12_wg_mul_div(arena, isolated[n], total) & ~(VkDeviceSize)15;
    }
    size[0] = min(size[0], ((VkDeviceSize)UINT32_MAX * d3d12_wg_pool_unit_bytes[D3D12_WG_POOL_ENTRIES]));
    size[1] = min(size[1], ((VkDeviceSize)UINT32_MAX * d3d12_wg_pool_unit_bytes[D3D12_WG_POOL_OFFSETS]));
    for (n = 2; n < count; n++)
        size[n] = min(size[n], (D3D12_WG_POOL_PAYLOAD_LIMIT - program->pool_sink_size) & ~(VkDeviceSize)15);
    if (!d3d12_wg_drain_place_regions(program, size, limit, &cursor, layout))
        return false;

    layout->staging.offset = limit;
    layout->staging.size = program->pool_staging_size;
    layout->backing_size = used;
    return d3d12_wg_drain_layout_check(program, layout);
}

/* amdgpu-wddm fork: W and the record bound of the pools, Min and Max of the pool layout. The state object does not
 * depend on them: if the metadata gives no bound, the pools are marked as not valid. */
static void d3d12_wg_state_object_program_compute_pools(struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_state_object_data *data)
{
    uint64_t w[D3D12_WG_POOL_COMPONENT_COUNT], demand[D3D12_WG_POOL_COMPONENT_COUNT];
    VkDeviceSize isolated[D3D12_WG_DRAIN_REGIONS], cursor, size;
    const struct d3d12_workgraph_level_execution *exec;
    struct d3d12_wg_drain_layout layout;
    unsigned int level, i, slot, c, count;

    memset(program->pool_w, 0, sizeof(program->pool_w));
    memset(program->pool_bound, 0, sizeof(program->pool_bound));
    program->pool_valid = false;

    if (program->pool_ignored_overrides)
    {
        WARN("Work graph pools: a node override that the engine ignores is set.\n");
        return;
    }

    for (level = 0; level < program->num_levels; level++)
    {
        exec = &program->levels[level];
        for (i = 0; i < exec->nodes_count + exec->shared_nodes_count; i++)
        {
            slot = i < exec->nodes_count ? exec->nodes[i] : exec->shared_nodes[i - exec->nodes_count].node_pipeline_index;
            if (!d3d12_wg_state_object_program_item_w(program, data, slot, w))
            {
                WARN("Work graph pools: the output of slot %u has no 64-bit bound.\n", slot);
                return;
            }
            /* The level builder puts every target of level L into level L + 1. */
            if (level + 1 == program->num_levels)
            {
                for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
                {
                    if (w[c])
                    {
                        WARN("Work graph pools: slot %u of the last level %u has outputs.\n", slot, level);
                        return;
                    }
                }
                continue;
            }
            for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
                program->pool_w[level + 1][c] = max(program->pool_w[level + 1][c], w[c]);
        }

        /* The records of the level: those of a slot and of the slots that share its input are one record. */
        for (i = 0; i < exec->nodes_count && level + 1 < program->num_levels; i++)
        {
            if (!d3d12_wg_state_object_program_record_demand(program, data, level, exec->nodes[i], demand))
            {
                WARN("Work graph pools: a record of slot %u has no 64-bit bound.\n", exec->nodes[i]);
                return;
            }
            for (c = 0; c < D3D12_WG_POOL_COMPONENT_COUNT; c++)
                program->pool_bound[level + 1][c] = max(program->pool_bound[level + 1][c], demand[c]);
        }
    }

    /* Min: no arena. The regions before the staging range end at a multiple of 16 bytes. */
    if (!d3d12_wg_drain_isolated_sizes(program, isolated, &count) ||
            !d3d12_wg_drain_place_regions(program, isolated, UINT64_MAX / 4, &cursor, &layout))
    {
        WARN("Work graph pools: the isolated regions do not fit their limits.\n");
        return;
    }
    size = align64(cursor, 16) + program->pool_staging_size;
    if (size > UINT64_MAX / 4)
        return;
    program->pool_min_size = align64(size, VKD3D_WORKGRAPH_BACKING_GRANULARITY);
    program->pool_max_size = max(program->max_backing_size, program->pool_min_size);
    program->pool_valid = true;

    /* Self-check: the layouts at Min and at Max exist and pass the region checks. */
    if (!d3d12_wg_drain_layout(program, program->pool_min_size, &layout) ||
            !d3d12_wg_drain_layout(program, program->pool_max_size, &layout))
    {
        WARN("Work graph pools: no valid layout at Min %#"PRIx64" or Max %#"PRIx64".\n",
                program->pool_min_size, program->pool_max_size);
        program->pool_valid = false;
        return;
    }

    TRACE("Work graph pools: %u levels, Min %#"PRIx64", Max %#"PRIx64".\n",
            program->num_levels, program->pool_min_size, program->pool_max_size);
}

/* amdgpu-wddm fork: the required scratch memory of a program, each range 64-byte aligned, the output guard last, so
 * that it ends where the packed offsets start. With rings, the payload rings and the unrolled offsets are in it (the
 * layout of today). Without rings, it is the fixed bookkeeping of the pool layout, and the pools, the packed entries
 * and the unrolled offsets follow it (d3d12_wg_drain_layout). Returns the size. */
static VkDeviceSize d3d12_wg_state_object_program_layout_scratch(struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_multi_input_config *config, bool rings)
{
    VkDeviceSize canary_size = config->canaries ? VKD3D_WORKGRAPH_CANARY_SIZE : 0;
    VkDeviceSize scratch_offset = 0;

#define alloc_scratch(member, size) \
    scratch_offset = align64(scratch_offset, 64); \
    program->member##_offset = scratch_offset; \
    program->member##_size = (size); \
    scratch_offset += program->member##_size
#define no_scratch(member) \
    program->member##_offset = 0; \
    program->member##_size = 0

    alloc_scratch(counters_scratch, (2 + program->num_pipelines) * sizeof(uint32_t));
    alloc_scratch(indirect_commands_scratch_base,
            program->num_pipelines * sizeof(struct d3d12_workgraph_indirect_command) + WG_INDIRECT_COMMAND_OFFSET);
    alloc_scratch(dividers_scratch, program->num_pipelines * sizeof(uint32_t));
    alloc_scratch(share_mapping_scratch, program->num_pipelines * sizeof(uint32_t));
    alloc_scratch(payload_expander_scratch, program->num_pipelines * sizeof(struct d3d12_wg_payload_expander_meta));
    if (rings)
    {
        /* A canary range follows each sink and the unrolled offsets directly, when the lab switch asks for it. */
        alloc_scratch(payload0_scratch, config->ring_size + VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE + canary_size);
        alloc_scratch(payload1_scratch, config->ring_size + VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE + canary_size);
        /* One offset of 4 bytes per record, and a record takes at least 4 bytes of the payload capacity. */
        alloc_scratch(unrolled_offsets_scratch, config->ring_size + canary_size);
    }
    else
    {
        no_scratch(payload0_scratch);
        no_scratch(payload1_scratch);
        no_scratch(unrolled_offsets_scratch);
    }
    if (program->drain_rounds)
    {
        alloc_scratch(drain_control_scratch, sizeof(struct vkd3d_workgraph_drain_control));
        alloc_scratch(pool_state_scratch, (VkDeviceSize)program->num_levels * program->pool_state_stride);
        alloc_scratch(pool_admit_scratch, program->num_pipelines * sizeof(uint32_t));
    }
    alloc_scratch(output_guard_scratch, sizeof(struct vkd3d_workgraph_output_guard));
#undef no_scratch
#undef alloc_scratch

    return scratch_offset;
}

/* amdgpu-wddm fork: the lab drain mode runs on the record pools if they are valid. The admission of each slot that
 * reads records of a level with outputs: u is the smallest number of records that one group of a reader takes (1 for
 * a broadcasting reader, the coalesce factor, 32 for a thread launch reader), and the demand is the demand of one
 * record on the next pool, all its readers and groups (d3d12_wg_state_object_program_record_demand). Admitting k
 * records charges ceil(k / u) x demand, which is at least the sum of ceil(k / u_r) x demand_r over the readers. The
 * other slots admit every record. The retention pass copies whole words, so each payload stride must be a multiple of
 * 4 bytes. If a value does not fit, the lab drain mode keeps the payload rings. */
static void d3d12_wg_state_object_program_init_pool_execution(struct d3d12_wg_state_object *object,
        const struct d3d12_wg_state_object_data *data, struct d3d12_wg_state_object_program *program)
{
    const struct d3d12_workgraph_level_execution *exec;
    const struct vkd3d_shader_node_input_data *input;
    struct d3d12_wg_multi_input_config config;
    struct vkd3d_workgraph_pool_admission *admission;
    unsigned int level, i, j, slot, reader, u;

    program->pool_execution = false;
    if (!program->drain_rounds || !program->pool_valid)
        return;
    /* The test hook of pool_records replaces the bounds of the metadata, so the pools do not hold the records. */
    d3d12_wg_get_multi_input_config(&config);
    if (!config.pools || program->pool_test_records)
    {
        WARN("Work graph pools: the extra rounds run on the payload rings (pools %u, pool records %u).\n",
                config.pools, program->pool_test_records);
        return;
    }

    for (i = 0; i < program->num_pipelines; i++)
    {
        input = data->entry_points[program->slot_entry[i]].node_input;
        if (input && (input->payload_stride & 3))
        {
            WARN("Work graph pools: slot %u has a payload stride of %u bytes, keeping the payload rings.\n",
                    i, input->payload_stride);
            return;
        }
    }

    if (!(admission = vkd3d_calloc(program->num_pipelines, sizeof(*admission))))
        return;
    for (i = 0; i < program->num_pipelines; i++)
        admission[i].u = 1;

    for (level = 1; level + 1 < program->num_levels; level++)
    {
        exec = &program->levels[level];
        for (i = 0; i < exec->nodes_count; i++)
        {
            slot = exec->nodes[i];
            if (!d3d12_wg_state_object_program_record_demand(program, data, level, slot, admission[slot].demand))
            {
                WARN("Work graph pools: the demand of slot %u does not fit in 64 bits, keeping the payload rings.\n",
                        slot);
                vkd3d_free(admission);
                return;
            }

            u = UINT32_MAX;
            for (j = 0; j <= exec->shared_nodes_count; j++)
            {
                if (j == exec->shared_nodes_count)
                    reader = slot;
                else if (exec->shared_nodes[j].node_payload_index == slot)
                    reader = exec->shared_nodes[j].node_pipeline_index;
                else
                    continue;
                input = data->entry_points[program->slot_entry[reader]].node_input;
                if (input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_COALESCING)
                    u = min(u, input->coalesce_factor);
                else if (input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_THREAD)
                    u = min(u, THREAD_COALESCE_COUNT);
                else
                    u = min(u, 1);
            }
            if (!u)
            {
                WARN("Work graph pools: slot %u has a coalesce factor of 0, keeping the payload rings.\n", slot);
                vkd3d_free(admission);
                return;
            }
            admission[slot].u = u;
            TRACE("Work graph pools: slot %u of level %u, u %u, demand %"PRIu64", %"PRIu64", %"PRIu64".\n", slot, level, u,
                    admission[slot].demand[0], admission[slot].demand[1], admission[slot].demand[2]);
        }
    }

    /* The fixed bookkeeping is the required scratch memory, so the output guard ends where the packed entries of the
     * pool layout start. The canaries are for the payload rings. */
    program->required_scratch_size = d3d12_wg_state_object_program_layout_scratch(program, &config, false);
    if (program->required_scratch_size != program->pool_fixed_size)
    {
        ERR("Work graph pools: the fixed bookkeeping is %#"PRIx64" bytes, expected %#"PRIx64".\n",
                program->required_scratch_size, program->pool_fixed_size);
        program->required_scratch_size = d3d12_wg_state_object_program_layout_scratch(program, &config, true);
        vkd3d_free(admission);
        return;
    }
    program->indirect_commands_scratch_node_offset =
            program->indirect_commands_scratch_base_offset + WG_INDIRECT_COMMAND_OFFSET;
    program->canary_count = 0;
    program->min_backing_size = program->pool_min_size;
    program->max_backing_size = program->pool_max_size;
    program->pool_admission = admission;
    vkd3d_meta_get_workgraph_pool_pipeline(&object->device->meta_ops, &program->pool_pass);
    program->pool_execution = true;
    TRACE("Work graph pools: execution on the pools, Min %#"PRIx64", Max %#"PRIx64".\n",
            program->pool_min_size, program->pool_max_size);
}

static HRESULT d3d12_wg_state_object_compile_program(
        struct d3d12_wg_state_object *object,
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_program *program)
{
    struct d3d12_wg_multi_input_config multi_input_config;
    struct d3d12_wg_state_object_spec_constant_tmp tmp;
    VkDeviceSize scratch_offset = 0;
    VkDeviceSize canary_size;
    unsigned int level;
    HRESULT hr = S_OK;
    unsigned int i;

    memset(&tmp, 0, sizeof(tmp));
    program->pipelines = vkd3d_calloc(program->num_pipelines, sizeof(*program->pipelines));
    program->coalesce_dividers_or_amp = vkd3d_calloc(program->num_pipelines, sizeof(int32_t));
    program->share_mapping = vkd3d_malloc(program->num_pipelines * sizeof(uint32_t));
    program->payload_expander_meta = vkd3d_calloc(program->num_pipelines, sizeof(struct d3d12_wg_payload_expander_meta));
    if (d3d12_wg_fault_injected("program", UINT_MAX))
    {
        vkd3d_free(program->share_mapping);
        program->share_mapping = NULL;
    }
    if (!program->pipelines || !program->coalesce_dividers_or_amp ||
            !program->share_mapping || !program->payload_expander_meta)
        return E_OUTOFMEMORY;

    for (i = 0; i < program->num_pipelines; i++)
        program->share_mapping[i] = UINT32_MAX;

    d3d12_wg_get_multi_input_config(&multi_input_config);
    canary_size = multi_input_config.canaries ? VKD3D_WORKGRAPH_CANARY_SIZE : 0;
    /* The extra rounds are conditional rendering blocks. Without conditional rendering there is only round 1. */
    program->drain_rounds = multi_input_config.drain_rounds;
    program->drain_pending = multi_input_config.drain_pending;
    if (program->drain_rounds && !object->device->device_info.conditional_rendering_features.conditionalRendering)
    {
        WARN("No conditional rendering, ignoring %u extra rounds.\n", program->drain_rounds);
        program->drain_rounds = 0;
    }
    if (program->drain_rounds)
        vkd3d_meta_get_workgraph_drain_pipeline(&object->device->meta_ops, &program->drain);

    /* amdgpu-wddm fork: compaction counts the groups of every record of a level, also of the records that the lab
     * drain mode does not admit, so the drain mode does not compact. */
    program->compact_broadcast_nodes_with_max_grid = !program->drain_rounds &&
            d3d12_wg_state_object_program_can_compact_broadcast_nodes(object, data, program);

    for (i = 0; i < program->num_pipelines; i++)
    {
        const struct vkd3d_shader_node_input_data *node_input = data->entry_points[program->slot_entry[i]].node_input;
        struct d3d12_wg_payload_expander_meta *meta;
        uint32_t node_index;

        if (!node_input)
            continue;

        switch (node_input->launch_type)
        {
            case VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING:
            {
                uint32_t max_amplification = node_input->broadcast_grid[0] * node_input->broadcast_grid[1] * node_input->broadcast_grid[2];
                max_amplification = min(max_amplification, MAX_AMPLIFICATION_RATE);
                /* Mark high bit to make sure we're not compacting. */
                if (!node_input->dispatch_grid_is_upper_bound)
                    max_amplification |= 1u << 30;
                program->coalesce_dividers_or_amp[i] = -(int)max_amplification;
                break;
            }

            case VKD3D_SHADER_NODE_LAUNCH_TYPE_THREAD:
                program->coalesce_dividers_or_amp[i] = THREAD_COALESCE_COUNT;
                break;

            case VKD3D_SHADER_NODE_LAUNCH_TYPE_COALESCING:
                program->coalesce_dividers_or_amp[i] = (int)node_input->coalesce_factor;
                break;

            default:
                return E_INVALIDARG;
        }

        meta = &program->payload_expander_meta[i];
        meta->payload_stride = node_input->payload_stride;

        if (node_input->dispatch_grid_is_upper_bound)
        {
            meta->grid_offset_or_count = (int)node_input->dispatch_grid_offset;
        }
        else
        {
            uint32_t total_wgs = node_input->broadcast_grid[0] *
                    node_input->broadcast_grid[1] *
                    node_input->broadcast_grid[2];
            meta->grid_offset_or_count = -(int)total_wgs;
        }

        if (node_input->node_track_rw_input_sharing ||
                (node_input->dispatch_grid_is_upper_bound && program->compact_broadcast_nodes_with_max_grid))
        {
            meta->group_components_u32 = node_input->dispatch_grid_type_bits == 32;
            meta->group_components = node_input->dispatch_grid_components;
        }

        meta->rw_group_tracking = node_input->node_track_rw_input_sharing;
        meta->group_compact_broadcast = node_input->dispatch_grid_is_upper_bound &&
                program->compact_broadcast_nodes_with_max_grid;

        if (i < program->num_nodes && node_input->node_share_input_id && *node_input->node_share_input_id != '\0')
        {
            node_index = d3d12_work_graph_find_node_by_id(
                    data->entry_points, data->entry_points_count,
                    node_input->node_share_input_id, node_input->node_share_input_array_index);

            /* If we don't find the node, assume there is no sharing to care about anyway, so *shrug*. */
            if (node_index != UINT32_MAX)
                program->share_mapping[i] = node_index;
        }
    }

    vkd3d_meta_get_workgraph_workgroup_pipeline(&object->device->meta_ops,
            &program->workgroup_distributor, program->compact_broadcast_nodes_with_max_grid);
    vkd3d_meta_get_workgraph_setup_gpu_input_pipeline(&object->device->meta_ops,
            &program->gpu_input_setup);
    vkd3d_meta_get_workgraph_payload_offset_pipeline(&object->device->meta_ops,
            &program->payload_offset_expander);
    vkd3d_meta_get_workgraph_complete_compaction_pipeline(&object->device->meta_ops,
            &program->complete_compaction);
    vkd3d_meta_get_workgraph_multi_gpu_input_pipelines(&object->device->meta_ops,
            &program->multi_gpu_input_prepare, &program->multi_gpu_input_copy);

    program->payload_ring_size = multi_input_config.ring_size;
    program->packed_limit = multi_input_config.packed_limit;
    program->pool_state_stride = align(16 + 8 * program->num_pipelines, 16);

    /* amdgpu-wddm fork: the fixed bookkeeping of the pool layout is the required scratch memory without the payload
     * rings and the unrolled offsets. The ring layout comes after it, so that its offsets are the ones in use. */
    program->pool_fixed_size = d3d12_wg_state_object_program_layout_scratch(program, &multi_input_config, false);
    scratch_offset = d3d12_wg_state_object_program_layout_scratch(program, &multi_input_config, true);
    program->canary_count = 0;
    if (canary_size)
    {
        program->canary_offsets[0] = program->payload0_scratch_offset + program->payload0_scratch_size - canary_size;
        program->canary_offsets[1] = program->payload1_scratch_offset + program->payload1_scratch_size - canary_size;
        program->canary_offsets[2] = program->unrolled_offsets_scratch_offset +
                program->unrolled_offsets_scratch_size - canary_size;
        program->canary_count = VKD3D_WORKGRAPH_CANARY_COUNT;
    }

    program->multi_input_large_size = multi_input_config.large_size;
    program->multi_input_groups = multi_input_config.groups;

    program->required_scratch_size = scratch_offset;

    /* amdgpu-wddm fork: after the required scratch memory come the packed offsets, then the staging range of the
     * multi GPU input at the end. */
    program->multi_input_staging_min_size = multi_input_config.staging_size;
    program->multi_input_staging_max_size = multi_input_config.staging_max_size;
    program->min_backing_size = align64(program->required_scratch_size + VKD3D_WORKGRAPH_PACKED_OFFSETS_MIN_SIZE +
            program->multi_input_staging_min_size, VKD3D_WORKGRAPH_BACKING_GRANULARITY);
    program->max_backing_size = align64(program->required_scratch_size + VKD3D_WORKGRAPH_PACKED_OFFSETS_MAX_SIZE +
            program->multi_input_staging_max_size, VKD3D_WORKGRAPH_BACKING_GRANULARITY);
    program->indirect_commands_scratch_node_offset =
            program->indirect_commands_scratch_base_offset + WG_INDIRECT_COMMAND_OFFSET;

    program->pool_staging_size = program->multi_input_staging_min_size;
    program->pool_sink_size = VKD3D_WORKGRAPH_PAYLOAD_SINK_SIZE + canary_size;
    program->pool_test_records = multi_input_config.pool_records;
    program->pool_test_stride = multi_input_config.pool_stride;
    d3d12_wg_state_object_program_compute_pools(program, data);
    d3d12_wg_state_object_program_init_pool_execution(object, data, program);

    for (level = 0; level < program->num_levels; level++)
    {
        const struct d3d12_workgraph_level_execution *exec = &program->levels[level];
        for (i = 0; i < exec->nodes_count; i++)
        {
            if (d3d12_wg_fault_injected("pipeline", exec->nodes[i]))
            {
                hr = E_OUTOFMEMORY;
                goto fail;
            }
            if (FAILED(hr = d3d12_wg_state_object_compile_pipeline(object, data, program, exec->nodes[i], &tmp)))
                goto fail;
        }

        for (i = 0; i < exec->shared_nodes_count; i++)
            if (FAILED(hr = d3d12_wg_state_object_compile_pipeline(object, data, program, exec->shared_nodes[i].node_pipeline_index, &tmp)))
                goto fail;
    }

fail:
    vkd3d_free(tmp.map_entries);
    vkd3d_free(tmp.spec_data);
    return hr;
}

static HRESULT d3d12_wg_state_object_convert_entry_point(
        struct d3d12_wg_state_object *object,
        struct d3d12_wg_state_object_data *data,
        struct d3d12_wg_state_object_module *module,
        struct vkd3d_shader_library_entry_point *entry)
{
    struct vkd3d_nv_shader_extn nv_shader_extn = d3d12_device_get_nv_shader_extn(object->device);
    struct vkd3d_shader_interface_local_info shader_interface_local_info;
    const struct d3d12_state_object_association *global_rs_assoc;
    const struct d3d12_state_object_association *local_rs_assoc;
    struct vkd3d_shader_interface_info shader_interface_info;
    struct vkd3d_shader_descriptor_binding push_ubo_binding;
    struct vkd3d_shader_compile_arguments compile_args;
    struct vkd3d_shader_code dxil, spirv;

    HRESULT hr;

    memset(&compile_args, 0, sizeof(compile_args));
    compile_args.target_extensions = object->device->vk_info.shader_extensions;
    compile_args.target_extension_count = object->device->vk_info.shader_extension_count;
    compile_args.target = VKD3D_SHADER_TARGET_SPIRV_VULKAN_1_0;
    compile_args.min_subgroup_size = object->device->d3d12_caps.options1.WaveLaneCountMin;
    compile_args.max_subgroup_size = object->device->d3d12_caps.options1.WaveLaneCountMax;
    /* Don't care about wave size promotion in RT. */
    compile_args.quirks = &object->device->workarounds.quirks;
    compile_args.nv_shader_extn_uav_slot = nv_shader_extn.uav_slot;
    compile_args.nv_shader_extn_uav_space = nv_shader_extn.uav_space;

    if (VKD3D_CONFIG_FLAG_IS_SET(DRIVER_VERSION_SENSITIVE_SHADERS))
    {
        compile_args.driver_id = object->device->device_info.vulkan_1_2_properties.driverID;
        compile_args.driver_version = object->device->device_info.properties2.properties.driverVersion;
    }

    memset(&shader_interface_info, 0, sizeof(shader_interface_info));
    memset(&shader_interface_local_info, 0, sizeof(shader_interface_local_info));

    shader_interface_info.min_ssbo_alignment = d3d12_device_get_ssbo_alignment(object->device);
    shader_interface_info.ssbo_robustness_size_alignment =
            object->device->device_info.robustness2_properties.robustStorageBufferAccessSizeAlignment;
    shader_interface_info.ssbo_exact_size_metadata_offset =
            (object->device->bindless_state.flags & VKD3D_BINDLESS_MUTABLE_EMBEDDED_PACKED_METADATA) ?
            object->device->bindless_state.packed_metadata_offset +
            offsetof(struct vkd3d_descriptor_metadata_view, info.buffer.range) : 0;
    vkd3d_bindless_state_find_binding(&object->device->bindless_state,
            VKD3D_BINDLESS_SET_EXTRA_RAW_VA_AUX_BUFFER, &shader_interface_info.exact_size_metadata_binding);

    shader_interface_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shader_interface_info.xfb_info = NULL;

    shader_interface_info.descriptor_size_cbv_srv_uav = d3d12_device_get_descriptor_handle_increment_size(
            object->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    shader_interface_info.descriptor_size_sampler = d3d12_device_get_descriptor_handle_increment_size(
            object->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    global_rs_assoc = d3d12_state_object_find_association(VKD3D_SHADER_SUBOBJECT_KIND_GLOBAL_ROOT_SIGNATURE,
            data->associations, data->associations_count,
            NULL, 0, entry, NULL);

    local_rs_assoc = d3d12_state_object_find_association(VKD3D_SHADER_SUBOBJECT_KIND_LOCAL_ROOT_SIGNATURE,
            data->associations, data->associations_count,
            NULL, 0, entry, NULL);

    /* Since we're dispatching shaders one by one, we can be very lenient with pipeline layout compat.
     * Just rebind descriptors for each dispatch. It should be fine, at least for now. */

    if (!global_rs_assoc)
    {
        struct d3d12_root_signature *global_root_signature;
        if (FAILED(hr = d3d12_root_signature_create_empty(object->device, &global_root_signature)))
            return E_OUTOFMEMORY;
        d3d12_root_signature_inc_ref(global_root_signature);
        ID3D12RootSignature_Release(&global_root_signature->ID3D12RootSignature_iface);
        module->root_signature = global_root_signature;
    }
    else
    {
        d3d12_root_signature_inc_ref(module->root_signature = global_rs_assoc->root_signature);
    }

    if (module->root_signature)
    {
        struct d3d12_root_signature *rs = module->root_signature;
        /* We might have different bindings per PSO, even if they are considered pipeline layout compatible.
         * Register/space declaration could differ, but those don't change the Vulkan pipeline layout. */
        shader_interface_info.flags = d3d12_root_signature_get_shader_interface_flags(rs, VKD3D_PIPELINE_TYPE_COMPUTE);
        shader_interface_info.descriptor_tables.offset = rs->descriptor_table_offset;
        shader_interface_info.descriptor_tables.count = rs->descriptor_table_count;
        shader_interface_info.bindings = rs->bindings;
        shader_interface_info.binding_count = rs->binding_count;
        shader_interface_info.push_constant_buffers = rs->root_constants;
        shader_interface_info.push_constant_buffer_count = rs->root_constant_count;
        /* TODO: EXTENDED_DEBUG_UTILS mapping. */
        shader_interface_info.push_constant_ubo_binding = &rs->push_constant_ubo_binding;
        shader_interface_info.offset_buffer_binding = &rs->offset_buffer_binding;
#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
        shader_interface_info.descriptor_qa_payload_binding = &rs->descriptor_qa_payload_binding;
        shader_interface_info.descriptor_qa_control_binding = &rs->descriptor_qa_control_binding;
#endif

        /* We map the root parameters as a PUSH_ADDRESS_EXT. */
        push_ubo_binding.set = VKD3D_SHADER_ROOT_CONSTANTS_VIRTUAL_DESCRIPTOR_SET;
        push_ubo_binding.binding = 0;
        shader_interface_info.push_constant_ubo_binding = &push_ubo_binding;
        shader_interface_info.flags |= VKD3D_ROOT_SIGNATURE_USE_PUSH_CONSTANT_UNIFORM_BLOCK;
    }

    if (local_rs_assoc)
    {
        struct d3d12_root_signature *rs = local_rs_assoc->root_signature;
        shader_interface_local_info.local_root_parameters = rs->parameters;
        shader_interface_local_info.local_root_parameter_count = rs->parameter_count;
        shader_interface_local_info.shader_record_constant_buffers = rs->root_constants;
        shader_interface_local_info.shader_record_buffer_count = rs->root_constant_count;
        shader_interface_local_info.bindings = rs->bindings;
        shader_interface_local_info.binding_count = rs->binding_count;

        /* Promote state which might only be active in local root signature. */
        shader_interface_info.flags |= d3d12_root_signature_get_shader_interface_flags(rs, VKD3D_PIPELINE_TYPE_COMPUTE);
    }

    /* Just fallback to normal heap descriptor if needed. */
    shader_interface_info.flags &= ~VKD3D_SHADER_INTERFACE_INLINE_REDZONE_CBV;

    if (module->root_signature)
    {
        module->mappings = d3d12_state_object_build_workgraph_root_signature_mappings(module->root_signature,
            local_rs_assoc ? local_rs_assoc->root_signature : NULL);
    }
    else if (local_rs_assoc)
    {
        module->mappings = d3d12_state_object_build_workgraph_root_signature_mappings(NULL,
            local_rs_assoc->root_signature);
    }
    else
    {
        module->mappings = NULL;
    }

    memset(&dxil, 0, sizeof(dxil));
    memset(&spirv, 0, sizeof(spirv));

    /* TODO: If we're exporting multiple entry points from one DXIL library,
     * we can amortize the parsing cost. */
    dxil.code = data->dxil_libraries[entry->identifier]->DXILLibrary.pShaderBytecode;
    dxil.size = data->dxil_libraries[entry->identifier]->DXILLibrary.BytecodeLength;

    if (vkd3d_shader_compile_dxil_export(&dxil, entry->real_entry_point, entry->debug_entry_point,
            &spirv, NULL,
            &shader_interface_info, &shader_interface_local_info, &compile_args) != VKD3D_OK)
    {
        ERR("Failed to convert DXIL export: %s (%s)\n",
                entry->real_entry_point, entry->debug_entry_point);
        return E_OUTOFMEMORY;
    }

    if (!d3d12_device_validate_shader_meta(object->device, &spirv.meta))
        return E_INVALIDARG;

    if (FAILED(hr = d3d12_pipeline_state_create_shader_module(object->device, &module->vk_module,
            &spirv, VK_SHADER_STAGE_COMPUTE_BIT)))
        return hr;

    return S_OK;
}

/* amdgpu-wddm fork: the host-visible struct vkd3d_workgraph_lost_records of the state object, cleared. */
static HRESULT d3d12_wg_state_object_allocate_lost_records(struct d3d12_wg_state_object *object)
{
    const struct vkd3d_vk_device_procs *vk_procs = &object->device->vk_procs;
    struct d3d12_wg_state_object_ring *ring = &object->lost_records;
    void *ptr;
    HRESULT hr;
    VkResult vr;

    /* struct vkd3d_workgraph_abandoned follows struct vkd3d_workgraph_lost_records. */
    if (FAILED(hr = vkd3d_create_buffer_explicit_usage(object->device,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VKD3D_WORKGRAPH_ABANDONED_OFFSET + sizeof(struct vkd3d_workgraph_abandoned), "workgraph-lost-records",
            &ring->vk_buffer)))
        return hr;

    if (FAILED(hr = vkd3d_allocate_internal_buffer_memory(object->device, ring->vk_buffer,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &ring->allocation)))
        return hr;

    if ((vr = VK_CALL(vkMapMemory(object->device->vk_device, ring->allocation.vk_memory, 0, VK_WHOLE_SIZE, 0, &ptr))))
        return hresult_from_vk_result(vr);

    object->lost_records_host = ptr;
    object->abandoned_host = (void *)((uint8_t *)ptr + VKD3D_WORKGRAPH_ABANDONED_OFFSET);
    memset(ptr, 0, VKD3D_WORKGRAPH_ABANDONED_OFFSET + sizeof(struct vkd3d_workgraph_abandoned));
    ring->va = vkd3d_get_buffer_device_address(object->device, ring->vk_buffer);
    object->abandoned_va = ring->va + VKD3D_WORKGRAPH_ABANDONED_OFFSET;
    return S_OK;
}

static HRESULT d3d12_wg_state_object_compile_programs(
        struct d3d12_wg_state_object *object, struct d3d12_wg_state_object_data *data)
{
    HRESULT hr;
    size_t i;

    object->programs = data->programs;
    object->programs_count = data->programs_count;
    data->programs = NULL;
    data->programs_count = 0;

    /* Build up a tree of executions. Level 0 are entry points which may receive work.
     * First we run all nodes in level 0, then we figure out how to distribute work, run every node in level 1,
     * etc, etc. */

    if (FAILED(hr = d3d12_wg_state_object_rearrange_entry_points(data)))
        return hr;

    for (i = 0; i < object->programs_count; i++)
    {
        struct d3d12_wg_state_object_program *program = &object->programs[i];

        /* TODO: Potential hazard is that each program can have different overrides per node,
         * meaning we need a local copy of entry point meta. However, ignore this for now. */
        if (object->programs_count > 1 && program->explicit_defined_nodes_count)
        {
            FIXME("More than one program is used and explicitly defined nodes is used. "
                  "This may not work if there are conflicts in overrides.\n");
        }

        if (FAILED(hr = d3d12_wg_state_object_apply_node_overrides(data, program)))
            return hr;

        if (FAILED(hr = d3d12_wg_state_object_program_init_slots(program, data)))
            return hr;

        if (FAILED(hr = d3d12_wg_state_object_populate_program(data, program)))
            return hr;

        if (FAILED(hr = d3d12_wg_state_object_assign_local_root_table_indices(data, program)))
            return hr;
    }

    /* Convert modules separately, per-program.
     * We will combine them with spec constants later when resolving the programs.
     * It's possible we'll have to compile multiple variants for cases where a node can be used as both
     * an entry point and non-entry point and there are overrides for dispatch parameters.
     * Many things can hopefully be resolved through spec constants of course, but we'll have to see
     * as the implementation comes together. */
    if (!(object->modules = vkd3d_calloc(data->entry_points_count, sizeof(*object->modules))))
        return E_OUTOFMEMORY;
    object->modules_count = data->entry_points_count;

    for (i = 0; i < data->entry_points_count; i++)
    {
        if (data->entry_points[i].node_input)
        {
            if (FAILED(hr = d3d12_wg_state_object_convert_entry_point(object, data, &object->modules[i],
                    &data->entry_points[i])))
                return hr;
        }
    }

    /* Create pipelines. Every program can have different overrides like node assignments, so we'll have to
     * assume we have to compile pipelines like this. For duplicated spec constant setups, we can fortunately
     * rely on caching to get us most of the way. */
    for (i = 0; i < object->programs_count; i++)
    {
        struct d3d12_wg_state_object_program *program = &object->programs[i];
        if (FAILED(hr = d3d12_wg_state_object_compile_program(object, data, program)))
            return hr;
    }

    if (FAILED(hr = d3d12_wg_state_object_allocate_lost_records(object)))
        return hr;

    return S_OK;
}

static HRESULT d3d12_wg_state_object_init(struct d3d12_wg_state_object *object, struct d3d12_device *device,
        const D3D12_STATE_OBJECT_DESC *desc)
{
    struct d3d12_wg_state_object_data data;
    HRESULT hr = S_OK;

    memset(&data, 0, sizeof(data));
    object->device = device;

    if (FAILED(hr = d3d12_wg_state_object_parse_subobjects(&data, device, desc)))
        goto fail;

    object->ID3D12StateObject_iface.lpVtbl = &d3d12_wg_state_object_vtbl;
    object->ID3D12StateObjectProperties1_iface.lpVtbl = &d3d12_wg_state_object_properties_vtbl;
    object->ID3D12WorkGraphProperties_iface.lpVtbl = &d3d12_work_graph_properties_vtbl;
    object->refcount = 1;
    object->internal_refcount = 1;
    object->type = desc->Type;

    if (FAILED(hr = d3d12_wg_state_object_compile_programs(object, &data)))
        goto fail;
    if (FAILED(hr = vkd3d_private_store_init(&object->private_store)))
        goto fail;
    d3d12_device_add_ref(object->device);

    object->entry_points = data.entry_points;
    object->entry_points_count = data.entry_points_count;
    data.entry_points = NULL;
    data.entry_points_count = 0;

fail:
    d3d12_wg_state_object_cleanup_data(&data, device);
    if (FAILED(hr))
        d3d12_wg_state_object_cleanup(object);
    return hr;
}

HRESULT d3d12_wg_state_object_create(struct d3d12_device *device, const D3D12_STATE_OBJECT_DESC *desc,
        struct d3d12_wg_state_object **state_object)
{
    struct d3d12_wg_state_object *object;
    HRESULT hr;

    if (!(object = vkd3d_calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    hr = d3d12_wg_state_object_init(object, device, desc);

    if (FAILED(hr))
    {
        vkd3d_free(object);
        return hr;
    }

    *state_object = object;
    return S_OK;
}

static void d3d12_command_list_update_buffer(struct d3d12_command_list *list,
        VkBuffer vk_buffer, VkDeviceSize vk_offset, VkDeviceSize size,
        const void *data_)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const uint8_t *data = data_;
    VkDeviceSize to_update;
    VkDeviceSize i;

    /* 64 KiB limit in the API. We don't expect significant spill over the limits. */
    for (i = 0; i < size; i += 64 * 1024)
    {
        to_update = min(size - i, 64 * 1024);
        VK_CALL(vkCmdUpdateBuffer(list->cmd.vk_command_buffer, vk_buffer,
                vk_offset + i, to_update, data + i));
    }
}

static void d3d12_command_list_workgraph_multi_input_staging(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, VkDeviceAddress *va, VkDeviceSize *size);

/* amdgpu-wddm fork: struct vkd3d_workgraph_output_guard. The packed offsets have the memory from the end of the
 * required scratch memory to the staging range of the multi GPU input (8 bytes per entry). */
/* amdgpu-wddm fork: the pool layout of the backing memory of the list. False without pool execution, or if the
 * backing memory is less than Min of the pools. */
static bool d3d12_wg_list_pool_layout(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, struct d3d12_wg_drain_layout *layout)
{
    return program->pool_execution &&
            d3d12_wg_drain_layout(program, list->wg_state.BackingMemory.SizeInBytes, layout);
}

/* amdgpu-wddm fork: the unrolled offsets of the execution: a region of the pool layout, or of the required scratch. */
static VkDeviceAddress d3d12_wg_offsets_va(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program)
{
    struct d3d12_wg_drain_layout layout;

    if (d3d12_wg_list_pool_layout(list, program, &layout))
        return list->wg_state.BackingMemory.StartAddress + layout.offsets.offset;
    return list->wg_state.BackingMemory.StartAddress + program->unrolled_offsets_scratch_offset;
}

/* amdgpu-wddm fork: the capacities of one half of a pool: the payload bytes and the packed entries of the output
 * guard of its producers, and the unrolled offsets in records. The admission keeps the records of a throttled level
 * inside all three capacities. The entry level, which writes pool 1, is not throttled. A record takes at least 4 bytes
 * of the payload capacity (an empty record 4 bytes, a record with a payload its stride, which is a multiple of 4 bytes)
 * and one unrolled offset. So the payload capacity of pool 1 is at most 4 bytes for each unrolled offset: its records
 * then fit in the unrolled offsets. The layout makes the unrolled offsets at least as large as a payload half of
 * pool 1 (d3d12_wg_drain_isolated_sizes), so this limit does not reduce it. */
static void d3d12_wg_pool_capacity(const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_drain_layout *layout, unsigned int pool, unsigned int half, uint32_t caps[3])
{
    VkDeviceSize offsets = min(layout->offsets.size / sizeof(uint32_t), (VkDeviceSize)UINT32_MAX);
    VkDeviceSize payload = layout->payload[pool][half].size;

    if (pool == 1)
        payload = min(payload, 4 * offsets);

    caps[D3D12_WG_POOL_PAYLOAD] = min(payload, (VkDeviceSize)UINT32_MAX) & ~(VkDeviceSize)15;
    caps[D3D12_WG_POOL_ENTRIES] = min(min(layout->entries.size / (2 * sizeof(uint32_t)),
            (VkDeviceSize)program->packed_limit), (VkDeviceSize)UINT32_MAX);
    caps[D3D12_WG_POOL_OFFSETS] = offsets;
}

static void d3d12_command_list_workgraph_init_output_guard(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, const struct vkd3d_unique_resource *resource)
{
    struct vkd3d_workgraph_output_guard guard;
    struct d3d12_wg_drain_layout layout;
    VkDeviceAddress packed_va, staging_va;
    VkDeviceSize packed_size, staging_size;
    uint32_t caps[3];

    /* With record pools, the retention pass of each level writes the guard of the next pool; this is the guard of
     * pool 1, half 0, for the entry level. */
    if (d3d12_wg_list_pool_layout(list, program, &layout))
    {
        memset(&guard, 0, sizeof(guard));
        if (program->num_levels > 1)
        {
            d3d12_wg_pool_capacity(program, &layout, 1, 0, caps);
            guard.payload_capacity = caps[D3D12_WG_POOL_PAYLOAD];
            guard.packed_capacity = caps[D3D12_WG_POOL_ENTRIES];
        }
        d3d12_command_list_update_buffer(list, resource->vk_buffer,
                list->wg_state.BackingMemory.StartAddress - resource->va + program->output_guard_scratch_offset,
                sizeof(guard), &guard);
        return;
    }

    packed_va = list->wg_state.BackingMemory.StartAddress + program->required_scratch_size;
    d3d12_command_list_workgraph_multi_input_staging(list, program, &staging_va, &staging_size);
    if (staging_size)
        packed_size = staging_va - packed_va;
    else
        packed_size = list->wg_state.BackingMemory.SizeInBytes - program->required_scratch_size;

    memset(&guard, 0, sizeof(guard));
    guard.payload_capacity = program->payload_ring_size;
    guard.packed_capacity = min(packed_size / (2 * sizeof(uint32_t)), program->packed_limit);

    d3d12_command_list_update_buffer(list, resource->vk_buffer,
            list->wg_state.BackingMemory.StartAddress - resource->va + program->output_guard_scratch_offset,
            sizeof(guard), &guard);
}

void d3d12_command_list_workgraph_initialize_scratch(struct d3d12_command_list *list)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_unique_resource *resource;
    struct d3d12_wg_state_object *wg_state;
    uint32_t wg_state_program_index;
    unsigned int i;
    VkMemoryBarrier2 vk_barrier;
    VkDependencyInfo dep_info;

    wg_state = (struct d3d12_wg_state_object *)(uintptr_t)list->wg_state.ProgramIdentifier.OpaqueData[1];
    wg_state_program_index = (uint32_t)list->wg_state.ProgramIdentifier.OpaqueData[0];

    if (!wg_state)
    {
        WARN("WG state is not set.\n");
        return;
    }

    if (!list->wg_state.BackingMemory.StartAddress)
        return;

    d3d12_command_list_end_current_render_pass(list, false);
    d3d12_command_list_debug_mark_begin_region(list, "WGInitScratch");

    /* INITIALIZE is assumed to happen in UAV. */
    memset(&dep_info, 0, sizeof(dep_info));
    memset(&vk_barrier, 0, sizeof(vk_barrier));
    dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.memoryBarrierCount = 1;
    dep_info.pMemoryBarriers = &vk_barrier;

    /* App is responsible for doing the UAV barrier here, so we just need to do a transitive barrier into CLEAR. */
    vk_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    vk_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    vk_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dep_info));

    if (list->wg_state.BackingMemory.SizeInBytes < wg_state->programs[wg_state_program_index].required_scratch_size)
    {
        ERR("Backing memory is not large enough (%"PRIu64" < %"PRIu64".\n",
                list->wg_state.BackingMemory.SizeInBytes, wg_state->programs[wg_state_program_index].required_scratch_size);
        return;
    }

    resource = vkd3d_va_map_deref(&list->device->memory_allocator.va_map, list->wg_state.BackingMemory.StartAddress);
    if (resource)
    {
        VK_CALL(vkCmdFillBuffer(list->cmd.vk_command_buffer, resource->vk_buffer,
                list->wg_state.BackingMemory.StartAddress - resource->va,
                wg_state->programs[wg_state_program_index].dividers_scratch_offset, 0));

        d3d12_command_list_update_buffer(list, resource->vk_buffer,
                list->wg_state.BackingMemory.StartAddress - resource->va +
                        wg_state->programs[wg_state_program_index].dividers_scratch_offset,
                wg_state->programs[wg_state_program_index].num_pipelines * sizeof(uint32_t),
                wg_state->programs[wg_state_program_index].coalesce_dividers_or_amp);

        d3d12_command_list_update_buffer(list, resource->vk_buffer,
                list->wg_state.BackingMemory.StartAddress - resource->va +
                        wg_state->programs[wg_state_program_index].share_mapping_scratch_offset,
                wg_state->programs[wg_state_program_index].num_pipelines * sizeof(uint32_t),
                wg_state->programs[wg_state_program_index].share_mapping);

        d3d12_command_list_update_buffer(list, resource->vk_buffer,
                list->wg_state.BackingMemory.StartAddress - resource->va +
                        wg_state->programs[wg_state_program_index].payload_expander_scratch_offset,
                wg_state->programs[wg_state_program_index].num_pipelines * sizeof(struct d3d12_wg_payload_expander_meta),
                wg_state->programs[wg_state_program_index].payload_expander_meta);

        /* amdgpu-wddm fork: the output bounds of the levels for the backing memory given here. */
        d3d12_command_list_workgraph_init_output_guard(list, &wg_state->programs[wg_state_program_index], resource);

        /* amdgpu-wddm fork, lab flavour: the canary ranges. */
        for (i = 0; i < wg_state->programs[wg_state_program_index].canary_count; i++)
        {
            VK_CALL(vkCmdFillBuffer(list->cmd.vk_command_buffer, resource->vk_buffer,
                    list->wg_state.BackingMemory.StartAddress - resource->va +
                            wg_state->programs[wg_state_program_index].canary_offsets[i],
                    VKD3D_WORKGRAPH_CANARY_SIZE, VKD3D_WORKGRAPH_CANARY_PATTERN));
        }
    }

    vk_barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    vk_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vk_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dep_info));
    d3d12_command_list_debug_mark_end_region(list);
}

/* amdgpu-wddm fork: a slot holds the records of one recursion depth, see struct d3d12_wg_state_object_program.
 * The count does not depend on the level, because a node can be reached at different levels of the graph. */
static unsigned int d3d12_command_list_workgraph_remaining_levels(
        const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program,
        unsigned int node_index)
{
    const struct vkd3d_shader_node_input_data *node_input;

    node_input = state->entry_points[program->slot_entry[node_index]].node_input;
    if (!node_input->recursion_factor)
        return 0;
    return node_input->recursion_factor - program->slot_depth[node_index];
}

static bool d3d12_command_list_workgraph_execute_node_cpu_entry(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program,
        uint32_t node_index, const D3D12_NODE_CPU_INPUT *desc,
        VkDeviceAddress output_payload, VkDeviceAddress input_payload,
        VkDeviceAddress vk_root_parameter_buffer_va)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_shader_node_input_data *node_input;
    struct vkd3d_shader_node_input_push_signature push;
    struct vkd3d_scratch_allocation offset_scratch;
    VkPushDataInfoEXT push_info;
    uint32_t table_index;

    memset(&push, 0, sizeof(push));

    /* Just rebind resources every time. We have to execute intermediate shaders anyway,
     * which clobbers all descriptor state. */
    d3d12_command_list_update_global_descriptor_heap(list);
    push.root_parameter_bda = vk_root_parameter_buffer_va;

    node_input = state->entry_points[program->slot_entry[node_index]].node_input;

    push.node_payload_bda = input_payload;
    push.node_payload_output_bda = output_payload;
    push.node_remaining_recursion_levels = d3d12_command_list_workgraph_remaining_levels(state, program, node_index);

    push.node_payload_stride_or_offsets_bda = desc->RecordStrideInBytes;
    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD, sizeof(uint32_t) * 3, sizeof(uint32_t), &offset_scratch))
        return false;

    ((uint32_t *)offset_scratch.host_ptr)[0] = 0; /* primary offset */
    ((uint32_t *)offset_scratch.host_ptr)[1] = desc->NumRecords & ~(WG_DIVIDER - 1u); /* secondary offset */
    ((uint32_t *)offset_scratch.host_ptr)[2] = desc->NumRecords; /* total nodes */
    push.node_linear_offset_bda = offset_scratch.va;
    push.node_total_nodes_bda = offset_scratch.va + 2 * sizeof(uint32_t);

    push.node_payload_output_atomic_bda = list->wg_state.BackingMemory.StartAddress;
    /* Counteract fixed offset applied by shader */
    push.node_payload_output_offset = program->required_scratch_size / sizeof(uint32_t) - 2;

    table_index = program->local_root_table_indices[node_index];
    if (table_index != UINT32_MAX)
    {
        push.local_root_signature_bda =
                list->wg_state.NodeLocalRootArgumentsTable.StartAddress + table_index *
                list->wg_state.NodeLocalRootArgumentsTable.StrideInBytes;
    }

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            desc->NumRecords == 1 && node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING ?
            program->pipelines[node_index].vk_static_cpu_node_entry_pipeline :
            program->pipelines[node_index].vk_cpu_node_entry_pipeline));

    memset(&push_info, 0, sizeof(push_info));
    push_info.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
    push_info.data.address = &push;
    push_info.data.size = sizeof(push);
    push_info.offset = 0;
    VK_CALL(vkCmdPushDataEXT(list->cmd.vk_command_buffer, &push_info));

    if (desc->NumRecords == 1 && node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING)
    {
        /* Common case for entry nodes that just kick off a large amount of work over a grid using a single record.
         * TODO: Can also unroll (few) multiple records if we run into that case in the wild. */
        uint32_t num_wgx[3];
        uint32_t i;

        if (node_input->dispatch_grid_is_upper_bound && node_input->dispatch_grid_components != 0)
        {
            const uint8_t *records = desc->pRecords;
            for (i = 0; i < 3; i++)
                num_wgx[i] = 1;

            if (node_input->dispatch_grid_type_bits == 32)
            {
                const uint32_t *grid = (const uint32_t *)(records + node_input->dispatch_grid_offset);
                for (i = 0; i < node_input->dispatch_grid_components; i++)
                    num_wgx[i] = grid[i];
            }
            else
            {
                for (i = 0; i < desc->NumRecords; i++)
                {
                    const uint16_t *grid = (const uint16_t *)(records + node_input->dispatch_grid_offset);
                    for (i = 0; i < node_input->dispatch_grid_components; i++)
                        num_wgx[i] = grid[i];
                }
            }
        }
        else
        {
            for (i = 0; i < 3; i++)
                num_wgx[i] = node_input->broadcast_grid[i];
        }

        if (list->device->vk_info.EXT_debug_utils)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CPU entry (static) - %s[%u]", node_input->node_id, node_input->node_array_index);
            d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
        }

        VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, num_wgx[0], num_wgx[1], num_wgx[2]));
    }
    else if (node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING)
    {
        uint32_t num_x = node_input->broadcast_grid[0];
        uint32_t num_y = node_input->broadcast_grid[1];
        uint32_t num_z = node_input->broadcast_grid[2];

        uint32_t num_wgs_x = desc->NumRecords;
        uint32_t amplification;
        uint32_t i, j;

        /* CPU entry nodes are likely to have a very large MaxDispatchGrid since it functions like a vkCmdDispatch(). Resolve it here. */
        if (node_input->dispatch_grid_is_upper_bound && node_input->dispatch_grid_components != 0)
        {
            const uint8_t *records = desc->pRecords;
            uint32_t max_dispatch[3] = { 1, 1, 1 };

            if (node_input->dispatch_grid_type_bits == 32)
            {
                for (i = 0; i < desc->NumRecords; i++)
                {
                    const uint32_t *grid = (const uint32_t *)(records + i * desc->RecordStrideInBytes + node_input->dispatch_grid_offset);
                    for (j = 0; j < node_input->dispatch_grid_components; j++)
                        max_dispatch[j] = max(max_dispatch[j], grid[j]);
                }
            }
            else
            {
                for (i = 0; i < desc->NumRecords; i++)
                {
                    const uint16_t *grid = (const uint16_t *)(records + i * desc->RecordStrideInBytes + node_input->dispatch_grid_offset);
                    for (j = 0; j < node_input->dispatch_grid_components; j++)
                        max_dispatch[j] = max(max_dispatch[j], grid[j]);
                }
            }

            num_x = min(num_x, max_dispatch[0]);
            num_y = min(num_y, max_dispatch[1]);
            num_z = min(num_z, max_dispatch[2]);
        }

        /* If the SV_DispatchGrid does not contain a component, it's implied to be 1. */
        if (node_input->dispatch_grid_components)
        {
            if (node_input->dispatch_grid_components < 3)
                num_z = 1;
            if (node_input->dispatch_grid_components < 2)
                num_y = 1;
        }

        /* Try to balance amplification rate. */
        amplification = min(MAX_AMPLIFICATION_RATE, num_x * num_y * num_z);
        amplification /= max(desc->NumRecords, 1u);
        amplification = max(amplification, 1u);

        if (list->device->vk_info.EXT_debug_utils)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CPU entry (amplified) - %s[%u]", node_input->node_id, node_input->node_array_index);
            d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
        }

        /* Primary offset */
        if (num_wgs_x >= WG_DIVIDER)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, WG_DIVIDER, num_wgs_x / WG_DIVIDER, amplification));

        push.node_linear_offset_bda += sizeof(uint32_t);
        VK_CALL(vkCmdPushDataEXT(list->cmd.vk_command_buffer, &push_info));

        /* Secondary offset */
        if (num_wgs_x % WG_DIVIDER)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, num_wgs_x % WG_DIVIDER, 1, amplification));
    }
    else
    {
        uint32_t num_wgs_x;

        if (list->device->vk_info.EXT_debug_utils)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CPU entry - %s[%u]", node_input->node_id, node_input->node_array_index);
            d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
        }

        if (node_input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_COALESCING)
            num_wgs_x = (desc->NumRecords + node_input->coalesce_factor - 1) / node_input->coalesce_factor;
        else
            num_wgs_x = align(desc->NumRecords, THREAD_COALESCE_COUNT) / THREAD_COALESCE_COUNT;

        /* Primary offset. */
        if (num_wgs_x >= WG_DIVIDER)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, WG_DIVIDER, num_wgs_x / WG_DIVIDER, 1));

        push.node_linear_offset_bda += sizeof(uint32_t);
        VK_CALL(vkCmdPushDataEXT(list->cmd.vk_command_buffer, &push_info));

        /* Secondary offset */
        if (num_wgs_x % WG_DIVIDER)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, num_wgs_x % WG_DIVIDER, 1, 1));
    }

    VKD3D_BREADCRUMB_AUX32(0);
    VKD3D_BREADCRUMB_AUX32(node_index);
    VKD3D_BREADCRUMB_TAG(node_input->node_id);
    VKD3D_BREADCRUMB_AUX32(node_input->node_array_index);
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_NODE);

    return true;
}

static void d3d12_command_list_workgraph_barrier(struct d3d12_command_list *list,
        VkPipelineStageFlags2 vk_dst_stages, VkAccessFlags2 vk_dst_access)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    VkMemoryBarrier2 vk_barrier;
    VkDependencyInfo dep_info;

    memset(&dep_info, 0, sizeof(dep_info));
    memset(&vk_barrier, 0, sizeof(vk_barrier));
    dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.memoryBarrierCount = 1;
    dep_info.pMemoryBarriers = &vk_barrier;

    /* Back to back workgraphs without barrier is allowed, as long as app doesn't expect ordering. */
    vk_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    vk_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vk_barrier.dstStageMask = vk_dst_stages;
    vk_barrier.dstAccessMask = vk_dst_access;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dep_info));
}

static void d3d12_command_list_emit_distribute_workgroups(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program, VkDeviceAddress admit_va)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_workgroups_args args;

    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE, program->workgroup_distributor.vk_pipeline));

    args.node_atomics_va = list->wg_state.BackingMemory.StartAddress;
    args.commands_va = program->indirect_commands_scratch_base_offset + list->wg_state.BackingMemory.StartAddress;
    args.dividers_va = program->dividers_scratch_offset + list->wg_state.BackingMemory.StartAddress;
    args.node_share_mapping_va = program->share_mapping_scratch_offset + list->wg_state.BackingMemory.StartAddress;
    args.output_guard_va = program->output_guard_scratch_offset + list->wg_state.BackingMemory.StartAddress;
    args.lost_records_va = state->lost_records.va;
    args.admit_va = admit_va;
    args.num_nodes = program->num_pipelines;

    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
            program->workgroup_distributor.vk_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            sizeof(args), &args);

    if (list->device->vk_info.EXT_debug_utils)
        d3d12_command_list_debug_mark_label(list, "Distribute Work Groups", 1.0f, 0.8f, 0.8f, 1.0f);

    VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, 1, 1, 1));

    VKD3D_BREADCRUMB_TAG("distribute");
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);

    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

static void d3d12_command_list_emit_distribute_payload_offsets(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program,
        uint32_t level, D3D12_GPU_VIRTUAL_ADDRESS payload_va)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_complete_compaction_args complete_args;
    struct vkd3d_workgraph_payload_offsets_args args;
    const struct vkd3d_unique_resource *resource;
    VkDeviceSize vk_offset;

    resource = vkd3d_va_map_deref(&list->device->memory_allocator.va_map, list->wg_state.BackingMemory.StartAddress);
    if (!resource)
        return;

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            program->payload_offset_expander.vk_pipeline));

    args.commands = list->wg_state.BackingMemory.StartAddress + program->indirect_commands_scratch_base_offset;
    args.meta = list->wg_state.BackingMemory.StartAddress + program->payload_expander_scratch_offset;
    args.payload = payload_va;
    args.unrolled_offsets = d3d12_wg_offsets_va(list, program);
    args.packed_offset_counts = list->wg_state.BackingMemory.StartAddress + program->required_scratch_size;

    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
            program->payload_offset_expander.vk_pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            sizeof(args), &args);

    if (list->device->vk_info.EXT_debug_utils)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Payload Distribute - level %u", level);
        d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
    }

    vk_offset = list->wg_state.BackingMemory.StartAddress - resource->va;
    vk_offset += program->indirect_commands_scratch_base_offset;

    VK_CALL(vkCmdDispatchIndirect(list->cmd.vk_command_buffer, resource->vk_buffer, vk_offset));
    VKD3D_BREADCRUMB_TAG("payload-offset");
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);

    /* This shader only expands offsets, indirect data is already written. */
    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    /* To prepare for ubershader payload expander, we cannot rely on single-pass handling anymore. */
    if (program->compact_broadcast_nodes_with_max_grid)
    {
        uint32_t wgx;
        complete_args.commands = args.commands;
        complete_args.meta = args.meta;
        complete_args.node_count = program->num_pipelines;

        VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                program->complete_compaction.vk_pipeline));

        d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
                program->complete_compaction.vk_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT,
                sizeof(complete_args), &complete_args);

        wgx = complete_args.node_count;
        wgx = align(complete_args.node_count, vkd3d_meta_get_workgraph_complete_compaction_workgroup_size()) /
                vkd3d_meta_get_workgraph_complete_compaction_workgroup_size();
        VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, wgx, 1, 1));

        d3d12_command_list_workgraph_barrier(list,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
}

struct vkd3d_workgraph_gpu_input_indirect
{
    uint32_t primary_indirect[3];
    uint32_t primary_linear_offset;
    uint32_t secondary_indirect[3];
    uint32_t secondary_linear_offset;
};

static bool d3d12_wg_requires_primary_execution(const struct d3d12_wg_state_object *state)
{
    /* amdgpu-wddm fork: the same decision as the distributor pipeline (struct vkd3d_workgraph_indirect_ops). */
    return state->device->meta_ops.workgraph.require_wg_divider;
}

static void d3d12_command_list_workgraph_execute_node_gpu(
        struct d3d12_command_list *list, const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program,
        D3D12_GPU_VIRTUAL_ADDRESS output_va, D3D12_GPU_VIRTUAL_ADDRESS input_va,
        unsigned int level, unsigned int node_index, const struct vkd3d_scratch_allocation *indirect_scratch,
        VkDeviceAddress vk_root_parameter_buffer_va)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_shader_node_input_data *node_input;
    struct vkd3d_shader_node_input_push_signature push;
    VkDeviceAddress secondary_linear_offset_bda;
    VkDeviceAddress primary_linear_offset_bda;
    VkDeviceSize vk_secondary_indirect_offset;
    VkDeviceSize vk_primary_indirect_offset;
    VkPushDataInfoEXT push_info;
    VkBuffer vk_indirect_buffer;
    unsigned int table_index;

    memset(&push, 0, sizeof(push));
    node_input = state->entry_points[program->slot_entry[node_index]].node_input;

    /* Just rebind resources every time. We have to execute intermediate shaders anyway,
     * which clobbers all descriptor state. */
    d3d12_command_list_update_global_descriptor_heap(list);
    push.root_parameter_bda = vk_root_parameter_buffer_va;

    push.node_payload_output_bda = output_va;
    push.node_remaining_recursion_levels = d3d12_command_list_workgraph_remaining_levels(state, program, node_index);

    if (indirect_scratch)
    {
        /* GPU node entries load payload/stride indirectly straight from app buffer. */
        push.node_payload_bda = input_va + offsetof(D3D12_NODE_GPU_INPUT, Records.StartAddress);
        push.node_payload_stride_or_offsets_bda = input_va + offsetof(D3D12_NODE_GPU_INPUT, Records.StrideInBytes);
        push.node_total_nodes_bda = input_va + offsetof(D3D12_NODE_GPU_INPUT, NumRecords);

        vk_indirect_buffer = indirect_scratch->buffer;
        vk_primary_indirect_offset = indirect_scratch->offset;
        vk_secondary_indirect_offset = indirect_scratch->offset;
        vk_primary_indirect_offset += node_index * sizeof(struct vkd3d_workgraph_gpu_input_indirect);
        vk_secondary_indirect_offset += node_index * sizeof(struct vkd3d_workgraph_gpu_input_indirect);
        vk_primary_indirect_offset += offsetof(struct vkd3d_workgraph_gpu_input_indirect, primary_indirect);
        vk_secondary_indirect_offset += offsetof(struct vkd3d_workgraph_gpu_input_indirect, secondary_indirect);

        /* amdgpu-wddm fork: the linear offsets of this node's own indirect command, as for the dispatch offsets
         * above. They differ from 0 only when the primary execution is used. */
        primary_linear_offset_bda = indirect_scratch->va +
                node_index * sizeof(struct vkd3d_workgraph_gpu_input_indirect) +
                offsetof(struct vkd3d_workgraph_gpu_input_indirect, primary_linear_offset);
        secondary_linear_offset_bda = indirect_scratch->va +
                node_index * sizeof(struct vkd3d_workgraph_gpu_input_indirect) +
                offsetof(struct vkd3d_workgraph_gpu_input_indirect, secondary_linear_offset);

        VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                program->pipelines[node_index].vk_gpu_node_entry_pipeline));
    }
    else
    {
        const struct vkd3d_unique_resource *resource;
        VkDeviceAddress indirect_va =
                list->wg_state.BackingMemory.StartAddress + program->indirect_commands_scratch_node_offset +
                sizeof(struct d3d12_workgraph_indirect_command) * node_index;

        push.node_payload_stride_or_offsets_bda = d3d12_wg_offsets_va(list, program);
        push.node_payload_bda = input_va;

        primary_linear_offset_bda = indirect_va + offsetof(struct d3d12_workgraph_indirect_command, primary_linear_offset);
        secondary_linear_offset_bda = indirect_va + offsetof(struct d3d12_workgraph_indirect_command, secondary_linear_offset);

        resource = vkd3d_va_map_deref(&list->device->memory_allocator.va_map, list->wg_state.BackingMemory.StartAddress);
        if (!resource)
            return;

        vk_indirect_buffer = resource->vk_buffer;
        vk_primary_indirect_offset = indirect_va - resource->va;
        vk_secondary_indirect_offset = indirect_va - resource->va;
        vk_primary_indirect_offset += offsetof(struct d3d12_workgraph_indirect_command, primary_execute);
        vk_secondary_indirect_offset += offsetof(struct d3d12_workgraph_indirect_command, secondary_execute);

        push.node_total_nodes_bda =
                list->wg_state.BackingMemory.StartAddress + program->indirect_commands_scratch_node_offset +
                sizeof(struct d3d12_workgraph_indirect_command) * node_index +
                offsetof(struct d3d12_workgraph_indirect_command, end_elements);

        VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                program->pipelines[node_index].vk_non_entry_pipeline));
    }

    push.node_payload_output_atomic_bda = list->wg_state.BackingMemory.StartAddress;
    /* Compensate for offset applied by shader. */
    push.node_payload_output_offset = program->required_scratch_size / sizeof(uint32_t) - 2;

    table_index = program->local_root_table_indices[node_index];
    if (table_index != UINT32_MAX)
    {
        push.local_root_signature_bda =
                list->wg_state.NodeLocalRootArgumentsTable.StartAddress + table_index *
                list->wg_state.NodeLocalRootArgumentsTable.StrideInBytes;
    }

    push.node_linear_offset_bda = primary_linear_offset_bda;

    memset(&push_info, 0, sizeof(push_info));
    push_info.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
    push_info.data.address = &push;
    push_info.data.size = sizeof(push);
    push_info.offset = 0;
    VK_CALL(vkCmdPushDataEXT(list->cmd.vk_command_buffer, &push_info));

    if (list->device->vk_info.EXT_debug_utils)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Node Dispatch - level %u - node %s[%u]", level, node_input->node_id, node_input->node_array_index);
        d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
    }

    if (d3d12_wg_requires_primary_execution(state))
        VK_CALL(vkCmdDispatchIndirect(list->cmd.vk_command_buffer, vk_indirect_buffer, vk_primary_indirect_offset));

    push.node_linear_offset_bda = secondary_linear_offset_bda;
    VK_CALL(vkCmdPushDataEXT(list->cmd.vk_command_buffer, &push_info));

    VK_CALL(vkCmdDispatchIndirect(list->cmd.vk_command_buffer, vk_indirect_buffer, vk_secondary_indirect_offset));

    VKD3D_BREADCRUMB_AUX32(level);
    VKD3D_BREADCRUMB_AUX32(node_index);
    VKD3D_BREADCRUMB_TAG(node_input->node_id);
    VKD3D_BREADCRUMB_AUX32(node_input->node_array_index);
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_NODE);
}

/* amdgpu-wddm fork: the record pools of one DispatchGraph call. level_args_va has struct
 * vkd3d_workgraph_pool_level_args for each half and level: half x num_levels + level. */
struct d3d12_wg_pool_dispatch
{
    struct d3d12_wg_drain_layout layout;
    VkDeviceAddress level_args_va;
};

static VkDeviceAddress d3d12_wg_pool_level_va(const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_pool_dispatch *pools, unsigned int level, unsigned int half)
{
    return pools->level_args_va + (half * program->num_levels + level) * sizeof(struct vkd3d_workgraph_pool_level_args);
}

static VkDeviceAddress d3d12_wg_pool_payload_va(const struct d3d12_command_list *list,
        const struct d3d12_wg_pool_dispatch *pools, unsigned int pool, unsigned int half)
{
    return list->wg_state.BackingMemory.StartAddress + pools->layout.payload[pool][half].offset;
}

/* Uploads the admission table and the level arguments of the pools for the backing memory of the list. */
static bool d3d12_command_list_workgraph_prepare_pools(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, struct d3d12_wg_pool_dispatch *pools)
{
    VkDeviceAddress base = list->wg_state.BackingMemory.StartAddress;
    struct vkd3d_workgraph_pool_level_args *args;
    struct vkd3d_scratch_allocation table, levels;
    unsigned int level, half;

    if (!d3d12_wg_list_pool_layout(list, program, &pools->layout))
        return false;

    if (!d3d12_command_list_workgraph_allocate_scratch(list, VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD,
            program->num_pipelines * sizeof(struct vkd3d_workgraph_pool_admission), 64, &table))
        return false;
    memcpy(table.host_ptr, program->pool_admission, program->num_pipelines * sizeof(struct vkd3d_workgraph_pool_admission));

    if (!d3d12_command_list_workgraph_allocate_scratch(list, VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD,
            2 * program->num_levels * sizeof(*args), 64, &levels))
        return false;
    pools->level_args_va = levels.va;

    for (half = 0; half < 2; half++)
    {
        for (level = 0; level < program->num_levels; level++)
        {
            args = (struct vkd3d_workgraph_pool_level_args *)levels.host_ptr + half * program->num_levels + level;
            memset(args, 0, sizeof(*args));
            args->commands_va = base + program->indirect_commands_scratch_base_offset;
            args->offsets_va = base + pools->layout.offsets.offset;
            if (level)
            {
                args->source_va = base + pools->layout.payload[level][half].offset;
                args->target_va = base + pools->layout.payload[level][half ^ 1].offset;
            }
            args->meta_va = base + program->payload_expander_scratch_offset;
            args->share_mapping_va = base + program->share_mapping_scratch_offset;
            args->pool_states_va = base + program->pool_state_scratch_offset;
            args->atomics_va = base + program->counters_scratch_offset;
            args->entries_va = base + pools->layout.entries.offset;
            args->guard_va = base + program->output_guard_scratch_offset;
            args->table_va = table.va;
            args->admit_va = base + program->pool_admit_scratch_offset;
            args->control_va = base + program->drain_control_scratch_offset;
            args->pool_state_stride = program->pool_state_stride;
            args->level = level;
            args->num_nodes = program->num_pipelines;
            if (level + 1 < program->num_levels)
            {
                args->inject_level = level + 1;
                d3d12_wg_pool_capacity(program, &pools->layout, level + 1, half, args->caps);
                args->payload_capacity = args->caps[D3D12_WG_POOL_PAYLOAD];
                args->packed_capacity = args->caps[D3D12_WG_POOL_ENTRIES];
                /* The lab hook pool-caps:<L> gives the passes of the rounds of half 1 a record capacity of 0 for pool
                 * L, so that round 2 finds a valid pool state above the capacity. The output guard keeps its value. */
                if (half && d3d12_wg_fault_injected("pool-caps", level + 1))
                    args->caps[D3D12_WG_POOL_OFFSETS] = 0;
            }
            /* The lab hooks write an invalid pool state of pool "level" in the rounds of half 0, so round 2 reads it:
             * 1 for payload bytes above every capacity, 2 for one record more than the slots hold, 3 for blocks 16
             * bytes after the place of the retention pass with a valid head. */
            if (level && !half && d3d12_wg_fault_injected("pool-head", level))
                args->fault_head = 1;
            else if (level && !half && d3d12_wg_fault_injected("pool-records", level))
                args->fault_head = 2;
            else if (level && !half && d3d12_wg_fault_injected("pool-slot", level))
                args->fault_head = 3;
        }
    }

    return true;
}

/* One admission or retention pass. The barrier before it orders it after every earlier write of the execution, and
 * makes them visible to its reads and writes: the retention pass overwrites the packed entries that the expander read,
 * and writes the counters that the nodes add to. */
static void d3d12_command_list_workgraph_emit_pool_pass(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, VkDeviceAddress level_va,
        enum vkd3d_workgraph_pool_mode mode, unsigned int level)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_pool_args args;

    d3d12_command_list_workgraph_barrier(list, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    memset(&args, 0, sizeof(args));
    args.level_va = level_va;
    args.mode = mode;

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            program->pool_pass.vk_pipeline));
    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer, program->pool_pass.vk_pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, sizeof(args), &args);

    if (list->device->vk_info.EXT_debug_utils)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Pool %s - level %u", mode == VKD3D_WORKGRAPH_POOL_MODE_ADMIT ? "admit" : "retain",
                level);
        d3d12_command_list_debug_mark_label(list, buf, 1.0f, 0.8f, 0.8f, 1.0f);
    }

    /* The admission is one serial loop over the slots. The retention copies with up to 64 groups. */
    VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, mode == VKD3D_WORKGRAPH_POOL_MODE_ADMIT ? 1 : 64, 1, 1));
    VKD3D_BREADCRUMB_TAG("pool");
    VKD3D_BREADCRUMB_AUX32(mode);
    VKD3D_BREADCRUMB_AUX32(level);
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);

    /* The nodes read the injected entries and counters and add to them, and the distributor reads the admitted
     * counts. */
    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

/* With record pools (pool_level_va is not 0), a level with outputs admits the records that fit the next pool before
 * the distributor, and retains the others after the expander. */
static void d3d12_command_list_workgraph_execute_level(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program,
        uint32_t level, VkDeviceAddress output_payload, VkDeviceAddress input_payload,
        VkDeviceAddress vk_root_parameter_buffer_va, VkDeviceAddress pool_level_va)
{
    bool throttled = pool_level_va && level + 1 < program->num_levels;
    uint32_t i;

    if (throttled)
        d3d12_command_list_workgraph_emit_pool_pass(list, program, pool_level_va, VKD3D_WORKGRAPH_POOL_MODE_ADMIT, level);
    d3d12_command_list_emit_distribute_workgroups(list, state, program, throttled ?
            list->wg_state.BackingMemory.StartAddress + program->pool_admit_scratch_offset : 0);
    d3d12_command_list_emit_distribute_payload_offsets(list, state, program, level, input_payload);
    if (throttled)
        d3d12_command_list_workgraph_emit_pool_pass(list, program, pool_level_va, VKD3D_WORKGRAPH_POOL_MODE_RETAIN, level);

    /* Execute nodes */
    for (i = 0; i < program->levels[level].nodes_count; i++)
    {
        d3d12_command_list_workgraph_execute_node_gpu(list, state, program,
                output_payload, input_payload, level, program->levels[level].nodes[i], NULL,
                vk_root_parameter_buffer_va);
    }

    /* Execute shared nodes. Distribute workgroups already handled the mapping. */
    for (i = 0; i < program->levels[level].shared_nodes_count; i++)
    {
        d3d12_command_list_workgraph_execute_node_gpu(list, state, program,
                output_payload, input_payload, level, program->levels[level].shared_nodes[i].node_pipeline_index, NULL,
                vk_root_parameter_buffer_va);
    }
}

/* The coalesce dividers and the entry point mapping of the GPU input setup pass. They are the same for each input of
 * a DispatchGraph call. */
struct d3d12_wg_gpu_input_tables
{
    VkDeviceAddress dividers_va;
    VkDeviceAddress entry_point_mapping_va;
};

static bool d3d12_command_list_workgraph_setup_indirect_tables(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *wg_state,
        const struct d3d12_wg_state_object_program *program, struct d3d12_wg_gpu_input_tables *tables)
{
    struct vkd3d_scratch_allocation dividers_scratch;
    struct vkd3d_scratch_allocation entry_scratch;
    unsigned int i, j;

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD,
            program->num_pipelines * sizeof(uint32_t),
            64, &dividers_scratch))
        return false;

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD,
            program->num_pipelines * sizeof(uint32_t),
            64, &entry_scratch))
        return false;

    for (i = 0; i < program->num_pipelines; i++)
    {
        const struct vkd3d_shader_node_input_data *input;
        int coalesce_divider_or_amp;
        uint32_t max_amplification;

        input = wg_state->entry_points[program->slot_entry[i]].node_input;
        if (input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_BROADCASTING)
        {
            max_amplification = input->broadcast_grid[0] * input->broadcast_grid[1] * input->broadcast_grid[2];
            max_amplification = min(max_amplification, MAX_AMPLIFICATION_RATE);
            coalesce_divider_or_amp = -(int)max_amplification;
        }
        else if (input->launch_type == VKD3D_SHADER_NODE_LAUNCH_TYPE_COALESCING)
            coalesce_divider_or_amp = (int)input->coalesce_factor;
        else
            coalesce_divider_or_amp = THREAD_COALESCE_COUNT;

        ((uint32_t *)entry_scratch.host_ptr)[i] = UINT32_MAX;
        ((int32_t *)dividers_scratch.host_ptr)[i] = coalesce_divider_or_amp;
    }

    for (i = 0; i < program->levels[0].nodes_count; i++)
    {
        unsigned int node_index = program->levels[0].nodes[i];
        ((uint32_t *)entry_scratch.host_ptr)[node_index] = i;
    }

    for (i = 0; i < program->levels[0].shared_nodes_count; i++)
    {
        unsigned int node_pipeline_index = program->levels[0].shared_nodes[i].node_pipeline_index;
        unsigned int node_payload_index = program->levels[0].shared_nodes[i].node_payload_index;
        for (j = 0; j < program->levels[0].nodes_count; j++)
        {
            if (node_payload_index == program->levels[0].nodes[j])
            {
                ((uint32_t *)entry_scratch.host_ptr)[node_pipeline_index] = j;
                break;
            }
        }
    }

    tables->dividers_va = dividers_scratch.va;
    tables->entry_point_mapping_va = entry_scratch.va;
    return true;
}

/* Writes the indirect commands of every pipeline for the D3D12_NODE_GPU_INPUT at va. The caller emits the barrier
 * before the commands are used, so that the setup of several inputs needs one barrier. */
static void d3d12_command_list_workgraph_emit_setup_indirect(
        struct d3d12_command_list *list, const struct d3d12_wg_state_object_program *program,
        const struct d3d12_wg_gpu_input_tables *tables, D3D12_GPU_VIRTUAL_ADDRESS va,
        const struct vkd3d_scratch_allocation *indirect_scratch)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_setup_gpu_input_args args;
    unsigned int num_wgs;

    args.gpu_input_va = va;
    args.indirect_commands_va = indirect_scratch->va;
    args.coalesce_divider_va = tables->dividers_va;
    args.entry_point_mapping_va = tables->entry_point_mapping_va;
    args.num_entry_points = program->num_pipelines;

    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
            program->gpu_input_setup.vk_pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            sizeof(args), &args);

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, program->gpu_input_setup.vk_pipeline));

    num_wgs = align(args.num_entry_points, vkd3d_meta_get_workgraph_setup_gpu_input_workgroup_size()) /
            vkd3d_meta_get_workgraph_setup_gpu_input_workgroup_size();

    if (list->device->vk_info.EXT_debug_utils)
        d3d12_command_list_debug_mark_label(list, "Setup Indirect Nodes", 1.0f, 0.8f, 0.8f, 1.0f);

    VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, num_wgs, 1, 1));

    VKD3D_BREADCRUMB_TAG("setup-indirect");
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);
}

static void d3d12_command_list_workgraph_setup_indirect_barrier(struct d3d12_command_list *list)
{
    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

static bool d3d12_command_list_workgraph_setup_indirect(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *wg_state,
        const struct d3d12_wg_state_object_program *program, D3D12_GPU_VIRTUAL_ADDRESS va,
        struct vkd3d_scratch_allocation *indirect_scratch)
{
    struct d3d12_wg_gpu_input_tables tables;

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_DEVICE_STORAGE,
            program->num_pipelines * sizeof(struct vkd3d_workgraph_gpu_input_indirect),
            64, indirect_scratch))
        return false;

    if (!d3d12_command_list_workgraph_setup_indirect_tables(list, wg_state, program, &tables))
        return false;

    d3d12_command_list_workgraph_emit_setup_indirect(list, program, &tables, va, indirect_scratch);
    d3d12_command_list_workgraph_setup_indirect_barrier(list);
    return true;
}

/* amdgpu-wddm fork: payload ring index (0 or 1) of the execution, in the backing memory. */
static VkDeviceAddress d3d12_wg_payload_va(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, unsigned int index)
{
    return list->wg_state.BackingMemory.StartAddress +
            (index ? program->payload1_scratch_offset : program->payload0_scratch_offset);
}

/* amdgpu-wddm fork: the output payload of the entry level: payload ring 0, or half 0 of pool 1. */
static VkDeviceAddress d3d12_wg_entry_output_va(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program)
{
    struct d3d12_wg_drain_layout layout;

    if (d3d12_wg_list_pool_layout(list, program, &layout))
        return program->num_levels > 1 ? list->wg_state.BackingMemory.StartAddress + layout.payload[1][0].offset : 0;
    return d3d12_wg_payload_va(list, program, 0);
}

static bool d3d12_command_list_workgraph_execute_entry_gpu(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program, D3D12_GPU_VIRTUAL_ADDRESS va,
        VkDeviceAddress vk_root_param_buffer_va)
{
    struct vkd3d_scratch_allocation indirect_scratch;
    unsigned int i;

    if (program->levels[0].nodes_count == 0)
        return true;
    if (!d3d12_command_list_workgraph_setup_indirect(list, state, program, va, &indirect_scratch))
        return false;

    /* We don't know which entry gets to execute, so have to spam indirects.
     * FIXME: DGCC can help with this if number of entry points is large. */
    for (i = 0; i < program->levels[0].nodes_count; i++)
    {
        d3d12_command_list_workgraph_execute_node_gpu(
                list, state, program, d3d12_wg_entry_output_va(list, program), va, 0, program->levels[0].nodes[i],
                &indirect_scratch, vk_root_param_buffer_va);
    }

    /* Execute shared nodes. Indirect setup already covered for us. */
    for (i = 0; i < program->levels[0].shared_nodes_count; i++)
    {
        d3d12_command_list_workgraph_execute_node_gpu(
                list, state, program, d3d12_wg_entry_output_va(list, program), va, 0, program->levels[0].shared_nodes[i].node_pipeline_index,
                &indirect_scratch, vk_root_param_buffer_va);
    }

    return true;
}

/* amdgpu-wddm fork: the staging range of the multi GPU input in the backing memory of the list. It is at the end of
 * the backing memory that the driver uses. With MinSizeInBytes it has the minimum size. Each more
 * SizeGranularityInBytes adds to it, up to the maximum size with MaxSizeInBytes. The packed offsets before it have
 * at least their minimum size. Memory past the last whole granule or past MaxSizeInBytes is not used
 * (WorkGraphs.md, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS). */
static void d3d12_command_list_workgraph_multi_input_staging(const struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, VkDeviceAddress *va, VkDeviceSize *size)
{
    VkDeviceSize backing_size = list->wg_state.BackingMemory.SizeInBytes;
    struct d3d12_wg_drain_layout layout;
    VkDeviceSize used, staging;

    *va = 0;
    *size = 0;

    /* amdgpu-wddm fork: with record pools, the staging range is the one of the pool layout. */
    if (program->pool_execution)
    {
        if (d3d12_wg_list_pool_layout(list, program, &layout))
        {
            *va = list->wg_state.BackingMemory.StartAddress + layout.staging.offset;
            *size = layout.staging.size;
        }
        return;
    }

    if (backing_size >= program->min_backing_size)
    {
        used = program->min_backing_size + (backing_size - program->min_backing_size) /
                VKD3D_WORKGRAPH_BACKING_GRANULARITY * VKD3D_WORKGRAPH_BACKING_GRANULARITY;
        used = min(used, program->max_backing_size);
        staging = program->multi_input_staging_min_size + (used - program->min_backing_size);
        staging = min(staging, program->multi_input_staging_max_size);
    }
    else
    {
        /* The application gives less backing memory than GetWorkGraphMemoryRequirements() asks for. The packed
         * offsets keep their minimum size, the staging range gets what is left. The records that do not fit are
         * reported as lost. */
        WARN("Backing memory is less than the minimum (%"PRIu64" < %"PRIu64").\n",
                backing_size, program->min_backing_size);
        used = backing_size;
        if (used <= program->required_scratch_size + VKD3D_WORKGRAPH_PACKED_OFFSETS_MIN_SIZE)
            return;
        staging = used - program->required_scratch_size - VKD3D_WORKGRAPH_PACKED_OFFSETS_MIN_SIZE;
        staging = min(staging, program->multi_input_staging_min_size);
    }

    /* The merge slots start at 16-byte offsets of the range, as with a range of the required scratch memory. */
    staging &= ~(VkDeviceSize)15;
    *va = list->wg_state.BackingMemory.StartAddress + used - staging;
    *size = staging;
}

static void d3d12_command_list_workgraph_multi_gpu_input_pass(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object_program *program, struct vkd3d_workgraph_multi_gpu_input_prepare_args *args,
        enum vkd3d_workgraph_multi_gpu_input_mode mode, uint32_t num_groups, const char *tag)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;

    args->mode = mode;
    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
            program->multi_gpu_input_prepare.vk_pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, sizeof(*args), args);
    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            program->multi_gpu_input_prepare.vk_pipeline));

    if (list->device->vk_info.EXT_debug_utils)
        d3d12_command_list_debug_mark_label(list, tag, 1.0f, 0.8f, 0.8f, 1.0f);

    VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, num_groups, 1, 1));
    VKD3D_BREADCRUMB_TAG(tag);
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);

    d3d12_command_list_workgraph_barrier(list, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

/* amdgpu-wddm fork: D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT (WorkGraphs.md, D3D12_MULTI_NODE_GPU_INPUT).
 * The number of inputs and their record counts are only in GPU memory, and the spec sets no limit on them for a
 * compute-only graph. Meta passes (cs_workgraph_multi_gpu_input_*.comp) give each entry point direct slots and one
 * merge slot, each a D3D12_NODE_GPU_INPUT that then executes as with D3D12_DISPATCH_MODE_NODE_GPU_INPUT.
 * A direct slot reads the records of one large input in place. The merge slot reads the records of the other inputs
 * from the staging range in the backing memory, so each execution context has its own copy, and the state of the
 * passes is scratch memory of this call. */
static bool d3d12_command_list_workgraph_execute_entry_multi_gpu(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *state,
        const struct d3d12_wg_state_object_program *program, D3D12_GPU_VIRTUAL_ADDRESS va,
        VkDeviceAddress vk_root_param_buffer_va)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_multi_gpu_input_prepare_args prepare_args;
    struct vkd3d_workgraph_multi_gpu_input_copy_args copy_args;
    struct vkd3d_scratch_allocation indirect_scratch;
    struct vkd3d_scratch_allocation strides_scratch;
    struct vkd3d_scratch_allocation state_scratch;
    struct vkd3d_scratch_allocation slot_indirect;
    uint32_t num_entries, num_slots, node_index;
    struct d3d12_wg_gpu_input_tables tables;
    VkDeviceSize indirect_size, state_size;
    VkDeviceAddress slots_va, slot_va;
    bool has_record_data = false;
    unsigned int i, j, s;

    num_entries = program->levels[0].nodes_count;
    if (num_entries == 0)
        return true;
    num_slots = vkd3d_workgraph_multi_gpu_input_slot_count(num_entries);

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD, num_entries * sizeof(uint32_t), 64, &strides_scratch))
        return false;

    state_size = vkd3d_workgraph_multi_gpu_input_slots_offset(num_entries) +
            (VkDeviceSize)num_slots * sizeof(D3D12_NODE_GPU_INPUT);
    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_DEVICE_STORAGE, state_size, 64, &state_scratch))
        return false;

    indirect_size = program->num_pipelines * sizeof(struct vkd3d_workgraph_gpu_input_indirect);
    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_DEVICE_STORAGE, num_slots * indirect_size, 64, &indirect_scratch))
        return false;

    if (!d3d12_command_list_workgraph_setup_indirect_tables(list, state, program, &tables))
        return false;

    for (i = 0; i < num_entries; i++)
    {
        node_index = program->levels[0].nodes[i];
        /* The record stride of the merge slot is the application record size, aligned to 4 bytes. 0 means an
         * empty record. An entry point does not use the internal word of [NodeTrackRWInputSharing]: it must not
         * call FinishedCrossGroupSharing(). */
        ((uint32_t *)strides_scratch.host_ptr)[i] = align(state->entry_points[node_index].node_input->record_size, 4);
        if (state->entry_points[node_index].node_input->record_size)
            has_record_data = true;
    }

    memset(&prepare_args, 0, sizeof(prepare_args));
    prepare_args.multi_input_va = va;
    prepare_args.record_strides_va = strides_scratch.va;
    prepare_args.state_va = state_scratch.va;
    prepare_args.num_entry_points = num_entries;
    prepare_args.large_size = program->multi_input_large_size;
    prepare_args.status_va = state->lost_records.va;
    d3d12_command_list_workgraph_multi_input_staging(list, program,
            &prepare_args.staging_va, &prepare_args.staging_capacity);

    d3d12_command_list_workgraph_multi_gpu_input_pass(list, program, &prepare_args,
            VKD3D_WORKGRAPH_MULTI_INPUT_MODE_INIT, 1, "multi-gpu-input-init");
    d3d12_command_list_workgraph_multi_gpu_input_pass(list, program, &prepare_args,
            VKD3D_WORKGRAPH_MULTI_INPUT_MODE_CLASSIFY, program->multi_input_groups, "multi-gpu-input-classify");
    d3d12_command_list_workgraph_multi_gpu_input_pass(list, program, &prepare_args,
            VKD3D_WORKGRAPH_MULTI_INPUT_MODE_LAYOUT, 1, "multi-gpu-input-layout");

    /* Only empty records: the merge slots need no copy. */
    if (has_record_data)
    {
        memset(&copy_args, 0, sizeof(copy_args));
        copy_args.multi_input_va = va;
        copy_args.record_strides_va = strides_scratch.va;
        copy_args.state_va = state_scratch.va;
        copy_args.num_entry_points = num_entries;

        d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
                program->multi_gpu_input_copy.vk_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT, sizeof(copy_args), &copy_args);
        VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                program->multi_gpu_input_copy.vk_pipeline));

        if (list->device->vk_info.EXT_debug_utils)
            d3d12_command_list_debug_mark_label(list, "Multi GPU Input Copy", 1.0f, 0.8f, 0.8f, 1.0f);

        VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, program->multi_input_groups, 1, 1));
        VKD3D_BREADCRUMB_TAG("multi-gpu-input-copy");
        VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);

        /* The setup passes read the slots, the nodes read the slots and the copied records. */
        d3d12_command_list_workgraph_barrier(list, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    slots_va = state_scratch.va + vkd3d_workgraph_multi_gpu_input_slots_offset(num_entries);
    slot_indirect = indirect_scratch;
    for (s = 0; s < num_slots; s++)
    {
        slot_indirect.offset = indirect_scratch.offset + s * indirect_size;
        slot_indirect.va = indirect_scratch.va + s * indirect_size;
        d3d12_command_list_workgraph_emit_setup_indirect(list, program, &tables,
                slots_va + s * sizeof(D3D12_NODE_GPU_INPUT), &slot_indirect);
    }
    d3d12_command_list_workgraph_setup_indirect_barrier(list);

    /* A slot with no records has indirect commands of zero workgroups. */
    for (s = 0; s < num_slots; s++)
    {
        i = s / (VKD3D_WORKGRAPH_MULTI_INPUT_DIRECT_SLOTS + 1);
        node_index = program->levels[0].nodes[i];
        slot_va = slots_va + s * sizeof(D3D12_NODE_GPU_INPUT);
        slot_indirect.offset = indirect_scratch.offset + s * indirect_size;
        slot_indirect.va = indirect_scratch.va + s * indirect_size;

        d3d12_command_list_workgraph_execute_node_gpu(list, state, program, d3d12_wg_entry_output_va(list, program), slot_va,
                0, node_index, &slot_indirect, vk_root_param_buffer_va);

        for (j = 0; j < program->levels[0].shared_nodes_count; j++)
        {
            if (program->levels[0].shared_nodes[j].node_payload_index == node_index)
            {
                d3d12_command_list_workgraph_execute_node_gpu(list, state, program, d3d12_wg_entry_output_va(list, program),
                        slot_va, 0, program->levels[0].shared_nodes[j].node_pipeline_index,
                        &slot_indirect, vk_root_param_buffer_va);
            }
        }
    }

    return true;
}

static bool d3d12_command_list_workgraph_execute_entry_cpu(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *wg_state,
        const struct d3d12_wg_state_object_program *program, const D3D12_NODE_CPU_INPUT *desc,
        VkDeviceAddress vk_root_param_buffer_va)
{
    struct vkd3d_scratch_allocation payload_scratch;
    VkDeviceSize payload_size;
    uint32_t record_size;
    uint32_t node_index;
    unsigned int i;

    if (desc->NumRecords == 0)
        return true;

    if (desc->EntrypointIndex >= program->levels[0].nodes_count)
    {
        ERR("EntryPointIndex %u is out of bounds.\n", desc->EntrypointIndex);
        return true;
    }

    node_index = program->levels[0].nodes[desc->EntrypointIndex];
    /* amdgpu-wddm fork: copy only the bytes of the application's records. record_size does not include the
     * internal word of [NodeTrackRWInputSharing], which an entry point does not use. The last record needs only
     * its own size: a full stride for it can read past the end of the application's array. For an empty record,
     * pRecords and RecordStrideInBytes are ignored (WorkGraphs.md, D3D12_NODE_CPU_INPUT), so nothing is read. */
    record_size = wg_state->entry_points[node_index].node_input->record_size;
    if (!record_size)
        payload_size = 0;
    else if (desc->RecordStrideInBytes == 0)
        payload_size = record_size;
    else
        payload_size = (VkDeviceSize)(desc->NumRecords - 1) * desc->RecordStrideInBytes + record_size;

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD, max(payload_size, 4), 64, &payload_scratch))
        return false;

    /* Alternatively use vkCmdUpdateBuffer, but at least for now, just rely on ReBAR doing its thing. */
    if (payload_size)
        memcpy(payload_scratch.host_ptr, desc->pRecords, payload_size);

    if (!d3d12_command_list_workgraph_execute_node_cpu_entry(list, wg_state, program, node_index, desc,
            d3d12_wg_entry_output_va(list, program), payload_scratch.va,
            vk_root_param_buffer_va))
        return false;

    /* For any shared nodes, just execute those as well. */
    for (i = 0; i < program->levels[0].shared_nodes_count; i++)
    {
        if (program->levels[0].shared_nodes[i].node_payload_index == node_index)
        {
            if (!d3d12_command_list_workgraph_execute_node_cpu_entry(list, wg_state, program,
                    program->levels[0].shared_nodes[i].node_pipeline_index, desc,
                    d3d12_wg_entry_output_va(list, program), payload_scratch.va,
                    vk_root_param_buffer_va))
                return false;
        }
    }

    return true;
}

/* amdgpu-wddm fork: false if a scratch allocation failed. The command list is then marked as out of memory, and
 * the levels after the entry level are not recorded. */
static bool d3d12_command_list_workgraph_execute_entry_level(
        struct d3d12_command_list *list, struct d3d12_wg_state_object *wg_state,
        const struct d3d12_wg_state_object_program *program, const D3D12_DISPATCH_GRAPH_DESC *desc,
        VkDeviceAddress vk_root_param_buffer_va)
{
    unsigned int i;
    switch (desc->Mode)
    {
        case D3D12_DISPATCH_MODE_NODE_CPU_INPUT:
            return d3d12_command_list_workgraph_execute_entry_cpu(
                    list, wg_state, program, &desc->NodeCPUInput,
                    vk_root_param_buffer_va);

        case D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT:
            for (i = 0; i < desc->MultiNodeCPUInput.NumNodeInputs; i++)
            {
                const D3D12_NODE_CPU_INPUT *input;
                input = (const void *)((const uint8_t *)desc->MultiNodeCPUInput.pNodeInputs +
                        desc->MultiNodeCPUInput.NodeInputStrideInBytes * i);
                if (!d3d12_command_list_workgraph_execute_entry_cpu(
                        list, wg_state, program, input,
                        vk_root_param_buffer_va))
                    return false;
            }
            return true;

        case D3D12_DISPATCH_MODE_NODE_GPU_INPUT:
            return d3d12_command_list_workgraph_execute_entry_gpu(
                    list, wg_state, program, desc->NodeGPUInput,
                    vk_root_param_buffer_va);

        case D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT:
            return d3d12_command_list_workgraph_execute_entry_multi_gpu(
                    list, wg_state, program, desc->MultiNodeGPUInput,
                    vk_root_param_buffer_va);

        default:
            /* amdgpu-wddm fork: an error, not a FIXME. The release flavour removes FIXME lines, and this graph
             * then does not run, which a log must show. */
            ERR("Unimplemented mode %u, the graph does not run.\n", desc->Mode);
            return true;
    }
}

/* amdgpu-wddm fork: the round frame of the draining scheduler (option C). After round 1 the engine records
 * program->drain_rounds extra rounds, each in a conditional rendering block on the pending word of the round before.
 * The engine owns conditional rendering from B0 to B6: B0 ends the predicate of SetPredication and keeps its state,
 * and sets enabled_on_command_buffer to false, so that no helper begins the application block inside an engine
 * block (d3d12_command_list_update_conditional_rendering_state returns at once then). Round 1 runs in an engine block
 * on the application's predicate, if there is one. B6 restores the state and begins the application block again
 * if it was active. Only dispatches and barriers go into the blocks: a copy or a fill is not predicated. */
struct d3d12_wg_drain_predication
{
    bool was_active;
    bool enabled_on_command_buffer;
    bool block_resume;
};

static void d3d12_command_list_workgraph_drain_suppress(struct d3d12_command_list *list,
        struct d3d12_wg_drain_predication *saved)
{
    /* was_active first: ending the block sets current_enabled to false. */
    saved->was_active = list->predication.current_enabled;
    d3d12_command_list_workgraph_update_predication(list, true);
    saved->enabled_on_command_buffer = list->predication.enabled_on_command_buffer;
    saved->block_resume = list->cmd.suspend_resume.block_resume;
    list->predication.enabled_on_command_buffer = false;
}

static void d3d12_command_list_workgraph_drain_restore(struct d3d12_command_list *list,
        const struct d3d12_wg_drain_predication *saved)
{
    list->predication.enabled_on_command_buffer = saved->enabled_on_command_buffer;
    list->cmd.suspend_resume.block_resume = saved->block_resume;
    if (saved->was_active)
        d3d12_command_list_workgraph_update_predication(list, false);
}

static void d3d12_command_list_workgraph_drain_barrier(struct d3d12_command_list *list)
{
    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
            VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT);
}

static void d3d12_command_list_workgraph_emit_drain(struct d3d12_command_list *list,
        const struct d3d12_wg_state_object *state, const struct d3d12_wg_state_object_program *program,
        enum vkd3d_workgraph_drain_mode mode, uint32_t round)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_workgraph_drain_args args;

    memset(&args, 0, sizeof(args));
    args.control_va = list->wg_state.BackingMemory.StartAddress + program->drain_control_scratch_offset;
    args.abandoned_va = state->abandoned_va;
    args.mode = mode;
    args.round = round;
    args.rounds = program->drain_rounds;
    args.pending_rounds = program->drain_pending;
    /* "drain-stall:<r>" makes round r a round without progress. */
    args.stall = mode == VKD3D_WORKGRAPH_DRAIN_MODE_PENDING && d3d12_wg_fault_injected("drain-stall", round);
    if (program->pool_execution)
    {
        args.pool_states_va = list->wg_state.BackingMemory.StartAddress + program->pool_state_scratch_offset;
        args.pools = program->num_levels;
        args.pool_state_words = program->pool_state_stride / sizeof(uint32_t);
    }

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            program->drain.vk_pipeline));
    d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer, program->drain.vk_pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, sizeof(args), &args);
    VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, 1, 1, 1));
    VKD3D_BREADCRUMB_TAG("drain");
    VKD3D_BREADCRUMB_AUX32(mode);
    VKD3D_BREADCRUMB_AUX32(round);
    VKD3D_BREADCRUMB_COMMAND(WORKGRAPH_META);
}

static void d3d12_command_list_workgraph_drain_begin(struct d3d12_command_list *list,
        struct d3d12_wg_state_object *state, VkBuffer vk_buffer, VkDeviceSize offset)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    VkConditionalRenderingBeginInfoEXT begin_info;

    /* "drain-hook" tries to begin the application block inside the engine scope, as a helper could. */
    if (d3d12_wg_fault_injected("drain-hook", UINT_MAX))
    {
        vkd3d_atomic_uint32_increment(&state->drain_hook_calls, vkd3d_memory_order_relaxed);
        d3d12_command_list_workgraph_update_predication(list, false);
    }

    /* Conditional rendering blocks do not nest. An active application block here is an engine defect. */
    if (list->predication.current_enabled)
    {
        ERR("The predicate of SetPredication is active in a work graph round, ending it.\n");
        vkd3d_atomic_uint32_increment(&state->drain_violations, vkd3d_memory_order_relaxed);
        d3d12_command_list_workgraph_update_predication(list, true);
    }

    begin_info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
    begin_info.pNext = NULL;
    begin_info.buffer = vk_buffer;
    begin_info.offset = offset;
    begin_info.flags = 0;
    VK_CALL(vkCmdBeginConditionalRenderingEXT(list->cmd.vk_command_buffer, &begin_info));
}

static void d3d12_command_list_workgraph_drain_end(struct d3d12_command_list *list)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;

    VK_CALL(vkCmdEndConditionalRenderingEXT(list->cmd.vk_command_buffer));
}

/* The end of round 1 and the extra rounds, then close. Round r (round 1 is r = 0) ends with the pending dispatch
 * that writes pending[r]. Extra round r runs levels 1 to num_levels - 1 under pending[r - 1]; the entry level runs only
 * in round 1. With record pools, round r uses half r & 1 of every pool, and an extra round begins with the injection
 * of the records of pool 1 that round r - 1 retained. */
static void d3d12_command_list_workgraph_drain_rounds(struct d3d12_command_list *list,
        struct d3d12_wg_state_object *state, const struct d3d12_wg_state_object_program *program,
        const struct vkd3d_unique_resource *backing, VkDeviceAddress vk_root_parameter_buffer_va,
        const struct d3d12_wg_pool_dispatch *pools, bool *block)
{
    VkDeviceSize control_offset;
    uint32_t r, i, half;

    control_offset = list->wg_state.BackingMemory.StartAddress - backing->va + program->drain_control_scratch_offset;

    d3d12_command_list_workgraph_emit_drain(list, state, program, VKD3D_WORKGRAPH_DRAIN_MODE_PENDING, 0);
    if (*block)
    {
        d3d12_command_list_workgraph_drain_end(list);
        *block = false;
    }

    for (r = 1; r <= program->drain_rounds; r++)
    {
        /* B3: the pending word for the predicate, and the state of the round before for the round. */
        d3d12_command_list_workgraph_drain_barrier(list);
        d3d12_command_list_workgraph_drain_begin(list, state, backing->vk_buffer,
                control_offset + offsetof(struct vkd3d_workgraph_drain_control, pending) +
                (r - 1) * sizeof(uint32_t));
        half = r & 1;
        if (pools && program->num_levels > 1)
        {
            d3d12_command_list_workgraph_emit_pool_pass(list, program, d3d12_wg_pool_level_va(program, pools, 0, half),
                    VKD3D_WORKGRAPH_POOL_MODE_RETAIN, 0);
        }
        for (i = 1; i < program->num_levels; i++)
        {
            if (pools)
            {
                d3d12_command_list_workgraph_execute_level(list, state, program, i,
                        i + 1 < program->num_levels ? d3d12_wg_pool_payload_va(list, pools, i + 1, half) : 0,
                        d3d12_wg_pool_payload_va(list, pools, i, half), vk_root_parameter_buffer_va,
                        d3d12_wg_pool_level_va(program, pools, i, half));
            }
            else
            {
                d3d12_command_list_workgraph_execute_level(list, state, program, i,
                        d3d12_wg_payload_va(list, program, i & 1), d3d12_wg_payload_va(list, program, (i & 1) ^ 1),
                        vk_root_parameter_buffer_va, 0);
            }
        }
        d3d12_command_list_workgraph_emit_drain(list, state, program, VKD3D_WORKGRAPH_DRAIN_MODE_PENDING, r);
        d3d12_command_list_workgraph_drain_end(list);
    }

    /* B4, close outside every block, then B5 for the next DispatchGraph on this backing memory. */
    d3d12_command_list_workgraph_barrier(list, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    d3d12_command_list_workgraph_emit_drain(list, state, program, VKD3D_WORKGRAPH_DRAIN_MODE_CLOSE, 0);
    d3d12_command_list_workgraph_drain_barrier(list);
}

void d3d12_command_list_workgraph_dispatch(struct d3d12_command_list *list, const D3D12_DISPATCH_GRAPH_DESC *desc)
{
    struct d3d12_wg_drain_predication drain_predication;
    const struct vkd3d_unique_resource *backing = NULL;
    struct d3d12_wg_pool_dispatch pool_dispatch;
    const struct d3d12_wg_pool_dispatch *pools = NULL;
    const struct d3d12_wg_state_object_program *program;
    struct vkd3d_scratch_allocation root_param_scratch;
    struct d3d12_wg_state_object *wg_state;
    uint32_t wg_state_program_index;
    bool block = false;
    uint32_t i;

    memset(&drain_predication, 0, sizeof(drain_predication));
    wg_state = (struct d3d12_wg_state_object *)(uintptr_t)list->wg_state.ProgramIdentifier.OpaqueData[1];
    wg_state_program_index = (uint32_t)list->wg_state.ProgramIdentifier.OpaqueData[0];

    if (!wg_state)
    {
        WARN("WG state is not set.\n");
        return;
    }

    d3d12_wg_scratch_fault_reset();
    d3d12_wg_state_object_report_lost_records(wg_state);
    d3d12_wg_state_object_report_abandoned(wg_state);

    d3d12_command_list_end_current_render_pass(list, false);
    d3d12_command_list_debug_mark_begin_region(list, "WGDispatch");

    d3d12_command_list_invalidate_current_pipeline(list, true);
    d3d12_command_list_update_global_descriptor_heap(list);

    d3d12_command_list_workgraph_barrier(list,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    program = &wg_state->programs[wg_state_program_index];

    /* amdgpu-wddm fork: the execution state, the payload rings included, is in the backing memory. With less
     * memory than GetWorkGraphMemoryRequirements() asks for, the graph would write past it, so it does not run. */
    if (list->wg_state.BackingMemory.SizeInBytes < program->required_scratch_size)
    {
        ERR("Backing memory is not large enough (%"PRIu64" < %"PRIu64"), the graph does not run.\n",
                list->wg_state.BackingMemory.SizeInBytes, program->required_scratch_size);
        goto out;
    }

    /* amdgpu-wddm fork: with record pools, the backing memory must have a pool layout (Min of the pools). */
    if (program->pool_execution)
    {
        if (list->wg_state.BackingMemory.SizeInBytes < program->pool_min_size)
        {
            ERR("Backing memory is less than Min of the record pools (%"PRIu64" < %"PRIu64"), the graph does not run.\n",
                    list->wg_state.BackingMemory.SizeInBytes, program->pool_min_size);
            goto out;
        }
        if (!d3d12_command_list_workgraph_prepare_pools(list, program, &pool_dispatch))
            goto out;
        pools = &pool_dispatch;
    }

    if (!d3d12_command_list_workgraph_allocate_scratch(list,
            VKD3D_SCRATCH_POOL_KIND_UNIFORM_UPLOAD,
            sizeof(union vkd3d_root_parameter_data), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
            &root_param_scratch))
        goto out;

    d3d12_command_list_fetch_root_parameter_data(list, &list->compute_bindings, root_param_scratch.host_ptr);
    /* TODO: For mesh nodes, may have to fetch graphics root parameter data. */

    if (program->drain_rounds && !(backing = vkd3d_va_map_deref(&list->device->memory_allocator.va_map,
            list->wg_state.BackingMemory.StartAddress)))
        WARN("No resource at the backing memory, running round 1 only.\n");
    /* The pools need the round frame: its init clears the pool states. */
    if (pools && !backing)
    {
        ERR("No resource at the backing memory, the graph does not run on the record pools.\n");
        goto out;
    }

    if (backing)
    {
        /* B0, init, B1, then round 1 in a block on the application's predicate. */
        d3d12_command_list_workgraph_drain_suppress(list, &drain_predication);
        d3d12_command_list_workgraph_emit_drain(list, wg_state, program, VKD3D_WORKGRAPH_DRAIN_MODE_INIT, 0);
        d3d12_command_list_workgraph_drain_barrier(list);
        if (drain_predication.enabled_on_command_buffer)
        {
            d3d12_command_list_workgraph_drain_begin(list, wg_state, list->predication.vk_buffer,
                    list->predication.vk_buffer_offset);
            block = true;
        }
    }

    /* With record pools, the injection pass of the entry level writes the output guard of pool 1, half 0. Nothing is
     * retained before round 1, so it injects no records. */
    if (pools && program->num_levels > 1)
    {
        d3d12_command_list_workgraph_emit_pool_pass(list, program, d3d12_wg_pool_level_va(program, pools, 0, 0),
                VKD3D_WORKGRAPH_POOL_MODE_RETAIN, 0);
    }

    if (!d3d12_command_list_workgraph_execute_entry_level(list, wg_state, program, desc,
            root_param_scratch.va))
        goto out;

    for (i = 1; i < program->num_levels; i++)
    {
        if (pools)
        {
            d3d12_command_list_workgraph_execute_level(list, wg_state, program, i,
                    i + 1 < program->num_levels ? d3d12_wg_pool_payload_va(list, pools, i + 1, 0) : 0,
                    d3d12_wg_pool_payload_va(list, pools, i, 0), root_param_scratch.va,
                    d3d12_wg_pool_level_va(program, pools, i, 0));
        }
        else
        {
            d3d12_command_list_workgraph_execute_level(list, wg_state, program, i,
                    d3d12_wg_payload_va(list, program, i & 1), d3d12_wg_payload_va(list, program, (i & 1) ^ 1),
                    root_param_scratch.va, 0);
        }
    }

    if (backing)
        d3d12_command_list_workgraph_drain_rounds(list, wg_state, program, backing, root_param_scratch.va, pools, &block);

out:
    /* After a failure the list does not execute; the block still ends, and B6 restores the predicate state. */
    if (block)
        d3d12_command_list_workgraph_drain_end(list);
    if (backing)
        d3d12_command_list_workgraph_drain_restore(list, &drain_predication);
    d3d12_command_list_debug_mark_end_region(list);
}
