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
#include "d3d12_crosstest.h"

/* amdgpu-wddm fork: the count arithmetic of the work graph GPU input passes, for
 * test_workgraph_multi_gpu_input_count_math. */
#define WG_MATH_FN static inline
#define WG_MATH_U32 uint32_t
#define WG_MATH_U64 uint64_t
#include "../libs/vkd3d/shaders/cs_workgraph_multi_gpu_input_math.h"

struct test_context_workgraph
{
    struct test_context context;
    ID3D12GraphicsCommandList10 *list;
    ID3D12Device5 *device;

    ID3D12RootSignature *default_root_uav_rs;
};

static bool export_strequal(LPCWSTR a, LPCWSTR b)
{
    if (!a || !b)
        return false;

    while (*a != '\0' && *b != '\0')
    {
        if (*a != *b)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool init_workgraph_test_context(struct test_context_workgraph *context)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21;
    D3D12_ROOT_SIGNATURE_DESC rs_desc;
    D3D12_ROOT_PARAMETER rs_param[2];

    if (!init_compute_test_context(&context->context))
        return false;

    options21.WorkGraphsTier = D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;

    if (FAILED(ID3D12Device_CheckFeatureSupport(context->context.device, D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21))) ||
        options21.WorkGraphsTier != D3D12_WORK_GRAPHS_TIER_1_0)
    {
        skip("Workgraphs not supported.\n");
        destroy_test_context(&context->context);
        return false;
    }

    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(context->context.list, &IID_ID3D12GraphicsCommandList10, (void **)&context->list)))
    {
        skip("GCLExperimental not supported.\n");
        destroy_test_context(&context->context);
        return false;
    }

    memset(&rs_desc, 0, sizeof(rs_desc));
    memset(rs_param, 0, sizeof(rs_param));
    rs_desc.NumParameters = ARRAY_SIZE(rs_param);
    rs_desc.pParameters = rs_param;
    rs_param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rs_param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rs_param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rs_param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rs_param[1].Descriptor.ShaderRegister = 1;
    create_root_signature(context->context.device, &rs_desc, &context->default_root_uav_rs);

    ID3D12Device_QueryInterface(context->context.device, &IID_ID3D12Device5, (void **)&context->device);

    return true;
}

static void destroy_workgraph_test_context(struct test_context_workgraph *context)
{
    ID3D12GraphicsCommandList10_Release(context->list);
    ID3D12Device5_Release(context->device);
    ID3D12RootSignature_Release(context->default_root_uav_rs);
    destroy_test_context(&context->context);
}

#include "shaders/workgraph/headers/basic.h"
#include "shaders/workgraph/headers/broadcast_custom_input_uint2.h"
#include "shaders/workgraph/headers/broadcast_custom_input_uint.h"
#include "shaders/workgraph/headers/broadcast_custom_input_u16_2.h"
#include "shaders/workgraph/headers/broadcast_custom_input.h"
#include "shaders/workgraph/headers/thread_custom_input.h"
#include "shaders/workgraph/headers/coalesce_custom_input.h"
#include "shaders/workgraph/headers/two_entry.h"
#include "shaders/workgraph/headers/rw_entry_input.h"
#include "shaders/workgraph/headers/depth_limit_32.h"
#include "shaders/workgraph/headers/depth_limit_33.h"
#include "shaders/workgraph/headers/lrat_order.h"
#include "shaders/workgraph/headers/sparse_self_recursion.h"
#include "shaders/workgraph/headers/scalar_self_recursion.h"
#include "shaders/workgraph/headers/empty_entry.h"
#include "shaders/workgraph/headers/track_rw_entry.h"
#include "shaders/workgraph/headers/multi_input_count.h"
#include "shaders/workgraph/headers/level_count.h"
#include "shaders/workgraph/headers/output_bounds.h"
#include "shaders/workgraph/headers/output_counts.h"
#include "shaders/workgraph/headers/output_index_bounds.h"
#include "shaders/workgraph/headers/output_zero_counts.h"
#include "shaders/workgraph/headers/drain_dispatch.h"
#include "shaders/workgraph/headers/level_sinks.h"
#include "shaders/workgraph/headers/empty_count64.h"

/* is_vkd3d_proton_device() is always false in the Windows build of the tests, also with the vkd3d-proton DLLs next
 * to the executable. This asks the device for the vkd3d-proton interop interface. */
static bool workgraph_is_vkd3d_proton(ID3D12Device5 *device)
{
    IUnknown *interop;

    if (FAILED(ID3D12Device5_QueryInterface(device, &IID_ID3D12DXVKInteropDevice, (void **)&interop)))
        return false;
    IUnknown_Release(interop);
    return true;
}

/* The stride 0 cases are skipped on AMD because they hang the GPU with AMD's own driver. A different driver with
 * the AMD vendor ID can run them: set VKD3D_TEST_WORKGRAPH_AMD_STRIDE_0=1. */
static bool workgraph_skip_amd_stride_0(ID3D12Device *device)
{
    const char *env = getenv("VKD3D_TEST_WORKGRAPH_AMD_STRIDE_0");

    return is_amd_windows_device(device) && !(env && env[0] == '1');
}

static void check_work_graph_properties(ID3D12StateObject *pso,
        LPCWSTR expected_program_name, LPCWSTR expected_entry_node, LPCWSTR expected_leaf_node,
        unsigned int expected_input_record_size,
        D3D12_PROGRAM_IDENTIFIER *ident, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *reqs)
{
    ID3D12StateObjectProperties1 *props1;
    ID3D12WorkGraphProperties *props;
    LPCWSTR program_name;
    D3D12_NODE_ID id;
    UINT num_graphs;
    HRESULT hr;
    UINT index;
    UINT size;

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query work graph props, hr #%x.\n", (int)hr);

    if (SUCCEEDED(hr))
    {
        hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12StateObjectProperties1, (void **)&props1);
        ok(SUCCEEDED(hr), "Failed to query SOP1.\n");
    }
    else
        return;

    num_graphs = ID3D12WorkGraphProperties_GetNumWorkGraphs(props);
    ok(num_graphs == 1, "Expected 1 graph, got %u.\n", num_graphs);
    program_name = ID3D12WorkGraphProperties_GetProgramName(props, 0);
    ok(export_strequal(program_name, expected_program_name), "Unexpected program name.\n");
    index = ID3D12WorkGraphProperties_GetWorkGraphIndex(props, expected_program_name);
    ok(index == 0, "Unexpected index %u\n", index);
    index = ID3D12WorkGraphProperties_GetWorkGraphIndex(props, u"DummyDoesNotExist");
    ok(index == UINT32_MAX, "Unexpected index %u\n", index);

    /* There is supposed to be a stable order of nodes, which of course is not documented ... */
    id = ID3D12WorkGraphProperties_GetNodeID(props, 0, 0);
    todo_if(ID3D12WorkGraphProperties_GetNumNodes(props, 0) > 1)
    ok(export_strequal(id.Name, expected_entry_node), "Unexpected node name.\n");
    todo_if(ID3D12WorkGraphProperties_GetNumNodes(props, 0) > 1)
    ok(id.ArrayIndex == 0, "Unexpected ArrayIndex %u.\n", id.ArrayIndex);

    if (expected_leaf_node)
    {
        id = ID3D12WorkGraphProperties_GetNodeID(props, 0, 0);
        todo_if(ID3D12WorkGraphProperties_GetNumNodes(props, 0) > 1)
        ok(export_strequal(id.Name, expected_leaf_node), "Unexpected node name.\n");
        ok(id.ArrayIndex == 0, "Unexpected ArrayIndex %u.\n", id.ArrayIndex);
    }

    /* We have a MaxDispatch property, so SV_DispatchGrid is part of the record implicitly. */
    size = ID3D12WorkGraphProperties_GetEntrypointRecordSizeInBytes(props, 0, 0);
    ok(size == expected_input_record_size, "Unexpected size %u.\n", size);

    *ident = ID3D12StateObjectProperties1_GetProgramIdentifier(props1, program_name);
    ok(ident->OpaqueData[0] || ident->OpaqueData[1] || ident->OpaqueData[2] || ident->OpaqueData[3],
        "Program identifier is NULL unexpectedly.\n");

    /* OOB writes 0 size. */
    memset(reqs, 0xff, sizeof(*reqs));
    ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 1, reqs);
    ok(reqs->MaxSizeInBytes == 0, "MaxSizeInBytes is not 0.\n");
    ok(reqs->MinSizeInBytes == 0, "MinSizeInBytes is not 0.\n");
    ok(reqs->SizeGranularityInBytes == 0, "Granularity is not 0.\n");

    memset(reqs, 0, sizeof(*reqs));
    ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 0, reqs);
    ok(reqs->MaxSizeInBytes >= reqs->MinSizeInBytes, "MinSize > MaxSize\n");

    ID3D12StateObjectProperties1_Release(props1);
    ID3D12WorkGraphProperties_Release(props);
}

static ID3D12StateObject *create_workgraph_pso(
        struct test_context_workgraph *context,
        D3D12_SHADER_BYTECODE code, LPCWSTR program_name,
        ID3D12RootSignature *rs)
{
    D3D12_GLOBAL_ROOT_SIGNATURE grs_desc;
    D3D12_STATE_SUBOBJECT subobjects[3];
    D3D12_STATE_OBJECT_DESC pso_desc;
    D3D12_WORK_GRAPH_DESC wg_desc;
    D3D12_DXIL_LIBRARY_DESC dxil;
    ID3D12StateObject *pso;
    HRESULT hr;

    memset(&pso_desc, 0, sizeof(pso_desc));
    memset(subobjects, 0, sizeof(subobjects));
    memset(&dxil, 0, sizeof(dxil));
    memset(&wg_desc, 0, sizeof(wg_desc));
    pso_desc.NumSubobjects = ARRAY_SIZE(subobjects);
    pso_desc.pSubobjects = subobjects;
    pso_desc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    grs_desc.pGlobalRootSignature = rs;

    subobjects[0].pDesc = &dxil;
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = &wg_desc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    subobjects[2].pDesc = &grs_desc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

    dxil.DXILLibrary = code;
    wg_desc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    wg_desc.ProgramName = program_name;

    hr = ID3D12Device5_CreateStateObject(context->device, &pso_desc, &IID_ID3D12StateObject, (void **)&pso);
    ok(SUCCEEDED(hr), "Failed to create state object, hr #%x.\n", (int)hr);
    if (FAILED(hr))
        return NULL;

    return pso;
}

struct workgraph_test_desc
{
    unsigned int entry_point_index;
    unsigned int num_records;
    unsigned int record_stride;
    unsigned int record_size;
    unsigned int num_graph_dispatches;
    unsigned int num_multi_instances;
    D3D12_DISPATCH_MODE mode;
    const void *records;
    unsigned int num_threads[3];
    bool is_bug;

    uint32_t (*expected_cb)(const struct workgraph_test_desc *desc, uint32_t i);
};

static uint32_t basic_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t x, y, z, record_index;
    uint32_t expected = 0;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;

    for (record_index = 0; record_index < desc->num_records; record_index++)
    {
        if (x < records[0] * desc->num_threads[0] &&
            y < records[1] * desc->num_threads[1] &&
            z < records[2] * desc->num_threads[2])
        {
            expected += desc->num_multi_instances * desc->num_graph_dispatches;
        }
        records += desc->record_stride / sizeof(uint32_t);
    }
    return expected;
}

static uint32_t broadcast_input_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t x, y, z, record_index;
    uint32_t expected = 0;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;

    for (record_index = 0; record_index < desc->num_records; record_index++)
    {
        if (x < 3 * desc->num_threads[0] &&
            y < 3 * desc->num_threads[1] &&
            z < 2 * desc->num_threads[2])
        {
            expected += records[0] ^ records[1];
        }
        records += desc->record_stride / sizeof(uint32_t);
    }
    return expected;
}

static uint32_t broadcast_input_uint2_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t x, y, z, record_index;
    uint32_t expected = 0;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;

    for (record_index = 0; record_index < desc->num_records; record_index++)
    {
        if (x < records[1] * desc->num_threads[0] &&
            y < records[2] * desc->num_threads[1] &&
            z < desc->num_threads[2])
        {
            expected += records[0] ^ records[3];
        }
        records += desc->record_stride / sizeof(uint32_t);
    }
    return expected;
}

static uint32_t broadcast_input_uint16x2_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t x, y, z, record_index;
    uint32_t expected = 0;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;

    for (record_index = 0; record_index < desc->num_records; record_index++)
    {
        if (x < (records[1] & 0xffff) * desc->num_threads[0] &&
            y < (records[1] >> 16) * desc->num_threads[1] &&
            z < desc->num_threads[2])
        {
            expected += records[0] ^ records[2];
        }
        records += desc->record_stride / sizeof(uint32_t);
    }
    return expected;
}

static uint32_t broadcast_input_uint_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t x, y, z, record_index;
    uint32_t expected = 0;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;

    for (record_index = 0; record_index < desc->num_records; record_index++)
    {
        if (x < records[1] * desc->num_threads[0] &&
            y < desc->num_threads[1] &&
            z < desc->num_threads[2])
        {
            expected += records[0] ^ records[2];
        }
        records += desc->record_stride / sizeof(uint32_t);
    }
    return expected;
}

static uint32_t thread_input_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t i;

    for (i = 0; i < desc->num_records; i++)
        if (records[2 * i] == value_index)
            return records[2 * i + 1];

    return 0;
}

static uint32_t coalesced_input_expected(const struct workgraph_test_desc *desc, uint32_t value_index)
{
    const uint32_t *records = desc->records;
    uint32_t i;

    for (i = 0; i < desc->num_records; i++)
    {
        uint32_t offset = records[2 * i];
        if (value_index >= offset && value_index < offset + desc->num_threads[0])
            return records[2 * i + 1];
    }

    return 0;
}

static void execute_workgraph_pso_simple(struct test_context_workgraph *context,
        ID3D12StateObject *pso, const D3D12_PROGRAM_IDENTIFIER *ident,
        const D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *wg_reqs,
        const void *node_payload, size_t node_payload_stride, size_t node_payload_count,
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *local_root_table,
        ID3D12Resource **scratch_output)
{
    ID3D12Resource *scratch = *scratch_output;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    D3D12_SET_PROGRAM_DESC program_desc;

    if (wg_reqs->MinSizeInBytes)
    {
        if (!scratch)
        {
            scratch = create_default_buffer(context->context.device, wg_reqs->MinSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    else
        scratch = NULL;

    memset(&program_desc, 0, sizeof(program_desc));
    program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    if (scratch)
    {
        program_desc.WorkGraph.BackingMemory.SizeInBytes = wg_reqs->MinSizeInBytes;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
    }

    if (local_root_table)
        program_desc.WorkGraph.NodeLocalRootArgumentsTable = *local_root_table;

    /* Only needs to be called once after memory has been allocated / clobbered by another graph.
     * Unclear if SetProgram itself initializes the work graph memory,
     * or if that is deferred to DispatchGraph time. */
    program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    program_desc.WorkGraph.ProgramIdentifier = *ident;
    ID3D12GraphicsCommandList10_SetProgram(context->list, &program_desc);

    dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch_desc.NodeCPUInput.EntrypointIndex = 0;
    dispatch_desc.NodeCPUInput.NumRecords = node_payload_count;
    dispatch_desc.NodeCPUInput.pRecords = (void *)node_payload;
    dispatch_desc.NodeCPUInput.RecordStrideInBytes = node_payload_stride;

    ID3D12GraphicsCommandList10_DispatchGraph(context->list, &dispatch_desc);
    *scratch_output = scratch;
}

static void execute_workgraph_test(struct test_context_workgraph *context,
        ID3D12StateObject *pso, const D3D12_PROGRAM_IDENTIFIER *ident,
        const D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *wg_reqs, const struct workgraph_test_desc *desc)
{
    D3D12_MULTI_NODE_GPU_INPUT multi_gpu_input;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    D3D12_SET_PROGRAM_DESC program_desc;
    D3D12_NODE_CPU_INPUT node_cpu_input;
    D3D12_NODE_GPU_INPUT node_gpu_input;
    ID3D12Resource *input_payload;
    ID3D12Resource *input_multi;
    struct resource_readback rb;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12Resource *input;
    unsigned int i;

    if (wg_reqs->MinSizeInBytes)
    {
        scratch = create_default_buffer(context->context.device, wg_reqs->MinSizeInBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    else
        scratch = NULL;

    output = create_default_buffer(context->context.device, 1024 * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    ID3D12GraphicsCommandList_SetComputeRootSignature(
        context->context.list, context->default_root_uav_rs);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context->context.list, 0,
        ID3D12Resource_GetGPUVirtualAddress(output));

    memset(&program_desc, 0, sizeof(program_desc));
    program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    if (scratch)
    {
        program_desc.WorkGraph.BackingMemory.SizeInBytes = wg_reqs->MinSizeInBytes;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
    }

    /* Only needs to be called once after memory has been allocated / clobbered by another graph.
     * Unclear if SetProgram itself initializes the work graph memory,
     * or if that is deferred to DispatchGraph time. */
    program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    program_desc.WorkGraph.ProgramIdentifier = *ident;
    ID3D12GraphicsCommandList10_SetProgram(context->list, &program_desc);

    /* 0 stride to duplicate is allowed. Use this to easily test multi-dispatch. */

    input_payload = create_upload_buffer(context->context.device, desc->record_size, desc->records);
    node_gpu_input.EntrypointIndex = desc->entry_point_index;
    /* Value > 1 breaks on WARP. It's treated as just 1. */
    node_gpu_input.NumRecords = desc->num_records;
    node_gpu_input.Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(input_payload);
    node_gpu_input.Records.StrideInBytes = desc->record_stride;
    input = create_upload_buffer(context->context.device, sizeof(node_gpu_input), &node_gpu_input);

    multi_gpu_input.NumNodeInputs = desc->num_multi_instances;
    multi_gpu_input.NodeInputs.StartAddress = ID3D12Resource_GetGPUVirtualAddress(input);
    multi_gpu_input.NodeInputs.StrideInBytes = 0;
    input_multi = create_upload_buffer(context->context.device, sizeof(multi_gpu_input), &multi_gpu_input);

    /* Apparently the spec allows for back-to-back dispatch graphs without barrier,
     * even if the scratch buffer is the same. The thing that is explicitly dis-allowed
     * is using same scratch across multiple queues. */
    node_cpu_input.EntrypointIndex = desc->entry_point_index;
    node_cpu_input.NumRecords = desc->num_records;
    node_cpu_input.pRecords = (void *)desc->records;
    node_cpu_input.RecordStrideInBytes = desc->record_stride;

    dispatch_desc.Mode = desc->mode;

    switch (desc->mode)
    {
        case D3D12_DISPATCH_MODE_NODE_CPU_INPUT:
            dispatch_desc.NodeCPUInput = node_cpu_input;
            for (i = 0; i < desc->num_graph_dispatches; i++)
                ID3D12GraphicsCommandList10_DispatchGraph(context->list, &dispatch_desc);
            break;

        case D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT:
            dispatch_desc.MultiNodeCPUInput.pNodeInputs = &node_cpu_input;
            dispatch_desc.MultiNodeCPUInput.NumNodeInputs = desc->num_multi_instances;
            dispatch_desc.MultiNodeCPUInput.NodeInputStrideInBytes = 0;
            for (i = 0; i < desc->num_graph_dispatches; i++)
                ID3D12GraphicsCommandList10_DispatchGraph(context->list, &dispatch_desc);
            break;

        case D3D12_DISPATCH_MODE_NODE_GPU_INPUT:
            dispatch_desc.NodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(input);
            for (i = 0; i < desc->num_graph_dispatches; i++)
                ID3D12GraphicsCommandList10_DispatchGraph(context->list, &dispatch_desc);
            break;

        case D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT:
            dispatch_desc.MultiNodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(input_multi);
            for (i = 0; i < desc->num_graph_dispatches; i++)
                ID3D12GraphicsCommandList10_DispatchGraph(context->list, &dispatch_desc);
            break;

        default:
            break;
    }

    transition_resource_state(context->context.list, output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
            context->context.queue, context->context.list);
    reset_command_list(context->context.list, context->context.allocator);

    for (i = 0; i < 1024; i++)
    {
        uint32_t expected;
        uint32_t v;

        expected = desc->expected_cb(desc, i);
        v = get_readback_uint(&rb, i, 0, 0);

        bug_if(desc->is_bug)
        ok(expected == v, "Value %u: expected %u, got %u.\n", i, expected, v);
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(input_payload);
    ID3D12Resource_Release(input_multi);
    ID3D12Resource_Release(input);
    ID3D12Resource_Release(output);
}

void test_workgraph_basic(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12StateObject *pso;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, basic_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL,
        12 /* implied struct with uint3 SV_DispatchGrid*/, &ident, &wg_reqs);

    /* Test various ways to dispatch a basic broadcast node. */

    {
        const uint32_t records[] = { 1, 2, 2, 0, 2, 1, 1, 0 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 3;
        desc.num_multi_instances = 1;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 0;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        if (workgraph_skip_amd_stride_0(context.context.device))
        {
            skip("Skipping stride 0 test. Hangs AMD GPU.\n");
        }
        else
        {
            vkd3d_test_set_context("CPU Input, stride 0");
            execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        }
        desc.record_stride = 16;
        vkd3d_test_set_context("CPU Input, stride 16");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    if (!workgraph_skip_amd_stride_0(context.context.device))
    {
        uint32_t records[] = { 1, 2, 2 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = 0;
        desc.record_size = 12;
        desc.record_stride = 0;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("Empty CPU Input - num records");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.num_records = 10;
        records[1] = 0;
        vkd3d_test_set_context("Empty CPU Input - empty workgroup");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }
    else
    {
        skip("Skipping stride 0 tests. Hangs AMD GPU.\n");
    }

    if (!workgraph_skip_amd_stride_0(context.context.device))
    {
        const uint32_t records[] = { 1, 2, 2, 2, 1, 1 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 4;
        desc.num_multi_instances = 3;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 12;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("Multi CPU Input");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }
    else
    {
        skip("Skipping GPU hang on AMD GPU due to stride = 0.\n");
    }

    {
        const uint32_t records[] = { 2, 1, 2, 2, 2, 2, 2, 2, 1 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        desc.num_graph_dispatches = 4;
        desc.num_multi_instances = 1;
        desc.num_records = 1;
        desc.record_size = sizeof(records);
        desc.record_stride = 0;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("GPU Input - single");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.num_records = 0;
        vkd3d_test_set_context("GPU Input - empty");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.num_records = 3;

        /* Bugged on WARP. */
        desc.is_bug = use_warp_device;
        vkd3d_test_set_context("GPU Input - multi");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.record_stride = 12;
        vkd3d_test_set_context("GPU Input - multi + stride");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    {
        const uint32_t records[] = { 2, 1, 2, 2, 2, 2, 2, 2, 1 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        desc.num_graph_dispatches = 4;
        desc.num_multi_instances = 2;
        desc.num_records = 1;
        desc.record_size = sizeof(records);
        desc.record_stride = 0;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("Multi GPU Input - single");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.num_records = 0;
        vkd3d_test_set_context("Multi GPU Input - empty");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.num_records = 3;

        /* Bugged on WARP. */
        desc.is_bug = use_warp_device;
        vkd3d_test_set_context("Multi GPU Input - multi");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.record_stride = 12;
        vkd3d_test_set_context("Multi GPU Input - multi + stride");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
    vkd3d_test_set_context(NULL);
}

/* Separate from test_workgraph_basic, so that it also runs on a native driver that fails the empty CPU input cases there. */
void test_workgraph_gpu_input_stride(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12StateObject *pso;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, basic_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL,
        12 /* implied struct with uint3 SV_DispatchGrid*/, &ident, &wg_reqs);

    {
        /* Records that are not packed: a 12 byte record at a stride of 16 bytes. The fourth uint is padding. */
        const uint32_t records[] = { 2, 1, 2, 7, 2, 2, 2, 7, 1, 2, 1, 7 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = basic_expected;
        desc.num_graph_dispatches = 2;
        desc.num_multi_instances = 1;
        desc.num_records = 3;
        desc.record_size = sizeof(records);
        desc.record_stride = 16;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;
        /* Bugged on WARP. */
        desc.is_bug = use_warp_device;

        desc.mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        vkd3d_test_set_context("GPU Input - multi + stride 16");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        desc.mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        desc.num_multi_instances = 2;
        vkd3d_test_set_context("Multi GPU Input - multi + stride 16");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
    vkd3d_test_set_context(NULL);
}

static uint32_t two_entry_grid_covers(const uint32_t *grid, uint32_t value_index)
{
    uint32_t x, y, z;

    x = value_index % 10;
    y = (value_index / 10) % 10;
    z = value_index / 100;
    return x < grid[0] * 2 && y < grid[1] * 3 && z < grid[2] * 4;
}

void test_workgraph_multi_node_gpu_input_entries(void)
{
    /* One D3D12_MULTI_NODE_GPU_INPUT with inputs to two different entry points. EntryA adds 1 and EntryB adds
     * 0x10000 per thread. The inputs have different record strides (16 with padding, 12, and 0 to repeat one record),
     * and the input array itself has a stride of 32 bytes for a 24 byte D3D12_NODE_GPU_INPUT. */
    static const uint32_t records_a[] = { 1, 2, 2, 9, 2, 1, 1, 9 };
    static const uint32_t records_b[] = { 2, 2, 2, 1, 1, 1, 2, 1, 2 };
    static const D3D12_NODE_ID entry_a_id = { u"EntryA", 0 };
    static const D3D12_NODE_ID entry_b_id = { u"EntryB", 0 };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    ID3D12StateObjectProperties1 *props1 = NULL;
    D3D12_MULTI_NODE_GPU_INPUT multi_gpu_input;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    ID3D12WorkGraphProperties *props = NULL;
    D3D12_SET_PROGRAM_DESC program_desc;
    D3D12_NODE_GPU_INPUT node_inputs[3];
    uint8_t input_array[3 * 32];
    ID3D12Resource *records_buffer;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    ID3D12Resource *scratch = NULL;
    ID3D12Resource *input_multi;
    struct resource_readback rb;
    D3D12_PROGRAM_IDENTIFIER ident;
    UINT index_a, index_b, num_inputs;
    ID3D12Resource *output;
    ID3D12Resource *inputs;
    uint8_t records[80];
    ID3D12StateObject *pso;
    unsigned int i, j;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, two_entry_dxil, u"Dummy", context.default_root_uav_rs);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query work graph props, hr #%x.\n", (int)hr);
    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12StateObjectProperties1, (void **)&props1);
    ok(SUCCEEDED(hr), "Failed to query SOP1, hr #%x.\n", (int)hr);
    if (!props || !props1)
        goto out;

    ok(ID3D12WorkGraphProperties_GetNumEntrypoints(props, 0) == 2, "Expected 2 entry points, got %u.\n",
            ID3D12WorkGraphProperties_GetNumEntrypoints(props, 0));
    /* The order of the entry points is not defined, so look up both. */
    index_a = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 0, entry_a_id);
    index_b = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 0, entry_b_id);
    ok(index_a < 2 && index_b < 2 && index_a != index_b, "Unexpected entry point indices %u, %u.\n", index_a, index_b);
    if (index_a >= 2 || index_b >= 2 || index_a == index_b)
        goto out;

    ident = ID3D12StateObjectProperties1_GetProgramIdentifier(props1, u"Dummy");
    memset(&wg_reqs, 0, sizeof(wg_reqs));
    ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 0, &wg_reqs);

    memset(records, 0, sizeof(records));
    memcpy(records, records_a, sizeof(records_a));
    memcpy(records + 32, records_b, sizeof(records_b));
    records_buffer = create_upload_buffer(context.context.device, sizeof(records), records);
    va = ID3D12Resource_GetGPUVirtualAddress(records_buffer);

    /* EntryA: 2 records at stride 16. EntryB: 3 records at stride 12. EntryA again: 1 record at stride 0, repeated 3 times. */
    node_inputs[0].EntrypointIndex = index_a;
    node_inputs[0].NumRecords = 2;
    node_inputs[0].Records.StartAddress = va;
    node_inputs[0].Records.StrideInBytes = 16;
    node_inputs[1].EntrypointIndex = index_b;
    node_inputs[1].NumRecords = 3;
    node_inputs[1].Records.StartAddress = va + 32;
    node_inputs[1].Records.StrideInBytes = 12;
    node_inputs[2].EntrypointIndex = index_a;
    node_inputs[2].NumRecords = 3;
    node_inputs[2].Records.StartAddress = va + 16;
    node_inputs[2].Records.StrideInBytes = 0;

    memset(input_array, 0, sizeof(input_array));
    for (i = 0; i < ARRAY_SIZE(node_inputs); i++)
        memcpy(input_array + 32 * i, &node_inputs[i], sizeof(node_inputs[i]));
    inputs = create_upload_buffer(context.context.device, sizeof(input_array), input_array);

    /* First one input per entry point, then a third input that targets EntryA again. */
    for (num_inputs = 2; num_inputs <= ARRAY_SIZE(node_inputs); num_inputs++)
    {
        vkd3d_test_set_context(num_inputs == 2 ? "two entry points" : "two entry points, EntryA twice");

        multi_gpu_input.NumNodeInputs = num_inputs;
        multi_gpu_input.NodeInputs.StartAddress = ID3D12Resource_GetGPUVirtualAddress(inputs);
        multi_gpu_input.NodeInputs.StrideInBytes = 32;
        input_multi = create_upload_buffer(context.context.device, sizeof(multi_gpu_input), &multi_gpu_input);

        scratch = NULL;
        if (wg_reqs.MinSizeInBytes)
        {
            scratch = create_default_buffer(context.context.device, wg_reqs.MinSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }

        output = create_default_buffer(context.context.device, 1024 * sizeof(uint32_t),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0,
                ID3D12Resource_GetGPUVirtualAddress(output));

        memset(&program_desc, 0, sizeof(program_desc));
        program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        if (scratch)
        {
            program_desc.WorkGraph.BackingMemory.SizeInBytes = wg_reqs.MinSizeInBytes;
            program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
        }
        program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        program_desc.WorkGraph.ProgramIdentifier = ident;
        ID3D12GraphicsCommandList10_SetProgram(context.list, &program_desc);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        dispatch_desc.MultiNodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(input_multi);
        for (i = 0; i < 2; i++)
            ID3D12GraphicsCommandList10_DispatchGraph(context.list, &dispatch_desc);

        transition_resource_state(context.context.list, output,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
                context.context.queue, context.context.list);
        reset_command_list(context.context.list, context.context.allocator);

        for (i = 0; i < 1024; i++)
        {
            uint32_t expected = 0;
            uint32_t v;

            for (j = 0; j < 2; j++)
                if (two_entry_grid_covers(&records_a[4 * j], i))
                    expected += 1;
            for (j = 0; j < 3; j++)
                if (two_entry_grid_covers(&records_b[3 * j], i))
                    expected += 0x10000;
            if (num_inputs > 2 && two_entry_grid_covers(&records_a[4], i))
                expected += 3;
            /* Two DispatchGraph calls. */
            expected *= 2;

            v = get_readback_uint(&rb, i, 0, 0);
            /* The NVIDIA driver on Windows gives a wrong count when two inputs with different strides target
             * the same entry point (measured on an RTX 4090). The spec allows several inputs to the same node. */
            bug_if(use_warp_device || (num_inputs > 2 && is_nvidia_windows_device(context.context.device)))
            ok(expected == v, "Value %u: expected #%x, got #%x.\n", i, expected, v);
        }

        release_resource_readback(&rb);
        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(input_multi);
    }
    vkd3d_test_set_context(NULL);

    ID3D12Resource_Release(inputs);
    ID3D12Resource_Release(records_buffer);

out:
    if (props1)
        ID3D12StateObjectProperties1_Release(props1);
    if (props)
        ID3D12WorkGraphProperties_Release(props);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_rw_entry_input(void)
{
    /* An entry point that writes to its input record (RWThreadNodeInputRecord). The shader adds 1 to RWBuf[value]
     * if it reads back value + 100 after its write, and 0x10000 if it reads another value.
     * With separate records, each thread must see its own write. With a record stride of 0 all records are one
     * record in memory: the native NVIDIA driver on Windows lets the threads see each other's writes there, so
     * only the launch count is checked (each record runs once). The spec makes the writeable part of GPU input
     * records undefined during graph execution (WorkGraphs.md, D3D12_NODE_GPU_INPUT), so the record data in the
     * application's buffer is not checked. */
    static const struct
    {
        const char *name;
        D3D12_DISPATCH_MODE mode;
        uint32_t stride;
        uint32_t constant_value; /* UINT32_MAX: record i has the value i. */
    } tests[] =
    {
        { "GPU input, packed", D3D12_DISPATCH_MODE_NODE_GPU_INPUT, 4, UINT32_MAX },
        { "GPU input, stride 0", D3D12_DISPATCH_MODE_NODE_GPU_INPUT, 0, 7 },
        { "Multi GPU input, stride 0", D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT, 0, 11 },
        { "CPU input, packed", D3D12_DISPATCH_MODE_NODE_CPU_INPUT, 4, UINT32_MAX },
        { "CPU input, stride 0", D3D12_DISPATCH_MODE_NODE_CPU_INPUT, 0, 9 },
    };
    enum { NUM_RECORDS = 256 };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    D3D12_MULTI_NODE_GPU_INPUT multi_gpu_input;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    D3D12_SET_PROGRAM_DESC program_desc;
    D3D12_NODE_GPU_INPUT node_gpu_input;
    struct resource_readback rb;
    uint32_t records[NUM_RECORDS];
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *records_buffer;
    ID3D12Resource *input_multi;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12Resource *input;
    ID3D12StateObject *pso;
    unsigned int i, j;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, rw_entry_input_dxil, u"Dummy", context.default_root_uav_rs);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    check_work_graph_properties(pso, u"Dummy", u"RWEntry", NULL, 4, &ident, &wg_reqs);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].name);

        for (j = 0; j < NUM_RECORDS; j++)
            records[j] = tests[i].constant_value == UINT32_MAX ? j : tests[i].constant_value;

        /* GPU input records in a default heap buffer, in the shader resource state that the spec asks for. */
        records_buffer = create_default_buffer(context.context.device, sizeof(records),
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        upload_buffer_data(records_buffer, 0, sizeof(records), records, context.context.queue, context.context.list);
        reset_command_list(context.context.list, context.context.allocator);
        transition_resource_state(context.context.list, records_buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        node_gpu_input.EntrypointIndex = 0;
        node_gpu_input.NumRecords = NUM_RECORDS;
        node_gpu_input.Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(records_buffer);
        node_gpu_input.Records.StrideInBytes = tests[i].stride;
        input = create_upload_buffer(context.context.device, sizeof(node_gpu_input), &node_gpu_input);

        multi_gpu_input.NumNodeInputs = 1;
        multi_gpu_input.NodeInputs.StartAddress = ID3D12Resource_GetGPUVirtualAddress(input);
        multi_gpu_input.NodeInputs.StrideInBytes = 0;
        input_multi = create_upload_buffer(context.context.device, sizeof(multi_gpu_input), &multi_gpu_input);

        scratch = NULL;
        if (wg_reqs.MinSizeInBytes)
        {
            scratch = create_default_buffer(context.context.device, wg_reqs.MinSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        output = create_default_buffer(context.context.device, 1024 * sizeof(uint32_t),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0,
                ID3D12Resource_GetGPUVirtualAddress(output));

        memset(&program_desc, 0, sizeof(program_desc));
        program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        if (scratch)
        {
            program_desc.WorkGraph.BackingMemory.SizeInBytes = wg_reqs.MinSizeInBytes;
            program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
        }
        program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        program_desc.WorkGraph.ProgramIdentifier = ident;
        ID3D12GraphicsCommandList10_SetProgram(context.list, &program_desc);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = tests[i].mode;
        switch (tests[i].mode)
        {
            case D3D12_DISPATCH_MODE_NODE_GPU_INPUT:
                dispatch_desc.NodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(input);
                break;
            case D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT:
                dispatch_desc.MultiNodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(input_multi);
                break;
            default:
                dispatch_desc.NodeCPUInput.EntrypointIndex = 0;
                dispatch_desc.NodeCPUInput.NumRecords = NUM_RECORDS;
                dispatch_desc.NodeCPUInput.pRecords = records;
                dispatch_desc.NodeCPUInput.RecordStrideInBytes = tests[i].stride;
                break;
        }
        ID3D12GraphicsCommandList10_DispatchGraph(context.list, &dispatch_desc);

        transition_resource_state(context.context.list, output,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
                context.context.queue, context.context.list);
        reset_command_list(context.context.list, context.context.allocator);

        if (tests[i].constant_value == UINT32_MAX)
        {
            for (j = 0; j < 1024; j++)
            {
                uint32_t expected = j < NUM_RECORDS ? 1 : 0;
                uint32_t v = get_readback_uint(&rb, j, 0, 0);
                ok(v == expected, "Value %u: expected #%x, got #%x.\n", j, expected, v);
            }
        }
        else
        {
            uint32_t launches = 0;

            for (j = 0; j < 1024; j++)
            {
                uint32_t v = get_readback_uint(&rb, j, 0, 0);
                launches += (v & 0xffff) + (v >> 16);
            }
            ok(launches == NUM_RECORDS, "Expected %u launches, got %u.\n", NUM_RECORDS, launches);
        }

        /* The system copies CPU input records, so the application's array does not change. */
        for (j = 0; j < NUM_RECORDS; j++)
        {
            uint32_t expected = tests[i].constant_value == UINT32_MAX ? j : tests[i].constant_value;
            ok(records[j] == expected, "CPU record %u: expected %u, got %u.\n", j, expected, records[j]);
        }

        release_resource_readback(&rb);
        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(input_multi);
        ID3D12Resource_Release(input);
        ID3D12Resource_Release(records_buffer);
    }
    vkd3d_test_set_context(NULL);

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

static HRESULT try_create_workgraph_pso(struct test_context_workgraph *context, D3D12_SHADER_BYTECODE code,
        LPCWSTR entry_point, ID3D12StateObject **pso)
{
    D3D12_NODE_ID entry_point_id;
    D3D12_GLOBAL_ROOT_SIGNATURE grs_desc;
    D3D12_STATE_SUBOBJECT subobjects[3];
    D3D12_STATE_OBJECT_DESC pso_desc;
    D3D12_WORK_GRAPH_DESC wg_desc;
    D3D12_DXIL_LIBRARY_DESC dxil;

    memset(&pso_desc, 0, sizeof(pso_desc));
    memset(subobjects, 0, sizeof(subobjects));
    memset(&dxil, 0, sizeof(dxil));
    memset(&wg_desc, 0, sizeof(wg_desc));
    pso_desc.NumSubobjects = ARRAY_SIZE(subobjects);
    pso_desc.pSubobjects = subobjects;
    pso_desc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    grs_desc.pGlobalRootSignature = context->default_root_uav_rs;

    subobjects[0].pDesc = &dxil;
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = &wg_desc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    subobjects[2].pDesc = &grs_desc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

    dxil.DXILLibrary = code;
    wg_desc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    wg_desc.ProgramName = u"Dummy";
    if (entry_point)
    {
        entry_point_id.Name = entry_point;
        entry_point_id.ArrayIndex = 0;
        wg_desc.NumEntrypoints = 1;
        wg_desc.pEntrypoints = &entry_point_id;
    }

    *pso = NULL;
    return ID3D12Device5_CreateStateObject(context->device, &pso_desc, &IID_ID3D12StateObject, (void **)pso);
}

void test_workgraph_depth_limit(void)
{
    /* The longest chain of nodes is at most 32 (D3D12_WORK_GRAPHS_MAX_NODE_DEPTH), and a recursive node counts
     * by its declared recursion depth (WorkGraphs.md, Node count limits). depth_limit_32 has a chain of 32 nodes,
     * depth_limit_33 a chain of 33. A graph that is too deep, or that names a missing entry point, must fail with
     * E_INVALIDARG (as on native NVIDIA) after the engine has started to build its program, without a crash, and
     * the device must stay usable. */
    struct test_context_workgraph context;
    ID3D12StateObject *pso;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    hr = try_create_workgraph_pso(&context, depth_limit_32_dxil, NULL, &pso);
    ok(hr == S_OK, "Chain of 32 nodes: expected S_OK, got hr #%x.\n", (int)hr);
    if (pso)
        ID3D12StateObject_Release(pso);

    hr = try_create_workgraph_pso(&context, depth_limit_33_dxil, NULL, &pso);
    ok(hr == E_INVALIDARG, "Chain of 33 nodes: expected E_INVALIDARG, got hr #%x.\n", (int)hr);
    if (pso)
        ID3D12StateObject_Release(pso);

    /* An entry point in the work graph description that names no node of the library. */
    hr = try_create_workgraph_pso(&context, depth_limit_32_dxil, u"NoSuchNode", &pso);
    ok(hr == E_INVALIDARG, "Missing entry point: expected E_INVALIDARG, got hr #%x.\n", (int)hr);
    if (pso)
        ID3D12StateObject_Release(pso);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    hr = try_create_workgraph_pso(&context, depth_limit_32_dxil, NULL, &pso);
    ok(hr == S_OK, "Chain of 32 nodes after the failure: expected S_OK, got hr #%x.\n", (int)hr);
    if (pso)
        ID3D12StateObject_Release(pso);

    destroy_workgraph_test_context(&context);
}

static void workgraph_set_fault(const char *value)
{
#ifdef _WIN32
    SetEnvironmentVariableA("AMDGPU_WDDM_WG_FAIL", value);
#else
    if (value)
        setenv("AMDGPU_WDDM_WG_FAIL", value, 1);
    else
        unsetenv("AMDGPU_WDDM_WG_FAIL");
#endif
}

/* The lab flavour of the amdgpu-wddm engine reads AMDGPU_WDDM_WG_MULTI_INPUT when it creates a work graph: small
 * staging and large input sizes and few workgroups let a test cross each limit of the multi GPU input passes with a
 * few thousand records. Other engines ignore the variable, and the tests give the same results with any value. */
#define WG_MULTI_INPUT_SMALL "staging=262144,large=2048,groups=2"

static void workgraph_set_multi_input_config(const char *value)
{
    const char *env = getenv("VKD3D_TEST_WORKGRAPH_MULTI_INPUT");

    /* VKD3D_TEST_WORKGRAPH_MULTI_INPUT replaces WG_MULTI_INPUT_SMALL, for a negative control with smaller limits. */
    if (value && env && env[0])
        value = env;
#ifdef _WIN32
    SetEnvironmentVariableA("AMDGPU_WDDM_WG_MULTI_INPUT", value);
#else
    if (value)
        setenv("AMDGPU_WDDM_WG_MULTI_INPUT", value, 1);
    else
        unsetenv("AMDGPU_WDDM_WG_MULTI_INPUT");
#endif
}

void test_workgraph_create_failure(void)
{
    /* The lab flavour of the amdgpu-wddm engine reads AMDGPU_WDDM_WG_FAIL at each work graph creation and fails
     * it at the named point as if an allocation failed there. Each point must give E_OUTOFMEMORY without a crash,
     * and the device must stay usable. depth_limit_32 has 2 nodes and 30 recursion depth slots (2 to 31); slot 31
     * is in the last level, so "pipeline:31" fails after the pipelines of all earlier levels were created.
     * Set VKD3D_TEST_WORKGRAPH_FAULT_INJECTION=1 to run this test; other engines ignore the variable. */
    static const char *points[] = { "slots", "program", "pipeline:0", "pipeline:31" };
    struct test_context_workgraph context;
    const char *env;
    ID3D12StateObject *pso;
    unsigned int i;
    HRESULT hr;

    env = getenv("VKD3D_TEST_WORKGRAPH_FAULT_INJECTION");
    if (!env || env[0] != '1')
    {
        skip("Set VKD3D_TEST_WORKGRAPH_FAULT_INJECTION=1 to run the fault injection test.\n");
        return;
    }

    if (!init_workgraph_test_context(&context))
        return;

    for (i = 0; i < ARRAY_SIZE(points); i++)
    {
        vkd3d_test_set_context("%s", points[i]);
        workgraph_set_fault(points[i]);
        hr = try_create_workgraph_pso(&context, depth_limit_32_dxil, NULL, &pso);
        workgraph_set_fault(NULL);
        ok(hr == E_OUTOFMEMORY, "Expected E_OUTOFMEMORY, got hr #%x.\n", (int)hr);
        ok(!pso, "Expected no state object.\n");
        if (pso)
            ID3D12StateObject_Release(pso);

        hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
        ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    }
    vkd3d_test_set_context(NULL);

    hr = try_create_workgraph_pso(&context, depth_limit_32_dxil, NULL, &pso);
    ok(hr == S_OK, "After the injected failures: expected S_OK, got hr #%x.\n", (int)hr);
    if (pso)
        ID3D12StateObject_Release(pso);

    destroy_workgraph_test_context(&context);
}

static HRESULT create_workgraph_pso_nodes(struct test_context_workgraph *context, D3D12_SHADER_BYTECODE code,
        const D3D12_NODE *nodes, unsigned int nodes_count, ID3D12StateObject **pso)
{
    D3D12_GLOBAL_ROOT_SIGNATURE grs_desc;
    D3D12_STATE_SUBOBJECT subobjects[3];
    D3D12_STATE_OBJECT_DESC pso_desc;
    D3D12_WORK_GRAPH_DESC wg_desc;
    D3D12_DXIL_LIBRARY_DESC dxil;

    memset(&pso_desc, 0, sizeof(pso_desc));
    memset(subobjects, 0, sizeof(subobjects));
    memset(&dxil, 0, sizeof(dxil));
    memset(&wg_desc, 0, sizeof(wg_desc));
    pso_desc.NumSubobjects = ARRAY_SIZE(subobjects);
    pso_desc.pSubobjects = subobjects;
    pso_desc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    grs_desc.pGlobalRootSignature = context->default_root_uav_rs;

    subobjects[0].pDesc = &dxil;
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = &wg_desc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    subobjects[2].pDesc = &grs_desc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

    dxil.DXILLibrary = code;
    wg_desc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    wg_desc.ProgramName = u"Dummy";
    wg_desc.NumExplicitlyDefinedNodes = nodes_count;
    wg_desc.pExplicitlyDefinedNodes = nodes;

    *pso = NULL;
    return ID3D12Device5_CreateStateObject(context->device, &pso_desc, &IID_ID3D12StateObject, (void **)pso);
}

/* Runs one DispatchGraph with CPU input to entry point 0 and reads back the first 16 values of the output UAV. */
static void workgraph_run_cpu_input(struct test_context_workgraph *context, ID3D12StateObject *pso,
        const void *records, size_t stride, size_t count,
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *local_root_table, uint32_t *values)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    ID3D12StateObjectProperties1 *props1;
    ID3D12WorkGraphProperties *props;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12Resource *output;
    unsigned int i;
    HRESULT hr;

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12StateObjectProperties1, (void **)&props1);
    ok(SUCCEEDED(hr), "Failed to query ID3D12StateObjectProperties1, hr #%x.\n", (int)hr);
    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
    ident = ID3D12StateObjectProperties1_GetProgramIdentifier(props1, u"Dummy");
    ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 0, &wg_reqs);
    ID3D12WorkGraphProperties_Release(props);
    ID3D12StateObjectProperties1_Release(props1);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context->context.list, context->default_root_uav_rs);
    output = create_default_buffer(context->context.device, 4 * 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context->context.list, 0,
            ID3D12Resource_GetGPUVirtualAddress(output));

    execute_workgraph_pso_simple(context, pso, &ident, &wg_reqs, records, stride, count, local_root_table, &scratch);

    transition_resource_state(context->context.list, output,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
            context->context.queue, context->context.list);
    reset_command_list(context->context.list, context->context.allocator);
    for (i = 0; i < 16; i++)
        values[i] = get_readback_uint(&rb, i, 0, 0);

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
}

void test_workgraph_lrat_order(void)
{
    /* Default local root arguments table indices are given in the order of graph population: the explicitly
     * defined nodes first, then the listed entry points, then INCLUDE_ALL_AVAILABLE_NODES in export order
     * (WorkGraphs.md, D3D12_WORK_GRAPH_DESC and [NodeLocalRootArgumentsTableIndex]). C has the explicit index 0.
     * The explicitly defined nodes are B, then A, so B gets 1 and A gets 2. Each table record holds a different
     * constant, and each node adds its constant to its own UAV element. */
    static const uint32_t local_data[3][4] = { { 100 }, { 200 }, { 300 } };
    static const struct
    {
        const WCHAR *name;
        uint32_t index;
        uint32_t uav;
    } expected[] =
    {
        { u"A", 2, 0 },
        { u"B", 1, 1 },
        { u"C", 0, 2 },
    };
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE local_table;
    D3D12_THREAD_LAUNCH_OVERRIDES overrides;
    struct test_context_workgraph context;
    ID3D12WorkGraphProperties *props;
    D3D12_NODE nodes[2];
    ID3D12StateObject *pso;
    ID3D12Resource *local;
    uint32_t values[16];
    D3D12_NODE_ID id;
    unsigned int i;
    UINT index;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    memset(&overrides, 0, sizeof(overrides));
    memset(nodes, 0, sizeof(nodes));
    for (i = 0; i < ARRAY_SIZE(nodes); i++)
    {
        nodes[i].NodeType = D3D12_NODE_TYPE_SHADER;
        nodes[i].Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_THREAD_LAUNCH;
        nodes[i].Shader.pThreadLaunchOverrides = &overrides;
    }
    nodes[0].Shader.Shader = u"B";
    nodes[1].Shader.Shader = u"A";

    hr = create_workgraph_pso_nodes(&context, lrat_order_dxil, nodes, ARRAY_SIZE(nodes), &pso);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (FAILED(hr))
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
    for (i = 0; i < ARRAY_SIZE(expected); i++)
    {
        id.Name = expected[i].name;
        id.ArrayIndex = 0;
        index = ID3D12WorkGraphProperties_GetNodeIndex(props, 0, id);
        index = ID3D12WorkGraphProperties_GetNodeLocalRootArgumentsTableIndex(props, 0, index);
        ok(index == expected[i].index, "Node %u: expected table index %u, got %u.\n", i, expected[i].index, index);
    }
    ID3D12WorkGraphProperties_Release(props);

    local = create_upload_buffer(context.context.device, sizeof(local_data), local_data);
    local_table.StartAddress = ID3D12Resource_GetGPUVirtualAddress(local);
    local_table.StrideInBytes = sizeof(local_data[0]);
    local_table.SizeInBytes = sizeof(local_data);
    workgraph_run_cpu_input(&context, pso, NULL, 0, 1, &local_table, values);

    for (i = 0; i < ARRAY_SIZE(expected); i++)
    {
        ok(values[expected[i].uav] == local_data[expected[i].index][0], "Node %u: expected %u, got %u.\n",
                i, local_data[expected[i].index][0], values[expected[i].uav]);
    }

    ID3D12Resource_Release(local);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* ASCII copy of a node name for messages. */
static const char *workgraph_dbgstr_w(const WCHAR *str)
{
    static char buffers[4][64];
    static unsigned int next;
    char *buffer;
    unsigned int i;

    if (!str)
        return "(null)";
    buffer = buffers[next++ % ARRAY_SIZE(buffers)];
    for (i = 0; i + 1 < sizeof(buffers[0]) && str[i]; i++)
        buffer[i] = str[i] < 0x80 ? (char)str[i] : '?';
    buffer[i] = '\0';
    return buffer;
}

struct workgraph_order_node
{
    const WCHAR *name;
    UINT array_index;
};

struct workgraph_order_case
{
    const char *name;
    D3D12_WORK_GRAPH_FLAGS flags;
    const struct workgraph_order_node *entry_list;
    unsigned int entry_list_count;
    /* Export name of one explicitly defined node, or NULL. */
    const WCHAR *explicit_node;
    BOOL explicit_program_entry;
    /* The library lists its exports, in the reverse of the definition order. */
    bool export_list;
    HRESULT expected_hr;
    const struct workgraph_order_node *nodes;
    unsigned int node_count;
    const struct workgraph_order_node *entries;
    unsigned int entry_count;
};

static HRESULT create_workgraph_pso_desc(struct test_context_workgraph *context, D3D12_SHADER_BYTECODE code,
        const D3D12_EXPORT_DESC *exports, unsigned int export_count, const D3D12_WORK_GRAPH_DESC *wg_desc,
        ID3D12StateObject **pso)
{
    D3D12_GLOBAL_ROOT_SIGNATURE grs_desc;
    D3D12_STATE_SUBOBJECT subobjects[3];
    D3D12_STATE_OBJECT_DESC pso_desc;
    D3D12_DXIL_LIBRARY_DESC dxil;

    memset(&pso_desc, 0, sizeof(pso_desc));
    memset(subobjects, 0, sizeof(subobjects));
    memset(&dxil, 0, sizeof(dxil));
    pso_desc.NumSubobjects = ARRAY_SIZE(subobjects);
    pso_desc.pSubobjects = subobjects;
    pso_desc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    grs_desc.pGlobalRootSignature = context->default_root_uav_rs;

    subobjects[0].pDesc = &dxil;
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = wg_desc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    subobjects[2].pDesc = &grs_desc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    dxil.DXILLibrary = code;
    dxil.NumExports = export_count;
    dxil.pExports = export_count ? (D3D12_EXPORT_DESC *)exports : NULL;

    *pso = NULL;
    return ID3D12Device5_CreateStateObject(context->device, &pso_desc, &IID_ID3D12StateObject, (void **)pso);
}

static void check_work_graph_node_order(ID3D12Device5 *device, ID3D12StateObject *pso,
        const struct workgraph_order_case *test)
{
    ID3D12WorkGraphProperties *props;
    D3D12_NODE_ID id;
    unsigned int i;
    HRESULT hr;
    UINT value;

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
    if (FAILED(hr))
        return;

    value = ID3D12WorkGraphProperties_GetNumNodes(props, 0);
    ok(value == test->node_count, "Expected %u nodes, got %u.\n", test->node_count, value);
    for (i = 0; i < test->node_count; i++)
    {
        id = ID3D12WorkGraphProperties_GetNodeID(props, 0, i);
        ok(export_strequal(id.Name, test->nodes[i].name) && id.ArrayIndex == test->nodes[i].array_index,
                "Node %u: expected %s[%u], got %s[%u].\n", i, workgraph_dbgstr_w(test->nodes[i].name),
                test->nodes[i].array_index, workgraph_dbgstr_w(id.Name), id.ArrayIndex);

        id.Name = test->nodes[i].name;
        id.ArrayIndex = test->nodes[i].array_index;
        value = ID3D12WorkGraphProperties_GetNodeIndex(props, 0, id);
        ok(value == i, "GetNodeIndex(%s[%u]): expected %u, got %u.\n", workgraph_dbgstr_w(id.Name), id.ArrayIndex,
                i, value);
    }

    /* Invalid indices and IDs (WorkGraphs.md, GetNumNodes, GetNodeID and GetNodeIndex). */
    id = ID3D12WorkGraphProperties_GetNodeID(props, 0, test->node_count);
    ok(!id.Name && !id.ArrayIndex, "GetNodeID(%u): expected {NULL, 0}, got %s[%u].\n", test->node_count,
            workgraph_dbgstr_w(id.Name), id.ArrayIndex);
    id = ID3D12WorkGraphProperties_GetNodeID(props, 1, 0);
    ok(!id.Name && !id.ArrayIndex, "GetNodeID of graph 1: expected {NULL, 0}, got %s[%u].\n",
            workgraph_dbgstr_w(id.Name), id.ArrayIndex);
    value = ID3D12WorkGraphProperties_GetNumNodes(props, 1);
    /* The NVIDIA driver returns 0. */
    bug_if(is_nvidia_windows_device((ID3D12Device *)device) && !workgraph_is_vkd3d_proton(device))
    ok(value == UINT32_MAX, "GetNumNodes of graph 1: expected -1, got %u.\n", value);
    id.Name = u"Arr";
    id.ArrayIndex = 2;
    value = ID3D12WorkGraphProperties_GetNodeIndex(props, 0, id);
    ok(value == UINT32_MAX, "GetNodeIndex(Arr[2]): expected -1, got %u.\n", value);
    id.Name = u"Nope";
    id.ArrayIndex = 0;
    value = ID3D12WorkGraphProperties_GetNodeIndex(props, 0, id);
    ok(value == UINT32_MAX, "GetNodeIndex(Nope): expected -1, got %u.\n", value);
    id.Name = test->nodes[0].name;
    id.ArrayIndex = test->nodes[0].array_index;
    value = ID3D12WorkGraphProperties_GetNodeIndex(props, 1, id);
    ok(value == UINT32_MAX, "GetNodeIndex of graph 1: expected -1, got %u.\n", value);

    value = ID3D12WorkGraphProperties_GetNumEntrypoints(props, 0);
    ok(value == test->entry_count, "Expected %u entry points, got %u.\n", test->entry_count, value);
    for (i = 0; i < test->entry_count; i++)
    {
        id = ID3D12WorkGraphProperties_GetEntrypointID(props, 0, i);
        ok(export_strequal(id.Name, test->entries[i].name) && id.ArrayIndex == test->entries[i].array_index,
                "Entry point %u: expected %s[%u], got %s[%u].\n", i, workgraph_dbgstr_w(test->entries[i].name),
                test->entries[i].array_index, workgraph_dbgstr_w(id.Name), id.ArrayIndex);

        id.Name = test->entries[i].name;
        id.ArrayIndex = test->entries[i].array_index;
        value = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 0, id);
        ok(value == i, "GetEntrypointIndex(%s[%u]): expected %u, got %u.\n", workgraph_dbgstr_w(id.Name),
                id.ArrayIndex, i, value);
    }

    id = ID3D12WorkGraphProperties_GetEntrypointID(props, 0, test->entry_count);
    ok(!id.Name && !id.ArrayIndex, "GetEntrypointID(%u): expected {NULL, 0}, got %s[%u].\n", test->entry_count,
            workgraph_dbgstr_w(id.Name), id.ArrayIndex);
    id = ID3D12WorkGraphProperties_GetEntrypointID(props, 1, 0);
    ok(!id.Name && !id.ArrayIndex, "GetEntrypointID of graph 1: expected {NULL, 0}, got %s[%u].\n",
            workgraph_dbgstr_w(id.Name), id.ArrayIndex);
    value = ID3D12WorkGraphProperties_GetNumEntrypoints(props, 1);
    ok(value == UINT32_MAX, "GetNumEntrypoints of graph 1: expected -1, got %u.\n", value);
    /* The last node of each case is not an entry point. */
    id.Name = test->nodes[test->node_count - 1].name;
    id.ArrayIndex = test->nodes[test->node_count - 1].array_index;
    value = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 0, id);
    ok(value == UINT32_MAX || test->node_count == 1, "GetEntrypointIndex(%s[%u]): expected -1, got %u.\n",
            workgraph_dbgstr_w(id.Name), id.ArrayIndex, value);
    id.Name = test->entries[0].name;
    id.ArrayIndex = test->entries[0].array_index;
    value = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 1, id);
    ok(value == UINT32_MAX, "GetEntrypointIndex of graph 1: expected -1, got %u.\n", value);

    ID3D12WorkGraphProperties_Release(props);
}

void test_workgraph_node_order(void)
{
    /* The node order of a work graph is the order of population (WorkGraphs.md, D3D12_WORK_GRAPH_DESC): first
     * the explicitly defined nodes, then the listed entry points, then INCLUDE_ALL_AVAILABLE_NODES in the
     * export order of the library. A node that is added also adds its outputs, depth first, in the order the
     * shader declares them; an array output adds its existing elements in index order. A node that is already
     * in the graph adds nothing. An explicitly defined node that is not an entry point does not add its
     * outputs; they are added when the population from an entry point reaches the node. GetNodeID and
     * GetNodeIndex are inverses over this order. The entry point indices are in the same order. The test
     * creates the state objects only; it does not dispatch. */
#define N(name) { u"" #name, 0 }
#define ARR(index) { u"Arr", index }
    static const struct workgraph_order_node all_nodes[] =
            { N(Zeta), N(Mid), N(Kid), N(Lone), N(Beta), N(Alpha), ARR(1), ARR(3), N(Omega), N(Pi) };
    static const struct workgraph_order_node omega_nodes[] = { N(Omega), N(Lone), N(Pi) };
    static const struct workgraph_order_node omega_zeta_nodes[] =
            { N(Omega), N(Lone), N(Pi), N(Zeta), N(Mid), N(Kid), N(Beta), N(Alpha), ARR(1), ARR(3) };
    static const struct workgraph_order_node beta_first_nodes[] =
            { N(Beta), N(Zeta), N(Mid), N(Kid), N(Lone), N(Alpha), ARR(1), ARR(3), N(Omega), N(Pi) };
    static const struct workgraph_order_node mid_nodes[] = { N(Mid), N(Kid), N(Lone) };
    static const struct workgraph_order_node arr1_nodes[] = { ARR(1) };
    static const struct workgraph_order_node arr2_list[] = { ARR(2) };
    static const struct workgraph_order_node zeta_omega[] = { N(Zeta), N(Omega) };
    static const struct workgraph_order_node omega_zeta[] = { N(Omega), N(Zeta) };
    static const struct workgraph_order_node omega[] = { N(Omega) };
    static const struct workgraph_order_node mid[] = { N(Mid) };
#undef N
#undef ARR
    static const struct workgraph_order_case tests[] =
    {
        { "include all", D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES, NULL, 0, NULL, FALSE, false, S_OK,
                all_nodes, ARRAY_SIZE(all_nodes), zeta_omega, ARRAY_SIZE(zeta_omega) },
        { "include all, export list", D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES, NULL, 0, NULL, FALSE, true,
                S_OK, all_nodes, ARRAY_SIZE(all_nodes), zeta_omega, ARRAY_SIZE(zeta_omega) },
        { "entry Omega", 0, omega, ARRAY_SIZE(omega), NULL, FALSE, false, S_OK,
                omega_nodes, ARRAY_SIZE(omega_nodes), omega, ARRAY_SIZE(omega) },
        { "entries Omega Zeta", 0, omega_zeta, ARRAY_SIZE(omega_zeta), NULL, FALSE, false, S_OK,
                omega_zeta_nodes, ARRAY_SIZE(omega_zeta_nodes), omega_zeta, ARRAY_SIZE(omega_zeta) },
        { "entry Omega, include all", D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES, omega, ARRAY_SIZE(omega),
                NULL, FALSE, false, S_OK, omega_zeta_nodes, ARRAY_SIZE(omega_zeta_nodes), omega_zeta, ARRAY_SIZE(omega_zeta) },
        { "explicit Beta, include all", D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES, NULL, 0, u"Beta", FALSE, false,
                S_OK, beta_first_nodes, ARRAY_SIZE(beta_first_nodes), zeta_omega, ARRAY_SIZE(zeta_omega) },
        { "explicit entry Mid", 0, NULL, 0, u"Mid", TRUE, false, S_OK, mid_nodes, ARRAY_SIZE(mid_nodes),
                mid, ARRAY_SIZE(mid) },
        { "entry Arr[1]", 0, arr1_nodes, ARRAY_SIZE(arr1_nodes), NULL, FALSE, false, S_OK,
                arr1_nodes, ARRAY_SIZE(arr1_nodes), arr1_nodes, ARRAY_SIZE(arr1_nodes) },
        /* Kid is not an entry point, and no node in the graph writes to it. */
        { "explicit Kid only", 0, NULL, 0, u"Kid", FALSE, false, E_INVALIDARG },
        /* Arr[2] does not exist. */
        { "entry Arr[2]", 0, arr2_list, ARRAY_SIZE(arr2_list), NULL, FALSE, false, E_INVALIDARG },
    };
    static const WCHAR *export_names[] =
            { u"Pi", u"Lone", u"Arr1", u"Arr3", u"Kid", u"Mid", u"Beta", u"Omega", u"Alpha", u"Zeta" };
    D3D12_EXPORT_DESC exports[ARRAY_SIZE(export_names)];
    D3D12_THREAD_LAUNCH_OVERRIDES overrides;
    struct test_context_workgraph context;
    D3D12_NODE_ID entry_ids[2];
    D3D12_WORK_GRAPH_DESC wg_desc;
    ID3D12StateObject *pso;
    D3D12_NODE node;
    unsigned int i, j;
    HRESULT hr;

#include "shaders/workgraph/headers/node_order.h"

    if (!init_workgraph_test_context(&context))
        return;

    memset(exports, 0, sizeof(exports));
    for (i = 0; i < ARRAY_SIZE(exports); i++)
        exports[i].Name = export_names[i];

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].name);

        memset(&wg_desc, 0, sizeof(wg_desc));
        wg_desc.ProgramName = u"Dummy";
        wg_desc.Flags = tests[i].flags;
        for (j = 0; j < tests[i].entry_list_count; j++)
        {
            entry_ids[j].Name = tests[i].entry_list[j].name;
            entry_ids[j].ArrayIndex = tests[i].entry_list[j].array_index;
        }
        wg_desc.NumEntrypoints = tests[i].entry_list_count;
        wg_desc.pEntrypoints = tests[i].entry_list_count ? entry_ids : NULL;

        if (tests[i].explicit_node)
        {
            memset(&overrides, 0, sizeof(overrides));
            memset(&node, 0, sizeof(node));
            node.NodeType = D3D12_NODE_TYPE_SHADER;
            node.Shader.Shader = tests[i].explicit_node;
            if (tests[i].explicit_program_entry)
            {
                overrides.pProgramEntry = &tests[i].explicit_program_entry;
                node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_THREAD_LAUNCH;
                node.Shader.pThreadLaunchOverrides = &overrides;
            }
            else
                node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_NONE;
            wg_desc.NumExplicitlyDefinedNodes = 1;
            wg_desc.pExplicitlyDefinedNodes = &node;
        }

        hr = create_workgraph_pso_desc(&context, node_order_dxil, exports,
                tests[i].export_list ? ARRAY_SIZE(exports) : 0, &wg_desc, &pso);
        if (tests[i].expected_hr == S_OK)
            ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        else
            ok(FAILED(hr), "Expected failure, hr #%x.\n", (int)hr);
        if (SUCCEEDED(hr))
        {
            if (tests[i].expected_hr == S_OK)
                check_work_graph_node_order(context.device, pso, &tests[i]);
            ID3D12StateObject_Release(pso);
        }
    }
    vkd3d_test_set_context(NULL);

    destroy_workgraph_test_context(&context);
}

void test_workgraph_array_self_recursion(void)
{
    /* N[1] is the only element of the node array N and has NodeMaxRecursionDepth(1). It recurses through
     * element 1 of a sparse output array of N (element 0 is a hole), or through a scalar output to N[1]. A node
     * may target itself, but not another node of its own array (WorkGraphs.md, Node array constraints).
     * Depth 0 adds GetRemainingRecursionLevels() + 1 = 2 to RWBuf[0], depth 1 adds 1 to RWBuf[1]. */
    static const uint32_t record = 0;
    static const struct
    {
        const char *name;
        D3D12_SHADER_BYTECODE code;
    } tests[] =
    {
        { "sparse output array", { sparse_self_recursion_code_dxil, sizeof(sparse_self_recursion_code_dxil) } },
        { "scalar output", { scalar_self_recursion_code_dxil, sizeof(scalar_self_recursion_code_dxil) } },
    };
    struct test_context_workgraph context;
    ID3D12StateObject *pso;
    uint32_t values[16];
    unsigned int i;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].name);
        hr = try_create_workgraph_pso(&context, tests[i].code, NULL, &pso);
        ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        if (FAILED(hr))
            continue;

        workgraph_run_cpu_input(&context, pso, &record, sizeof(record), 1, NULL, values);
        ok(values[0] == 2, "Depth 0: expected 2, got %u.\n", values[0]);
        ok(values[1] == 1, "Depth 1: expected 1, got %u.\n", values[1]);
        ok(values[2] == 0, "Depth 2: expected 0, got %u.\n", values[2]);
        ID3D12StateObject_Release(pso);
    }
    vkd3d_test_set_context(NULL);

    destroy_workgraph_test_context(&context);
}

void test_workgraph_empty_cpu_input(void)
{
    /* For an empty input record, pRecords and RecordStrideInBytes of D3D12_NODE_CPU_INPUT are ignored
     * (WorkGraphs.md, D3D12_NODE_CPU_INPUT). With pRecords NULL and each stride, the entry point must run once
     * for each record. */
    static const uint32_t strides[] = { 0, 4, 16, 4096 };
    struct test_context_workgraph context;
    ID3D12StateObject *pso;
    uint32_t values[16];
    unsigned int i;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    hr = try_create_workgraph_pso(&context, empty_entry_dxil, NULL, &pso);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (FAILED(hr))
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    for (i = 0; i < ARRAY_SIZE(strides); i++)
    {
        vkd3d_test_set_context("stride %u", strides[i]);
        workgraph_run_cpu_input(&context, pso, NULL, strides[i], 37, NULL, values);
        ok(values[0] == 37, "Expected 37 launches, got %u.\n", values[0]);
    }
    vkd3d_test_set_context(NULL);

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_cpu_input_extent(void)
{
    /* The CPU input of an entry point must read only the application's record bytes. The last record is at the
     * end of a page, and the next page is not accessible.
     * rw_entry_input has a 4-byte record. With a stride of 16, the last record has no padding after it.
     * track_rw_entry has a 4-byte record with [NodeTrackRWInputSharing]. The internal word of an implementation is
     * not part of the application's record, so GetEntrypointRecordSizeInBytes() must report 4. Native NVIDIA
     * refuses this state object with E_INVALIDARG; the cases then do not run. */
#ifdef _WIN32
    static const struct
    {
        const char *name;
        bool tracked;
        uint32_t stride;
        uint32_t count;
    } tests[] =
    {
        { "record, stride 4", false, 4, 3 },
        { "record, stride 16", false, 16, 2 },
        { "tracked record, stride 4", true, 4, 3 },
        { "tracked record, stride 16", true, 16, 2 },
        { "tracked record, stride 0", true, 0, 5 },
    };
    struct test_context_workgraph context;
    ID3D12StateObject *pso[2] = { NULL };
    uint32_t values[16], *records;
    ID3D12WorkGraphProperties *props;
    SYSTEM_INFO info;
    DWORD old_protect;
    unsigned int i, j;
    uint8_t *pages;
    size_t span;
    UINT size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    GetSystemInfo(&info);
    pages = VirtualAlloc(NULL, 2 * info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(!!pages, "Failed to allocate pages.\n");
    if (!pages)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old_protect);

    hr = try_create_workgraph_pso(&context, rw_entry_input_dxil, NULL, &pso[0]);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);

    hr = try_create_workgraph_pso(&context, track_rw_entry_dxil, NULL, &pso[1]);
    if (workgraph_is_vkd3d_proton(context.device))
        ok(hr == S_OK, "Failed to create the tracked state object, hr #%x.\n", (int)hr);
    else
        ok(hr == S_OK || hr == E_INVALIDARG, "Tracked state object: unexpected hr #%x.\n", (int)hr);
    if (pso[1])
    {
        hr = ID3D12StateObject_QueryInterface(pso[1], &IID_ID3D12WorkGraphProperties, (void **)&props);
        ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
        size = ID3D12WorkGraphProperties_GetEntrypointRecordSizeInBytes(props, 0, 0);
        ok(size == 4, "Expected record size 4, got %u.\n", size);
        ID3D12WorkGraphProperties_Release(props);
    }

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (!pso[tests[i].tracked])
            continue;
        vkd3d_test_set_context("%s", tests[i].name);

        span = tests[i].stride ? (tests[i].count - 1) * tests[i].stride + 4 : 4;
        records = (uint32_t *)(pages + info.dwPageSize - span);
        for (j = 0; j < (tests[i].stride ? tests[i].count : 1); j++)
            records[j * tests[i].stride / 4] = 3 + j;

        workgraph_run_cpu_input(&context, pso[tests[i].tracked], records, tests[i].stride, tests[i].count,
                NULL, values);
        for (j = 0; j < 16; j++)
        {
            uint32_t expected = 0;

            if (tests[i].stride && j >= 3 && j < 3 + tests[i].count)
                expected = 1;
            else if (!tests[i].stride && j == 3)
                expected = tests[i].count;
            ok(values[j] == expected, "Value %u: expected %u, got %u.\n", j, expected, values[j]);
        }
    }
    vkd3d_test_set_context(NULL);

    for (i = 0; i < ARRAY_SIZE(pso); i++)
    {
        if (pso[i])
            ID3D12StateObject_Release(pso[i]);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    destroy_workgraph_test_context(&context);
#else
    skip("The CPU input extent test needs VirtualAlloc.\n");
#endif
}

/* Set VKD3D_TEST_WORKGRAPH_CASE to a part of a case name to run only the cases whose name contains it. */
static bool workgraph_case_enabled(const char *name)
{
    const char *env = getenv("VKD3D_TEST_WORKGRAPH_CASE");

    return !env || !env[0] || strstr(name, env);
}

struct workgraph_multi_input
{
    ID3D12Resource *buffer;
    D3D12_GPU_VIRTUAL_ADDRESS va;
};

/* Writes one D3D12_MULTI_NODE_GPU_INPUT at offset 0 of an upload buffer and the D3D12_NODE_GPU_INPUT array at
 * offset 256, with a stride of 32 bytes. */
static void workgraph_create_multi_input(struct test_context_workgraph *context,
        const D3D12_NODE_GPU_INPUT *inputs, unsigned int inputs_count, UINT num_node_inputs, UINT64 input_stride,
        struct workgraph_multi_input *multi)
{
    D3D12_MULTI_NODE_GPU_INPUT header;
    unsigned int i;
    uint8_t *data;
    size_t size;

    size = 256 + 32 * max(inputs_count, 1u);
    data = calloc(1, size);
    for (i = 0; i < inputs_count; i++)
        memcpy(data + 256 + 32 * i, &inputs[i], sizeof(inputs[i]));
    multi->buffer = create_upload_buffer(context->context.device, size, NULL);
    multi->va = ID3D12Resource_GetGPUVirtualAddress(multi->buffer);

    memset(&header, 0, sizeof(header));
    header.NumNodeInputs = num_node_inputs;
    header.NodeInputs.StartAddress = multi->va + 256;
    header.NodeInputs.StrideInBytes = input_stride;
    memcpy(data, &header, sizeof(header));
    update_buffer_data(multi->buffer, 0, size, data);
    free(data);
}

static bool workgraph_get_program(ID3D12StateObject *pso, D3D12_PROGRAM_IDENTIFIER *ident,
        D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *wg_reqs, UINT *entry_indices, const D3D12_NODE_ID *entry_ids,
        unsigned int entry_count)
{
    ID3D12StateObjectProperties1 *props1 = NULL;
    ID3D12WorkGraphProperties *props = NULL;
    unsigned int i;
    bool ret = true;
    HRESULT hr;

    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12StateObjectProperties1, (void **)&props1);
    ok(SUCCEEDED(hr), "Failed to query ID3D12StateObjectProperties1, hr #%x.\n", (int)hr);
    hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
    ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
    if (!props1 || !props)
    {
        ret = false;
        goto out;
    }

    *ident = ID3D12StateObjectProperties1_GetProgramIdentifier(props1, u"Dummy");
    memset(wg_reqs, 0, sizeof(*wg_reqs));
    ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 0, wg_reqs);
    for (i = 0; i < entry_count; i++)
    {
        /* The order of the entry points is not defined, so look up each. */
        entry_indices[i] = ID3D12WorkGraphProperties_GetEntrypointIndex(props, 0, entry_ids[i]);
        ok(entry_indices[i] < entry_count, "Unexpected entry point index %u.\n", entry_indices[i]);
        if (entry_indices[i] >= entry_count)
            ret = false;
    }

out:
    if (props)
        ID3D12WorkGraphProperties_Release(props);
    if (props1)
        ID3D12StateObjectProperties1_Release(props1);
    return ret;
}

/* Records the root arguments, SetProgram with initialization of the backing memory and one DispatchGraph. */
static void workgraph_record_dispatch(struct test_context_workgraph *context, ID3D12GraphicsCommandList *list,
        const D3D12_PROGRAM_IDENTIFIER *ident, ID3D12Resource *scratch, UINT64 scratch_size,
        ID3D12Resource *output, const D3D12_DISPATCH_GRAPH_DESC *dispatch_desc)
{
    ID3D12GraphicsCommandList10 *list10;
    D3D12_SET_PROGRAM_DESC program_desc;
    HRESULT hr;

    hr = ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&list10);
    ok(SUCCEEDED(hr), "Failed to query ID3D12GraphicsCommandList10, hr #%x.\n", (int)hr);
    if (FAILED(hr))
        return;

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, context->default_root_uav_rs);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    memset(&program_desc, 0, sizeof(program_desc));
    program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    if (scratch)
    {
        program_desc.WorkGraph.BackingMemory.SizeInBytes = scratch_size;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
    }
    program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    program_desc.WorkGraph.ProgramIdentifier = *ident;
    ID3D12GraphicsCommandList10_SetProgram(list10, &program_desc);
    ID3D12GraphicsCommandList10_DispatchGraph(list10, dispatch_desc);
    ID3D12GraphicsCommandList10_Release(list10);
}

static ID3D12Resource *workgraph_create_backing_memory(struct test_context_workgraph *context,
        const D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *wg_reqs)
{
    if (!wg_reqs->MinSizeInBytes)
        return NULL;
    return create_default_buffer(context->context.device, wg_reqs->MinSizeInBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

static void workgraph_read_output(struct test_context_workgraph *context, ID3D12Resource *output,
        uint32_t *values, unsigned int count)
{
    struct resource_readback rb;
    unsigned int i;

    transition_resource_state(context->context.list, output,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
            context->context.queue, context->context.list);
    reset_command_list(context->context.list, context->context.allocator);
    for (i = 0; i < count; i++)
        values[i] = get_readback_uint(&rb, i, 0, 0);
    release_resource_readback(&rb);
}

/* The records of multi_input_count are 256 bytes. Record i of a buffer has the value (i % 7) + 1 in its first
 * dword and the buffer's tail value in its last dword. */
#define WG_COUNT_RECORD_SIZE 256u

static ID3D12Resource *workgraph_create_count_records(struct test_context_workgraph *context,
        unsigned int count, unsigned int stride, uint32_t tail)
{
    ID3D12Resource *buffer;
    unsigned int i;
    uint8_t *data;
    size_t size;

    size = (size_t)count * stride;
    data = calloc(1, size);
    for (i = 0; i < count; i++)
    {
        uint32_t value = (i % 7) + 1;

        memcpy(data + (size_t)i * stride, &value, sizeof(value));
        memcpy(data + (size_t)i * stride + WG_COUNT_RECORD_SIZE - 4, &tail, sizeof(tail));
    }
    buffer = create_upload_buffer(context->context.device, size, data);
    free(data);
    return buffer;
}

struct workgraph_count_input
{
    unsigned int entry; /* 0: EntryA, 1: EntryB. */
    unsigned int first; /* First record of the buffer. */
    unsigned int count;
    bool repeat; /* Stride 0: record first is repeated count times. */
};

/* Adds the launch count and the sums of the inputs to expected[0..2] (EntryA) and expected[4..6] (EntryB). */
static void workgraph_count_expected(const struct workgraph_count_input *input, unsigned int times, uint32_t tail,
        uint32_t *expected)
{
    uint32_t *e = expected + 4 * input->entry;
    uint64_t value = 0;
    unsigned int i;

    if (input->repeat)
        value = (uint64_t)input->count * ((input->first % 7) + 1);
    else
    {
        for (i = 0; i < input->count; i++)
            value += ((input->first + i) % 7) + 1;
    }

    e[0] += input->count * times;
    e[1] += (uint32_t)(value * times);
    e[2] += input->count * tail * times;
}

void test_workgraph_multi_gpu_input_capacity(void)
{
    /* D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT must launch every record of every input (WorkGraphs.md,
     * D3D12_MULTI_NODE_GPU_INPUT). A compute-only graph declares no maximum number of input records
     * (SetMaximumInputRecords applies only to graphs with graphics nodes). The records are 256 bytes.
     * With WG_MULTI_INPUT_SMALL, the amdgpu-wddm engine has a staging range of 1024 records, an input of more than
     * 8 records is large, 4 large inputs per entry point are read in place, and the classify pass has 128 threads.
     * The cases cross each of these limits: one large input, more large inputs than direct slots, a merge slot that
     * fills the staging range exactly, two entry points that share the staging range, many inputs of one record
     * (many classify passes and copy batches), and input arrays with a stride of 0. */
#define WG_CAPACITY_BUFFER_RECORDS 4096u
#define WG_CAPACITY_MAX_INPUTS 1024u
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    struct workgraph_capacity_group
    {
        unsigned int inputs;
        unsigned int records; /* Records of each input. */
        unsigned int entry; /* 0: EntryA, 1: EntryB, 2: EntryA and EntryB in turn. */
    };
    static const struct
    {
        const char *name;
        struct workgraph_capacity_group groups[2];
        bool padded; /* Use the buffer with a stride of 260 bytes. */
        bool repeat; /* Record stride 0: each input repeats its first record. */
        unsigned int array_repeat; /* If not 0: NumNodeInputs, with an input array stride of 0. */
    }
    tests[] =
    {
        { "one large input", { { 1, 3000, 0 } } },
        { "one large input, padded stride", { { 1, 3000, 0 } }, true },
        { "one large input, stride 0", { { 1, 3000, 0 } }, false, true },
        { "five large inputs", { { 5, 200, 0 } } },
        { "five large inputs per entry point", { { 5, 100, 0 }, { 5, 100, 1 } } },
        { "merge fills the staging range", { { 128, 8, 0 } } },
        { "large inputs and a full merge", { { 4, 300, 0 }, { 128, 8, 0 } } },
        { "two entry points share the staging range", { { 64, 8, 0 }, { 64, 8, 1 } } },
        { "many inputs of one record", { { 1000, 1, 2 } } },
        { "many inputs of one record, padded stride", { { 1000, 1, 2 } }, true },
        { "input array stride 0", { { 1, 3, 0 } }, false, false, 300 },
        { "input array stride 0, large input", { { 1, 3000, 0 } }, false, false, 4 },
    };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct workgraph_count_input *inputs;
    D3D12_NODE_GPU_INPUT *node_inputs;
    struct workgraph_multi_input multi;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    ID3D12Resource *records[2], *buffer;
    uint32_t expected[8], values[8];
    unsigned int i, j, k, n, first;
    D3D12_PROGRAM_IDENTIFIER ident;
    unsigned int inputs_count;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    unsigned int stride;
    UINT entry_index[2];
    ID3D12StateObject *pso;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    workgraph_set_multi_input_config(WG_MULTI_INPUT_SMALL);
    hr = try_create_workgraph_pso(&context, multi_input_count_dxil, NULL, &pso);
    workgraph_set_multi_input_config(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    records[0] = workgraph_create_count_records(&context, WG_CAPACITY_BUFFER_RECORDS, WG_COUNT_RECORD_SIZE, 1);
    records[1] = workgraph_create_count_records(&context, WG_CAPACITY_BUFFER_RECORDS, WG_COUNT_RECORD_SIZE + 4, 1);
    inputs = calloc(WG_CAPACITY_MAX_INPUTS, sizeof(*inputs));
    node_inputs = calloc(WG_CAPACITY_MAX_INPUTS, sizeof(*node_inputs));

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (!workgraph_case_enabled(tests[i].name))
            continue;
        vkd3d_test_set_context("%s", tests[i].name);

        /* The inputs take consecutive records of the buffer. */
        inputs_count = 0;
        first = 0;
        for (j = 0; j < ARRAY_SIZE(tests[i].groups); j++)
        {
            for (k = 0; k < tests[i].groups[j].inputs; k++)
            {
                inputs[inputs_count].entry = tests[i].groups[j].entry == 2 ? k & 1 : tests[i].groups[j].entry;
                inputs[inputs_count].first = first;
                inputs[inputs_count].count = tests[i].groups[j].records;
                inputs[inputs_count].repeat = tests[i].repeat;
                first += tests[i].groups[j].records;
                inputs_count++;
            }
        }
        ok(first <= WG_CAPACITY_BUFFER_RECORDS && inputs_count <= WG_CAPACITY_MAX_INPUTS, "Case too large.\n");

        buffer = records[tests[i].padded];
        stride = tests[i].padded ? WG_COUNT_RECORD_SIZE + 4 : WG_COUNT_RECORD_SIZE;
        memset(expected, 0, sizeof(expected));
        for (j = 0; j < inputs_count; j++)
        {
            node_inputs[j].EntrypointIndex = entry_index[inputs[j].entry];
            node_inputs[j].NumRecords = inputs[j].count;
            node_inputs[j].Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(buffer) +
                    (UINT64)inputs[j].first * stride;
            node_inputs[j].Records.StrideInBytes = inputs[j].repeat ? 0 : stride;
            workgraph_count_expected(&inputs[j], max(tests[i].array_repeat, 1u), 1, expected);
        }

        n = tests[i].array_repeat ? tests[i].array_repeat : inputs_count;
        workgraph_create_multi_input(&context, node_inputs, inputs_count, n, tests[i].array_repeat ? 0 : 32, &multi);

        scratch = workgraph_create_backing_memory(&context, &wg_reqs);
        output = create_default_buffer(context.context.device, 4 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        dispatch_desc.MultiNodeGPUInput = multi.va;
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, wg_reqs.MinSizeInBytes,
                output, &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        for (j = 0; j < ARRAY_SIZE(values); j++)
            ok(values[j] == expected[j], "Value %u: expected %u, got %u.\n", j, expected[j], values[j]);

        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(multi.buffer);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    free(inputs);
    free(node_inputs);
    ID3D12Resource_Release(records[0]);
    ID3D12Resource_Release(records[1]);
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
#undef WG_CAPACITY_BUFFER_RECORDS
#undef WG_CAPACITY_MAX_INPUTS
}

/* Adds value to a 64-bit count in two 32-bit words, as add_merged() in cs_workgraph_multi_gpu_input_prepare.comp. */
static void workgraph_math_add_merged(uint32_t *lo, uint32_t *hi, uint64_t value)
{
    uint32_t value_lo = (uint32_t)value, value_hi = (uint32_t)(value >> 32);
    uint32_t old = *lo;

    *lo = old + value_lo;
    value_hi += wg_math_add_carry(old, value_lo);
    *hi += value_hi;
}

/* The classify pass of n inputs of records records each, with threads invocations: invocation t takes the inputs
 * t, t + threads, ... and adds its sum once. Returns the 64-bit count of the two words. */
static uint64_t workgraph_math_classify(uint64_t n, uint32_t records, uint32_t threads)
{
    uint32_t lo = 0, hi = 0;
    uint64_t taken;
    uint32_t t;

    for (t = 0; t < threads; t++)
    {
        taken = n > t ? (n - t + threads - 1) / threads : 0;
        if (taken)
            workgraph_math_add_merged(&lo, &hi, taken * records);
    }
    return ((uint64_t)hi << 32) | lo;
}

void test_workgraph_multi_gpu_input_count_math(void)
{
    /* The count arithmetic of the GPU input passes (cs_workgraph_multi_gpu_input_math.h), checked on the CPU for
     * counts near 2^32 and their 64-bit sums. A dispatch of such counts would launch billions of threads. */
    static const struct
    {
        uint32_t count, divider, expected;
    }
    div_tests[] =
    {
        { 0, 5, 0 },
        { 1, 32, 1 },
        { 32, 32, 1 },
        { 33, 32, 2 },
        { 0xffffffffu, 1, 0xffffffffu },
        { 0xffffffffu, 2, 0x80000000u },
        { 0xffffffffu, 32, 0x08000000u },
        { 0xffffffe1u, 32, 0x07ffffffu + 1 },
        { 0xffffffffu, 0xffffffffu, 1 },
    };
    static const struct
    {
        uint32_t records, stride, large;
        bool expected;
    }
    large_tests[] =
    {
        { 2048, 256, 524288, false },
        { 2049, 256, 524288, true },
        { 0xffffffffu, 256, 524288, true },
        { 0xffffffffu, 0xfffffffcu, 524288, true },
        { 131072, 0, 524288, false }, /* An empty record counts as 4 bytes. */
        { 131073, 0, 524288, true },
        { 512, 4, 2048, false },
        { 513, 4, 2048, true },
    };
    static const struct
    {
        uint64_t merged;
        uint32_t stride;
        uint64_t room;
        uint64_t expected;
    }
    merge_tests[] =
    {
        { 1024, 256, 262144, 1024 },
        { 1025, 256, 262144, 1024 },
        { 100, 256, 25599, 99 },
        { 0x100000000ull, 0, 0, 0xffffffffu }, /* NumRecords is 32 bits. */
        { 0xffffffffull * 0xffffffffull, 0, 0, 0xffffffffu },
        { 0xffffffffu, 0, 0, 0xffffffffu },
        { 1ull << 40, 4, 32u * 1024 * 1024, 8u * 1024 * 1024 },
        { 0xffffffffu, 4, 0xffffffffull * 4, 0xffffffffu },
        { 5, 260, 0, 0 },
    };
    static const struct
    {
        uint32_t old, fit, num, expected;
    }
    reserve_tests[] =
    {
        { 0, 10, 3, 3 },
        { 9, 10, 3, 1 },
        { 10, 10, 3, 0 },
        { 11, 10, 1, 0 },
        { 0, 0xffffffffu, 0xffffffffu, 0xffffffffu },
        { 0xfffffff0u, 0xffffffffu, 0xffffffffu, 0xf },
        { 0xffffffffu, 0xffffffffu, 1, 0 },
    };
    static const struct
    {
        uint64_t inputs;
        uint32_t records, threads;
        uint64_t expected;
    }
    sum_tests[] =
    {
        { 2, 0x80000000u, 65536, 0x100000000ull },
        { 0xffffffffu, 1, 65536, 0xffffffffull },
        { 0xffffffffu, 1, 128, 0xffffffffull },
        { 0xffffffffu, 0xffffffffu, 65536, 0xffffffffull * 0xffffffffull },
        { 0x1000000u, 0x100u, 65536, 0x100000000ull },
        { 3, 0xffffffffu, 1, 0x2fffffffdull },
    };
    uint32_t lo, hi, cursor, count;
    unsigned int i;
    uint64_t sum;

    for (i = 0; i < ARRAY_SIZE(div_tests); i++)
    {
        vkd3d_test_set_context("Divide %u", i);
        ok(wg_math_div_round_up(div_tests[i].count, div_tests[i].divider) == div_tests[i].expected,
                "%u / %u: expected %u, got %u.\n", div_tests[i].count, div_tests[i].divider, div_tests[i].expected,
                wg_math_div_round_up(div_tests[i].count, div_tests[i].divider));
    }

    for (i = 0; i < ARRAY_SIZE(large_tests); i++)
    {
        vkd3d_test_set_context("Large %u", i);
        ok(wg_math_is_large(large_tests[i].records, large_tests[i].stride, large_tests[i].large)
                == large_tests[i].expected, "Unexpected result.\n");
    }

    for (i = 0; i < ARRAY_SIZE(merge_tests); i++)
    {
        vkd3d_test_set_context("Merge %u", i);
        ok(wg_math_merge_count(merge_tests[i].merged, merge_tests[i].stride, merge_tests[i].room)
                == merge_tests[i].expected, "Expected %"PRIu64", got %"PRIu64".\n", merge_tests[i].expected,
                wg_math_merge_count(merge_tests[i].merged, merge_tests[i].stride, merge_tests[i].room));
    }

    vkd3d_test_set_context("Room and offsets");
    ok(wg_math_room(262144, 0) == 262144, "Unexpected room.\n");
    ok(wg_math_room(262144, 262144) == 0, "Unexpected room.\n");
    ok(wg_math_room(262144, 262160) == 0, "Unexpected room.\n");
    ok(wg_math_next_offset(0, 0, 256) == 0, "Unexpected offset.\n");
    ok(wg_math_next_offset(0, 1, 4) == 16, "Unexpected offset.\n");
    ok(wg_math_next_offset(16, 3, 260) == 800, "Unexpected offset.\n");
    ok(wg_math_next_offset(0, 0xffffffffu, 0xfffffffcu) == ((0xffffffffull * 0xfffffffcull + 15) & ~15ull),
            "Unexpected offset.\n");

    for (i = 0; i < ARRAY_SIZE(reserve_tests); i++)
    {
        vkd3d_test_set_context("Reserve %u", i);
        ok(wg_math_reserve_count(reserve_tests[i].old, reserve_tests[i].fit, reserve_tests[i].num)
                == reserve_tests[i].expected, "Expected %u, got %u.\n", reserve_tests[i].expected,
                wg_math_reserve_count(reserve_tests[i].old, reserve_tests[i].fit, reserve_tests[i].num));
    }

    /* Many reservations, then reservations of 2^32 - 1 records: the cursor stops at the fit and does not wrap. */
    vkd3d_test_set_context("Cursor");
    cursor = 0;
    for (i = 0; i < 2000000; i++)
    {
        count = wg_math_reserve_count(cursor, 0xfffffff0u, i < 1000000 ? 3 : 0xffffffffu);
        cursor += count;
        if (cursor > 0xfffffff0u)
            break;
    }
    ok(cursor == 0xfffffff0u, "Expected cursor 0xfffffff0, got %#x.\n", cursor);

    for (i = 0; i < ARRAY_SIZE(sum_tests); i++)
    {
        vkd3d_test_set_context("Sum %u", i);
        sum = workgraph_math_classify(sum_tests[i].inputs, sum_tests[i].records, sum_tests[i].threads);
        ok(sum == sum_tests[i].expected, "Expected %"PRIu64", got %"PRIu64".\n", sum_tests[i].expected, sum);
    }

    /* Carries in the low word. */
    vkd3d_test_set_context("Carry");
    lo = 0xffffffffu;
    hi = 0;
    workgraph_math_add_merged(&lo, &hi, 1);
    ok(lo == 0 && hi == 1, "Expected 0x100000000, got %#x%08x.\n", hi, lo);
    workgraph_math_add_merged(&lo, &hi, 0x1ffffffffull);
    ok(lo == 0xffffffffu && hi == 2, "Expected 0x2ffffffff, got %#x%08x.\n", hi, lo);
    ok(wg_math_add_carry(0x80000000u, 0x80000000u) == 1, "Expected a carry.\n");
    ok(wg_math_add_carry(0x7fffffffu, 0x80000000u) == 0, "Expected no carry.\n");
    vkd3d_test_set_context(NULL);
}

void test_workgraph_multi_gpu_input_counts(void)
{
    /* Launch counts of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT with empty records. empty_count64 counts its
     * launches in 64 bits. The inputs come from an input array with a stride of 32, or of 0 to repeat one input.
     * With WG_MULTI_INPUT_SMALL, the classify pass of the amdgpu-wddm engine has 128 threads, and an input of more
     * than 512 empty records is large. Each case launches at most 30000 threads; the arithmetic of counts near
     * 2^32 is checked by test_workgraph_multi_gpu_input_count_math. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EmptyEntry", 0 } };
    static const struct
    {
        const char *name;
        UINT num_node_inputs;
        UINT64 input_stride;
        UINT num_records[2];
        uint64_t expected;
    }
    tests[] =
    {
        { "two inputs", 2, 32, { 1000, 2000 }, 3000 },
        { "4096 inputs of one record", 4096, 0, { 1 }, 4096 },
        { "65536 inputs of no record", 65536, 0, { 0 }, 0 },
        { "six large inputs", 6, 0, { 5000 }, 30000 },
        { "large and small inputs", 2, 32, { 5000, 3 }, 5003 },
    };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_multi_input multi;
    D3D12_NODE_GPU_INPUT node_inputs[2];
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12StateObject *pso;
    uint32_t values[2];
    UINT entry_index;
    unsigned int i;
    uint64_t count;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    workgraph_set_multi_input_config(WG_MULTI_INPUT_SMALL);
    hr = try_create_workgraph_pso(&context, empty_count64_dxil, NULL, &pso);
    workgraph_set_multi_input_config(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, &entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (!workgraph_case_enabled(tests[i].name))
            continue;
        vkd3d_test_set_context("%s", tests[i].name);

        memset(node_inputs, 0, sizeof(node_inputs));
        node_inputs[0].EntrypointIndex = entry_index;
        node_inputs[0].NumRecords = tests[i].num_records[0];
        node_inputs[1].EntrypointIndex = entry_index;
        node_inputs[1].NumRecords = tests[i].num_records[1];
        workgraph_create_multi_input(&context, node_inputs, ARRAY_SIZE(node_inputs), tests[i].num_node_inputs,
                tests[i].input_stride, &multi);

        scratch = workgraph_create_backing_memory(&context, &wg_reqs);
        output = create_default_buffer(context.context.device, 4 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        dispatch_desc.MultiNodeGPUInput = multi.va;
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, wg_reqs.MinSizeInBytes,
                output, &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        count = values[0] | ((uint64_t)values[1] << 32);
        ok(count == tests[i].expected, "Expected %"PRIu64" launches, got %"PRIu64".\n", tests[i].expected, count);

        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(multi.buffer);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

out:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* amdgpu-wddm fork, lab flavour: GetPrivateData of a work graph state object with this GUID gives the
 * struct workgraph_lost_records of all its executions so far. The engine has the same GUID
 * (AMDGPU_WDDM_WG_STATUS_GUID in libs/vkd3d/workgraphs.c). */
static const GUID workgraph_status_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x05 } };

struct workgraph_lost_records
{
    uint64_t events; /* DispatchGraph calls that lost records. */
    uint64_t records; /* Records that these calls did not launch. */
    uint64_t lower_bound; /* 1 if records is a lower bound (a count that is not valid was clamped). */
};

void test_workgraph_multi_gpu_input_lost_records(void)
{
    /* The merge slot of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT has a fixed staging range in the backing memory.
     * Records that do not fit are not launched. The amdgpu-wddm engine must then report the loss: an ERR line,
     * and in the lab flavour the counts through GetPrivateData. With "staging=4096", 16 records of 256 bytes fit.
     * The cases launch at most 200 records. Native drivers launch all records, so only the sums are checked
     * there. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const struct
    {
        const char *name;
        unsigned int inputs; /* Inputs of one record each, to EntryA. */
        uint64_t lost_events, lost_records;
    }
    tests[] =
    {
        { "16 records fit", 16, 0, 0 },
        { "100 records, 84 lost", 100, 1, 84 },
        { "200 records, 184 lost", 200, 2, 268 },
    };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct workgraph_count_input inputs[200];
    D3D12_NODE_GPU_INPUT node_inputs[200];
    struct workgraph_multi_input multi;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_lost_records lost;
    uint32_t expected[8], values[8];
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch;
    ID3D12Resource *records;
    ID3D12Resource *output;
    UINT entry_index[2];
    ID3D12StateObject *pso;
    unsigned int i, j;
    bool is_engine;
    UINT size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    workgraph_set_multi_input_config("staging=4096");
    hr = try_create_workgraph_pso(&context, multi_input_count_dxil, NULL, &pso);
    workgraph_set_multi_input_config(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    records = workgraph_create_count_records(&context, ARRAY_SIZE(inputs), WG_COUNT_RECORD_SIZE, 1);
    for (i = 0; i < ARRAY_SIZE(inputs); i++)
    {
        inputs[i].entry = 0;
        inputs[i].first = i;
        inputs[i].count = 1;
        inputs[i].repeat = false;
        node_inputs[i].EntrypointIndex = entry_index[0];
        node_inputs[i].NumRecords = 1;
        node_inputs[i].Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(records) +
                (UINT64)i * WG_COUNT_RECORD_SIZE;
        node_inputs[i].Records.StrideInBytes = WG_COUNT_RECORD_SIZE;
    }

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].name);

        memset(expected, 0, sizeof(expected));
        for (j = 0; j < tests[i].inputs; j++)
            workgraph_count_expected(&inputs[j], 1, 1, expected);
        workgraph_create_multi_input(&context, node_inputs, tests[i].inputs, tests[i].inputs, 32, &multi);

        scratch = workgraph_create_backing_memory(&context, &wg_reqs);
        output = create_default_buffer(context.context.device, 4 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        dispatch_desc.MultiNodeGPUInput = multi.va;
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, wg_reqs.MinSizeInBytes,
                output, &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        memset(&lost, 0, sizeof(lost));
        size = sizeof(lost);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &size, &lost);
        if (is_engine)
        {
            /* The engine launches the records that fit, and reports the others. */
            ok(hr == S_OK && size == sizeof(lost), "The engine does not report lost records, hr #%x.\n", (int)hr);
            ok(lost.events == tests[i].lost_events, "Expected %"PRIu64" events with lost records, got %"PRIu64".\n",
                    tests[i].lost_events, lost.events);
            ok(lost.records == tests[i].lost_records, "Expected %"PRIu64" lost records, got %"PRIu64".\n",
                    tests[i].lost_records, lost.records);
            ok(values[0] + (uint32_t)(lost.records - (i ? tests[i - 1].lost_records : 0)) == expected[0],
                    "Expected %u launched and lost records, got %u launched.\n", expected[0], values[0]);
        }
        else
        {
            for (j = 0; j < ARRAY_SIZE(values); j++)
                ok(values[j] == expected[j], "Value %u: expected %u, got %u.\n", j, expected[j], values[j]);
        }

        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(multi.buffer);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    ID3D12Resource_Release(records);
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_multi_gpu_input_backing_size(void)
{
    /* WorkGraphs.md, Discovering backing memory size requirements: a backing memory larger than the minimum can
     * make a work graph execute more efficiently. The amdgpu-wddm engine uses the backing memory above
     * MinSizeInBytes as more staging range for the merge slot of D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT, up to
     * MaxSizeInBytes. With "staging=4096,staging_max=131072", 16 records of 256 bytes fit with MinSizeInBytes,
     * 16 + 256 with one more SizeGranularityInBytes (64 KiB), and 512 with MaxSizeInBytes. Each case gives 600
     * inputs of one record; the engine reports the records that do not fit. Native drivers launch all records,
     * so only the sums are checked there. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const struct
    {
        const char *name;
        unsigned int granules; /* Backing memory: MinSizeInBytes + granules * SizeGranularityInBytes, ~0u: Max. */
        uint64_t lost_events, lost_records; /* Cumulative. */
    }
    tests[] =
    {
        { "minimum, 584 lost", 0, 1, 584 },
        { "minimum + 1 granule, 328 lost", 1, 2, 912 },
        { "maximum, 88 lost", ~0u, 3, 1000 },
    };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct workgraph_count_input inputs[600];
    D3D12_NODE_GPU_INPUT node_inputs[600];
    struct workgraph_multi_input multi;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_lost_records lost;
    uint32_t expected[8], values[8];
    D3D12_PROGRAM_IDENTIFIER ident;
    UINT64 size, max_size;
    ID3D12Resource *scratch;
    ID3D12Resource *records;
    ID3D12Resource *output;
    UINT entry_index[2];
    ID3D12StateObject *pso;
    unsigned int i, j;
    bool is_engine;
    UINT data_size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    workgraph_set_multi_input_config("staging=4096,staging_max=131072");
    hr = try_create_workgraph_pso(&context, multi_input_count_dxil, NULL, &pso);
    workgraph_set_multi_input_config(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    ok(wg_reqs.MaxSizeInBytes >= wg_reqs.MinSizeInBytes, "Max size %"PRIu64" is less than min size %"PRIu64".\n",
            (uint64_t)wg_reqs.MaxSizeInBytes, (uint64_t)wg_reqs.MinSizeInBytes);
    if (is_engine)
    {
        /* The staging range grows by staging_max - staging from Min to Max. */
        ok(wg_reqs.MaxSizeInBytes - wg_reqs.MinSizeInBytes >= 131072 - 4096,
                "Max size %"PRIu64" leaves no room for more staging above min size %"PRIu64".\n",
                (uint64_t)wg_reqs.MaxSizeInBytes, (uint64_t)wg_reqs.MinSizeInBytes);
    }
    /* Native drivers can report a very large maximum. The sums do not depend on the size there. */
    max_size = wg_reqs.MaxSizeInBytes;
    if (!is_engine && max_size > wg_reqs.MinSizeInBytes + 4 * (UINT64)wg_reqs.SizeGranularityInBytes)
        max_size = wg_reqs.MinSizeInBytes + 4 * (UINT64)wg_reqs.SizeGranularityInBytes;

    records = workgraph_create_count_records(&context, ARRAY_SIZE(inputs), WG_COUNT_RECORD_SIZE, 1);
    for (i = 0; i < ARRAY_SIZE(inputs); i++)
    {
        inputs[i].entry = 0;
        inputs[i].first = i;
        inputs[i].count = 1;
        inputs[i].repeat = false;
        node_inputs[i].EntrypointIndex = entry_index[0];
        node_inputs[i].NumRecords = 1;
        node_inputs[i].Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(records) +
                (UINT64)i * WG_COUNT_RECORD_SIZE;
        node_inputs[i].Records.StrideInBytes = WG_COUNT_RECORD_SIZE;
    }

    memset(expected, 0, sizeof(expected));
    for (i = 0; i < ARRAY_SIZE(inputs); i++)
        workgraph_count_expected(&inputs[i], 1, 1, expected);
    workgraph_create_multi_input(&context, node_inputs, ARRAY_SIZE(node_inputs), ARRAY_SIZE(node_inputs), 32, &multi);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].name);

        if (tests[i].granules == ~0u)
            size = max_size;
        else
            size = wg_reqs.MinSizeInBytes + tests[i].granules * (UINT64)wg_reqs.SizeGranularityInBytes;
        if (size > max_size)
            size = max_size;

        scratch = NULL;
        if (size)
            scratch = create_default_buffer(context.context.device, size,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        output = create_default_buffer(context.context.device, 4 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        dispatch_desc.MultiNodeGPUInput = multi.va;
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, size, output, &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        memset(&lost, 0, sizeof(lost));
        data_size = sizeof(lost);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &data_size, &lost);
        if (is_engine)
        {
            /* The engine launches the records that fit, and reports the others. */
            ok(hr == S_OK && data_size == sizeof(lost), "The engine does not report lost records, hr #%x.\n", (int)hr);
            ok(lost.events == tests[i].lost_events, "Expected %"PRIu64" events with lost records, got %"PRIu64".\n",
                    tests[i].lost_events, lost.events);
            ok(lost.records == tests[i].lost_records, "Expected %"PRIu64" lost records, got %"PRIu64".\n",
                    tests[i].lost_records, lost.records);
            ok(values[0] + (uint32_t)(lost.records - (i ? tests[i - 1].lost_records : 0)) == expected[0],
                    "Expected %u launched and lost records, got %u launched.\n", expected[0], values[0]);
        }
        else
        {
            for (j = 0; j < ARRAY_SIZE(values); j++)
                ok(values[j] == expected[j], "Value %u: expected %u, got %u.\n", j, expected[j], values[j]);
        }

        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    ID3D12Resource_Release(multi.buffer);
    ID3D12Resource_Release(records);
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_multi_gpu_input_two_queues(void)
{
    /* Two queues execute the same work graph at the same time, each with its own backing memory, inputs and
     * output (WorkGraphs.md: a work graph can run on several queues in parallel when each execution has its own
     * backing memory). The graph has only leaf entry points. Each execution has one large input and 16 small
     * inputs to EntryA; the records of the two executions have different tail values, so that records that one
     * execution takes from the other change the sums. With WG_MULTI_INPUT_SMALL, the small inputs of the
     * amdgpu-wddm engine are merged into the staging range and the large input is read in place. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const uint32_t tails[2] = { 1, 3 };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    ID3D12CommandAllocator *allocators[2];
    ID3D12GraphicsCommandList *lists[2];
    struct test_context_workgraph context;
    struct workgraph_count_input inputs[17];
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_multi_input multi[2];
    D3D12_NODE_GPU_INPUT node_inputs[17];
    ID3D12Resource *scratch[2], *output[2];
    ID3D12CommandQueue *queues[2];
    uint32_t expected[8], values[8];
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *records[2];
    unsigned int i, j, k, iter;
    ID3D12StateObject *pso;
    UINT entry_index[2];
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    workgraph_set_multi_input_config(WG_MULTI_INPUT_SMALL);
    hr = try_create_workgraph_pso(&context, multi_input_count_dxil, NULL, &pso);
    workgraph_set_multi_input_config(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    /* One input of 3000 records, then 16 inputs of 8 records. */
    inputs[0].entry = 0;
    inputs[0].first = 0;
    inputs[0].count = 3000;
    inputs[0].repeat = false;
    for (i = 1; i < ARRAY_SIZE(inputs); i++)
    {
        inputs[i].entry = 0;
        inputs[i].first = 3000 + (i - 1) * 8;
        inputs[i].count = 8;
        inputs[i].repeat = false;
    }

    for (i = 0; i < 2; i++)
    {
        queues[i] = create_command_queue(context.context.device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        hr = ID3D12Device_CreateCommandAllocator(context.context.device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                &IID_ID3D12CommandAllocator, (void **)&allocators[i]);
        ok(hr == S_OK, "Failed to create command allocator, hr #%x.\n", (int)hr);
        hr = ID3D12Device_CreateCommandList(context.context.device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                allocators[i], NULL, &IID_ID3D12GraphicsCommandList, (void **)&lists[i]);
        ok(hr == S_OK, "Failed to create command list, hr #%x.\n", (int)hr);

        records[i] = workgraph_create_count_records(&context, 3128, WG_COUNT_RECORD_SIZE, tails[i]);
        for (j = 0; j < ARRAY_SIZE(inputs); j++)
        {
            node_inputs[j].EntrypointIndex = entry_index[inputs[j].entry];
            node_inputs[j].NumRecords = inputs[j].count;
            node_inputs[j].Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(records[i]) +
                    (UINT64)inputs[j].first * WG_COUNT_RECORD_SIZE;
            node_inputs[j].Records.StrideInBytes = WG_COUNT_RECORD_SIZE;
        }
        workgraph_create_multi_input(&context, node_inputs, ARRAY_SIZE(inputs), ARRAY_SIZE(inputs), 32, &multi[i]);
        scratch[i] = workgraph_create_backing_memory(&context, &wg_reqs);
    }

    for (iter = 0; iter < 4; iter++)
    {
        vkd3d_test_set_context("Iteration %u", iter);

        for (i = 0; i < 2; i++)
        {
            output[i] = create_default_buffer(context.context.device, 4 * 1024,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            memset(&dispatch_desc, 0, sizeof(dispatch_desc));
            dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
            dispatch_desc.MultiNodeGPUInput = multi[i].va;
            workgraph_record_dispatch(&context, lists[i], &ident, scratch[i], wg_reqs.MinSizeInBytes,
                    output[i], &dispatch_desc);
            hr = ID3D12GraphicsCommandList_Close(lists[i]);
            ok(hr == S_OK, "Failed to close command list, hr #%x.\n", (int)hr);
        }

        /* Both executions are submitted before either is waited for. */
        for (i = 0; i < 2; i++)
            exec_command_list(queues[i], lists[i]);
        for (i = 0; i < 2; i++)
            wait_queue_idle(context.context.device, queues[i]);

        for (i = 0; i < 2; i++)
        {
            memset(expected, 0, sizeof(expected));
            for (j = 0; j < ARRAY_SIZE(inputs); j++)
                workgraph_count_expected(&inputs[j], 1, tails[i], expected);

            workgraph_read_output(&context, output[i], values, ARRAY_SIZE(values));
            for (k = 0; k < ARRAY_SIZE(values); k++)
                ok(values[k] == expected[k], "Queue %u, value %u: expected %u, got %u.\n", i, k, expected[k], values[k]);

            ID3D12Resource_Release(output[i]);
            reset_command_list(lists[i], allocators[i]);
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    for (i = 0; i < 2; i++)
    {
        if (scratch[i])
            ID3D12Resource_Release(scratch[i]);
        ID3D12Resource_Release(multi[i].buffer);
        ID3D12Resource_Release(records[i]);
        ID3D12GraphicsCommandList_Release(lists[i]);
        ID3D12CommandAllocator_Release(allocators[i]);
        ID3D12CommandQueue_Release(queues[i]);
    }
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* Fills count records of WG_COUNT_RECORD_SIZE bytes in CPU memory: record i has the value (i % 7) + 1 in its first
 * dword and tail in its last dword, as workgraph_create_count_records() does in GPU memory. */
static uint8_t *workgraph_create_cpu_count_records(unsigned int count, uint32_t tail)
{
    uint8_t *data;
    unsigned int i;

    data = calloc(count, WG_COUNT_RECORD_SIZE);
    for (i = 0; i < count; i++)
    {
        uint32_t value = (i % 7) + 1;

        memcpy(data + (size_t)i * WG_COUNT_RECORD_SIZE, &value, sizeof(value));
        memcpy(data + (size_t)i * WG_COUNT_RECORD_SIZE + WG_COUNT_RECORD_SIZE - 4, &tail, sizeof(tail));
    }
    return data;
}

void test_workgraph_two_queues_levels(void)
{
    /* Two queues execute the same work graph at the same time, each with its own backing memory, inputs and
     * output (WorkGraphs.md: each set of backing memory is an independent state of work graph execution, and
     * command lists on parallel queues can use the same work graph with different backing memory). The graph has
     * two levels: each entry point sends its record to a node of the second level, which counts it. So the records
     * of the second level are in the payload memory of the execution. The records of the two executions have
     * different tail values, so that records that one execution takes from the other change the sums. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const struct workgraph_count_input inputs[2] = { { 0, 0, 1500 }, { 1, 1500, 700 } };
    static const uint32_t tails[2] = { 1, 3 };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    ID3D12CommandAllocator *allocators[2];
    ID3D12GraphicsCommandList *lists[2];
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    D3D12_NODE_CPU_INPUT cpu_inputs[2][2];
    ID3D12Resource *scratch[2], *output[2];
    ID3D12CommandQueue *queues[2];
    uint32_t expected[8], values[8];
    D3D12_PROGRAM_IDENTIFIER ident;
    unsigned int i, j, k, iter;
    uint8_t *records[2];
    ID3D12StateObject *pso;
    UINT entry_index[2];
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;

    hr = try_create_workgraph_pso(&context, level_count_dxil, NULL, &pso);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    for (i = 0; i < 2; i++)
    {
        queues[i] = create_command_queue(context.context.device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        hr = ID3D12Device_CreateCommandAllocator(context.context.device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                &IID_ID3D12CommandAllocator, (void **)&allocators[i]);
        ok(hr == S_OK, "Failed to create command allocator, hr #%x.\n", (int)hr);
        hr = ID3D12Device_CreateCommandList(context.context.device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                allocators[i], NULL, &IID_ID3D12GraphicsCommandList, (void **)&lists[i]);
        ok(hr == S_OK, "Failed to create command list, hr #%x.\n", (int)hr);

        records[i] = workgraph_create_cpu_count_records(2200, tails[i]);
        for (j = 0; j < ARRAY_SIZE(inputs); j++)
        {
            cpu_inputs[i][j].EntrypointIndex = entry_index[inputs[j].entry];
            cpu_inputs[i][j].NumRecords = inputs[j].count;
            cpu_inputs[i][j].pRecords = records[i] + (size_t)inputs[j].first * WG_COUNT_RECORD_SIZE;
            cpu_inputs[i][j].RecordStrideInBytes = WG_COUNT_RECORD_SIZE;
        }
        scratch[i] = workgraph_create_backing_memory(&context, &wg_reqs);
    }

    for (iter = 0; iter < 4; iter++)
    {
        vkd3d_test_set_context("Iteration %u", iter);

        for (i = 0; i < 2; i++)
        {
            output[i] = create_default_buffer(context.context.device, 4 * 1024,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            memset(&dispatch_desc, 0, sizeof(dispatch_desc));
            dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
            dispatch_desc.MultiNodeCPUInput.NumNodeInputs = ARRAY_SIZE(inputs);
            dispatch_desc.MultiNodeCPUInput.pNodeInputs = cpu_inputs[i];
            dispatch_desc.MultiNodeCPUInput.NodeInputStrideInBytes = sizeof(cpu_inputs[i][0]);
            workgraph_record_dispatch(&context, lists[i], &ident, scratch[i], wg_reqs.MinSizeInBytes,
                    output[i], &dispatch_desc);
            hr = ID3D12GraphicsCommandList_Close(lists[i]);
            ok(hr == S_OK, "Failed to close command list, hr #%x.\n", (int)hr);
        }

        /* Both executions are submitted before either is waited for. */
        for (i = 0; i < 2; i++)
            exec_command_list(queues[i], lists[i]);
        for (i = 0; i < 2; i++)
            wait_queue_idle(context.context.device, queues[i]);

        for (i = 0; i < 2; i++)
        {
            memset(expected, 0, sizeof(expected));
            for (j = 0; j < ARRAY_SIZE(inputs); j++)
                workgraph_count_expected(&inputs[j], 1, tails[i], expected);

            workgraph_read_output(&context, output[i], values, ARRAY_SIZE(values));
            for (k = 0; k < ARRAY_SIZE(values); k++)
                ok(values[k] == expected[k], "Queue %u, value %u: expected %u, got %u.\n", i, k, expected[k], values[k]);

            ID3D12Resource_Release(output[i]);
            reset_command_list(lists[i], allocators[i]);
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    for (i = 0; i < 2; i++)
    {
        if (scratch[i])
            ID3D12Resource_Release(scratch[i]);
        free(records[i]);
        ID3D12GraphicsCommandList_Release(lists[i]);
        ID3D12CommandAllocator_Release(allocators[i]);
        ID3D12CommandQueue_Release(queues[i]);
    }
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* The lab flavour of the amdgpu-wddm engine gives the canary ranges of a work graph (AMDGPU_WDDM_WG_LIMITS=canary=1)
 * through GetPrivateData with this GUID (amdgpu_wddm_wg_canary_guid in libs/vkd3d/workgraphs.c). */
static const GUID workgraph_canary_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x06 } };

struct workgraph_canaries
{
    uint32_t count;
    uint32_t pattern;
    struct
    {
        uint64_t offset;
        uint64_t size;
    } ranges[3], sinks[2]; /* sinks[i] is followed by ranges[i]. */
};

#define WG_CANARY_SIZE 4096u

/* True if [offset, offset + size) is a nonempty range of 4-byte words inside a backing memory of backing_size bytes.
 * offset + size is not computed, so that the check cannot wrap. */
static bool workgraph_range_is_inside(uint64_t offset, uint64_t size, uint64_t backing_size)
{
    return size && !(offset & 3) && !(size & 3) && offset <= backing_size && size <= backing_size - offset;
}

/* Checks the answer of the engine to the canary GetPrivateData call: the call succeeded with the full structure,
 * there are 3 canary ranges of WG_CANARY_SIZE bytes and 2 sinks, all nonempty, aligned, inside the backing memory
 * and without overlap, and the canary range of each sink follows it directly. A case that writes into the sinks runs
 * only if this is true. */
static bool workgraph_canaries_are_valid(HRESULT hr, UINT size, const struct workgraph_canaries *canaries,
        uint64_t backing_size)
{
    uint64_t offsets[5], sizes[5];
    unsigned int i, j;

    if (hr != S_OK || size != sizeof(*canaries) || canaries->count != 3)
        return false;
    for (i = 0; i < 3; i++)
    {
        offsets[i] = canaries->ranges[i].offset;
        sizes[i] = canaries->ranges[i].size;
        if (sizes[i] != WG_CANARY_SIZE)
            return false;
    }
    for (i = 0; i < 2; i++)
    {
        offsets[3 + i] = canaries->sinks[i].offset;
        sizes[3 + i] = canaries->sinks[i].size;
    }
    for (i = 0; i < 5; i++)
    {
        if (!workgraph_range_is_inside(offsets[i], sizes[i], backing_size))
            return false;
    }
    /* All ranges are inside the backing memory, so the sums below cannot wrap. */
    for (i = 0; i < 5; i++)
    {
        for (j = i + 1; j < 5; j++)
        {
            if (offsets[i] < offsets[j] + sizes[j] && offsets[j] < offsets[i] + sizes[i])
                return false;
        }
    }
    for (i = 0; i < 2; i++)
    {
        if (canaries->sinks[i].offset + canaries->sinks[i].size != canaries->ranges[i].offset)
            return false;
    }
    return true;
}

/* Reads size bytes at offset of a buffer in the COMMON state. */
static void workgraph_read_buffer_range(struct test_context_workgraph *context, ID3D12Resource *buffer,
        UINT64 offset, UINT64 size, uint32_t *data)
{
    ID3D12Resource *readback;
    D3D12_RANGE range;
    void *ptr;
    HRESULT hr;

    readback = create_readback_buffer(context->context.device, size);
    ID3D12GraphicsCommandList_CopyBufferRegion(context->context.list, readback, 0, buffer, offset, size);
    hr = ID3D12GraphicsCommandList_Close(context->context.list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr #%x.\n", (int)hr);
    exec_command_list(context->context.queue, context->context.list);
    wait_queue_idle(context->context.device, context->context.queue);
    reset_command_list(context->context.list, context->context.allocator);

    range.Begin = 0;
    range.End = size;
    hr = ID3D12Resource_Map(readback, 0, &range, &ptr);
    ok(SUCCEEDED(hr), "Failed to map readback buffer, hr #%x.\n", (int)hr);
    if (SUCCEEDED(hr))
    {
        memcpy(data, ptr, size);
        range.End = 0;
        ID3D12Resource_Unmap(readback, 0, &range);
    }
    else
    {
        memset(data, 0, size);
    }
    ID3D12Resource_Release(readback);
}

/* The number of words of size bytes at offset of a buffer that are not pattern. */
static unsigned int workgraph_count_damaged_words(struct test_context_workgraph *context, ID3D12Resource *buffer,
        UINT64 offset, UINT64 size, uint32_t pattern)
{
    unsigned int i, damaged = 0;
    uint32_t *data;

    data = malloc(size);
    workgraph_read_buffer_range(context, buffer, offset, size, data);
    for (i = 0; i < size / sizeof(uint32_t); i++)
        damaged += data[i] != pattern;
    free(data);
    return damaged;
}

/* The lab flavour of the amdgpu-wddm engine reads AMDGPU_WDDM_WG_LIMITS when it creates a work graph: a small
 * payload ring and a small limit of packed offset entries let a test fill the payload memory of a level with a
 * few thousand records. Other engines ignore the variable. */
static void workgraph_set_limits(const char *value)
{
#ifdef _WIN32
    SetEnvironmentVariableA("AMDGPU_WDDM_WG_LIMITS", value);
#else
    if (value)
        setenv("AMDGPU_WDDM_WG_LIMITS", value, 1);
    else
        unsetenv("AMDGPU_WDDM_WG_LIMITS");
#endif
}

/* A case in which the engine writes node outputs into the payload sinks runs only in a runner with a time bound,
 * which sets VKD3D_TEST_WORKGRAPH_BOUNDED=1 (lab-kit wg-run.ps1, run-one-host.sh, run-tests-host.sh), and only when
 * the engine gives the canary ranges before the dispatch. */
static bool workgraph_is_bounded_runner(void)
{
    const char *env = getenv("VKD3D_TEST_WORKGRAPH_BOUNDED");

    return env && !strcmp(env, "1");
}

/* Gets the canary ranges of a state object with backing_size bytes of backing memory, before its dispatch. Returns
 * false, with no ranges, when a case that writes into the payload sinks must not run. */
static bool workgraph_get_sink_canaries(ID3D12StateObject *pso, struct workgraph_canaries *canaries,
        uint64_t backing_size)
{
    UINT size = sizeof(*canaries);
    HRESULT hr;

    memset(canaries, 0, sizeof(*canaries));
    hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_canary_guid, &size, canaries);
    if (workgraph_canaries_are_valid(hr, size, canaries, backing_size))
        return true;
    ok(false, "No valid canary ranges, hr #%x, size %u, count %u.\n", (int)hr, size, canaries->count);
    memset(canaries, 0, sizeof(*canaries));
    skip("The case that writes into the sinks does not run.\n");
    return false;
}

/* Checks that the canary ranges in the backing memory were not written. */
static void workgraph_check_canaries(struct test_context_workgraph *context, ID3D12Resource *scratch,
        const struct workgraph_canaries *canaries)
{
    unsigned int damaged, w;

    for (w = 0; w < canaries->count && w < ARRAY_SIZE(canaries->ranges); w++)
    {
        damaged = workgraph_count_damaged_words(context, scratch, canaries->ranges[w].offset,
                canaries->ranges[w].size, canaries->pattern);
        ok(!damaged, "Canary range %u at %#"PRIx64": %u words were written.\n",
                w, canaries->ranges[w].offset, damaged);
    }
}

void test_workgraph_level_output_limits(void)
{
    /* The node outputs of a level go to the payload memory of the execution. The amdgpu-wddm engine has a payload
     * ring of fixed size per level, so outputs can be more than it holds. They must then not be written past the
     * ring: the engine launches the records that fit, and reports the others. EntryA sends each of its 1536 records
     * as one 128-byte record to ChildA, which counts it. With "ring=65536", 512 records fit. With "packed=8", 8
     * allocations fit; a thread launch node allocates once per wave, so 8 x 32 or 8 x 64 records fit. Native drivers
     * launch all records, so only the sums are checked there. On the engine the two cases with limits write into the
     * payload sinks, so they run only in a bounded runner and with the canary ranges (workgraph_is_bounded_runner). */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const struct workgraph_count_input input = { 0, 0, 1536 };
    static const struct
    {
        const char *limits;
        unsigned int fit; /* 0: 8 allocations of one wave each. */
    }
    tests[] =
    {
        { NULL, 1536 },
        { "ring=65536,canary=1", 512 },
        { "packed=8,canary=1", 0 },
    };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_canaries canaries;
    struct workgraph_lost_records lost;
    uint32_t expected[8], values[8];
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12StateObject *pso;
    UINT entry_index[2];
    uint8_t *records;
    unsigned int i, j;
    bool is_engine;
    UINT size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);
    records = workgraph_create_cpu_count_records(input.count, 1);

    memset(expected, 0, sizeof(expected));
    workgraph_count_expected(&input, 1, 1, expected);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("%s", tests[i].limits ? tests[i].limits : "default");
        if (is_engine && tests[i].limits && !workgraph_is_bounded_runner())
        {
            skip("Set VKD3D_TEST_WORKGRAPH_BOUNDED=1 in a runner with a time bound to run this case.\n");
            continue;
        }

        workgraph_set_limits(tests[i].limits);
        hr = try_create_workgraph_pso(&context, level_count_dxil, NULL, &pso);
        workgraph_set_limits(NULL);
        ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        if (!pso)
            continue;
        if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        {
            ID3D12StateObject_Release(pso);
            continue;
        }
        memset(&canaries, 0, sizeof(canaries));
        if (is_engine && tests[i].limits && !workgraph_get_sink_canaries(pso, &canaries, wg_reqs.MinSizeInBytes))
        {
            ID3D12StateObject_Release(pso);
            continue;
        }

        scratch = workgraph_create_backing_memory(&context, &wg_reqs);
        output = create_default_buffer(context.context.device, 4 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index[0];
        dispatch_desc.NodeCPUInput.NumRecords = input.count;
        dispatch_desc.NodeCPUInput.pRecords = records;
        dispatch_desc.NodeCPUInput.RecordStrideInBytes = WG_COUNT_RECORD_SIZE;
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, wg_reqs.MinSizeInBytes,
                output, &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        memset(&lost, 0, sizeof(lost));
        size = sizeof(lost);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &size, &lost);
        if (is_engine)
        {
            /* The records that fit are launched with their own data: the tail sum is the launch count. */
            ok(hr == S_OK && size == sizeof(lost), "The engine does not report lost records, hr #%x.\n", (int)hr);
            if (tests[i].fit)
                ok(values[0] == tests[i].fit, "Expected %u launches, got %u.\n", tests[i].fit, values[0]);
            else
                ok(values[0] == 8 * 32 || values[0] == 8 * 64, "Expected 8 waves of launches, got %u.\n", values[0]);
            ok(values[2] == values[0], "Expected tail sum %u, got %u.\n", values[0], values[2]);
            ok(values[1] >= values[0] && values[1] <= 7 * values[0], "Value sum %u is out of range for %u launches.\n",
                    values[1], values[0]);
            ok(lost.records == input.count - values[0], "Expected %u lost records, got %"PRIu64".\n",
                    input.count - values[0], lost.records);
            ok(lost.events == (values[0] == input.count ? 0 : 1), "Got %"PRIu64" events with lost records.\n",
                    lost.events);
            if (scratch)
                workgraph_check_canaries(&context, scratch, &canaries);
        }
        else
        {
            for (j = 0; j < ARRAY_SIZE(values); j++)
                ok(values[j] == expected[j], "Value %u: expected %u, got %u.\n", j, expected[j], values[j]);
        }

        if (scratch)
            ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12StateObject_Release(pso);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    free(records);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_output_bounds(void)
{
    /* The node output allocations of a level must stay inside the payload memory, also when they do not fit.
     * 64 thread groups of 64 threads each allocate group records (one allocation of 16 KiB per group) and one
     * thread record per thread (one allocation per wave), concurrently. The consumers check the contents of each
     * record and set one bit per record id, so that a record with wrong contents, a record that arrives twice and
     * a record that should not exist are seen. The amdgpu-wddm engine drops the allocations that do not fit (lab
     * switch AMDGPU_WDDM_WG_LIMITS) and counts their records. For it the test checks:
     * - every record that arrives is correct, and arrives once;
     * - arrived records plus lost records are the records that the groups asked for;
     * - an allocation arrives completely or not at all (the records of a group, or of a wave);
     * - the exact boundary: allocations that end at the capacity fit, one 16-byte granule less does not;
     * - counts above the limit of one allocation (256 records and 64 KiB - 15 bytes) are dropped without a wrap of
     *   count * stride, with a 64-bit lost count; it is exact for a group count, and a lower bound, marked as such,
     *   for a thread count above the limit (each lane counts as the limit + 1);
     * - the canary ranges after the sinks and after the given backing memory are not written.
     * Native drivers launch all records; for them only the cases with valid counts run, and the results and the
     * canary after the backing memory are checked.
     * On the engine, a case in which not all records fit writes into the payload sinks, so it runs only in a bounded
     * runner and with the canary ranges (workgraph_is_bounded_runner). */
    static const D3D12_NODE_ID entry_ids[] = { { u"Producer", 0 } };
    static const struct
    {
        const char *limits;
        uint32_t group_count, thread_count;
        bool engine_only; /* Counts above the declared MaxRecords. */
        int group_fit; /* Group allocations that fit, -1 when the result depends on the order. */
        int thread_fit; /* Lanes whose records fit, -1 when the result depends on the order. */
        uint64_t requested; /* Records that the groups ask for. */
        uint64_t lower_bound; /* 0: arrived + lost = requested. Else the lost count, marked as a lower bound. */
    }
    tests[] =
    {
        { "canary=1", 64, 1, false, 64, 4096, 8192 },
        /* 10 allocations of 16384 bytes end at the capacity. */
        { "ring=163840,canary=1", 64, 0, false, 10, 0, 4096 },
        { "ring=163824,canary=1", 64, 0, false, 9, 0, 4096 },
        { "packed=10,canary=1", 64, 0, false, 10, 0, 4096 },
        /* Group and wave allocations race at the boundary. */
        { "ring=262144,canary=1", 64, 1, false, -1, -1, 8192 },
        /* Nothing fits: all allocations go to the sinks at the same time. */
        { "ring=64,canary=1", 64, 1, false, 0, 0, 8192 },
        /* 0x10000001 records of 256 bytes would wrap to 256 bytes. 64 of them are more than 2^32 records. */
        { "canary=1", 0x10000001, 0, true, 0, 0, 64ull * 0x10000001, 0 },
        /* The sum of 64 lane counts of 0x04000001 would wrap. A lane count above the limit of 256 records counts as
         * 257, so the lost count is the lower bound 4096 * 257. */
        { "canary=1", 0, 0x04000001, true, 0, 0, 4096ull * 0x04000001, 4096ull * 257 },
    };
    unsigned int group_bits[64], thread_bits[64], wave_size, damaged;
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_canaries canaries;
    struct workgraph_lost_records lost;
    uint32_t values[16 + 256], params[2];
    D3D12_PROGRAM_IDENTIFIER ident;
    unsigned int i, j, k, group_ok;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12StateObject *pso;
    uint32_t *tail_pattern;
    UINT64 scratch_size;
    bool is_engine, to_sinks;
    UINT entry_index;
    UINT size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    tail_pattern = malloc(64 * 1024);
    for (i = 0; i < 64 * 1024 / sizeof(uint32_t); i++)
        tail_pattern[i] = 0x7a11c0deu;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (tests[i].engine_only && !is_engine)
            continue;
        vkd3d_test_set_context("Test %u, %s, counts %#x %#x", i, tests[i].limits, tests[i].group_count,
                tests[i].thread_count);
        to_sinks = is_engine && (tests[i].group_fit != 64 ||
                tests[i].thread_fit != (tests[i].thread_count ? 4096 : 0));
        if (to_sinks && !workgraph_is_bounded_runner())
        {
            skip("Set VKD3D_TEST_WORKGRAPH_BOUNDED=1 in a runner with a time bound to run this case.\n");
            continue;
        }

        workgraph_set_limits(tests[i].limits);
        hr = try_create_workgraph_pso(&context, output_bounds_dxil, NULL, &pso);
        workgraph_set_limits(NULL);
        ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        if (!pso)
            continue;
        if (!workgraph_get_program(pso, &ident, &wg_reqs, &entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        {
            ID3D12StateObject_Release(pso);
            continue;
        }

        /* The canary ranges after the payload sinks and after the unrolled offsets, known before the dispatch. */
        memset(&canaries, 0, sizeof(canaries));
        if (is_engine && !workgraph_get_sink_canaries(pso, &canaries, wg_reqs.MinSizeInBytes) && to_sinks)
        {
            ID3D12StateObject_Release(pso);
            continue;
        }

        /* 64 KiB more than the backing memory that the graph gets, with a pattern that nothing may write. */
        scratch_size = wg_reqs.MinSizeInBytes;
        scratch = create_default_buffer(context.context.device, scratch_size + 64 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        upload_buffer_data(scratch, scratch_size, 64 * 1024, tail_pattern, context.context.queue,
                context.context.list);
        reset_command_list(context.context.list, context.context.allocator);

        output = create_default_buffer(context.context.device, sizeof(values),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        params[0] = tests[i].group_count;
        params[1] = tests[i].thread_count;
        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index;
        dispatch_desc.NodeCPUInput.NumRecords = 1;
        dispatch_desc.NodeCPUInput.pRecords = params;
        dispatch_desc.NodeCPUInput.RecordStrideInBytes = sizeof(params);
        workgraph_record_dispatch(&context, context.context.list, &ident, scratch, scratch_size, output,
                &dispatch_desc);
        workgraph_read_output(&context, output, values, ARRAY_SIZE(values));

        ok(values[2] == 0, "%u records have wrong contents.\n", values[2]);
        ok(values[3] == 0, "%u records arrived more than once.\n", values[3]);
        wave_size = values[4];

        /* The records of an allocation arrive together: all 64 of a group, all lanes of a wave. */
        group_ok = 0;
        for (j = 0; j < 64; j++)
        {
            group_bits[j] = vkd3d_popcount(values[16 + 2 * j]) + vkd3d_popcount(values[16 + 2 * j + 1]);
            thread_bits[j] = vkd3d_popcount(values[16 + 128 + 2 * j]) + vkd3d_popcount(values[16 + 128 + 2 * j + 1]);
            ok(group_bits[j] == 0 || group_bits[j] == min(tests[i].group_count, 64u),
                    "Group %u: %u of its group records arrived.\n", j, group_bits[j]);
            group_ok += group_bits[j] != 0;
            if (wave_size == 32 || wave_size == 64)
            {
                for (k = 0; k < 64; k += wave_size)
                {
                    uint64_t lanes = ((uint64_t)values[16 + 128 + 2 * j + 1] << 32) | values[16 + 128 + 2 * j];
                    uint64_t mask = wave_size == 64 ? ~0ull : (0xffffffffull << k);

                    lanes &= mask;
                    ok(!lanes || lanes == mask, "Group %u, wave at %u: thread records %#"PRIx64" arrived.\n",
                            j, k, lanes);
                }
            }
        }
        ok(wave_size == 32 || wave_size == 64, "Unexpected wave size %u.\n", wave_size);

        /* Nothing is written past the backing memory that the graph got. */
        damaged = workgraph_count_damaged_words(&context, scratch, scratch_size, 64 * 1024, 0x7a11c0deu);
        ok(!damaged, "%u words after the backing memory were written.\n", damaged);

        if (is_engine)
        {
            memset(&lost, 0, sizeof(lost));
            size = sizeof(lost);
            hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &size, &lost);
            ok(hr == S_OK && size == sizeof(lost), "The engine does not report lost records, hr #%x.\n", (int)hr);
            if (tests[i].lower_bound)
            {
                ok(lost.records == tests[i].lower_bound && lost.lower_bound == 1,
                        "Expected the lower bound %"PRIu64" of %"PRIu64" records, got %"PRIu64" (lower bound %"PRIu64").\n",
                        tests[i].lower_bound, tests[i].requested, lost.records, lost.lower_bound);
                ok(!values[0] && !values[1], "%u group and %u thread records arrived.\n", values[0], values[1]);
            }
            else
            {
                ok(values[0] + values[1] + lost.records == tests[i].requested,
                        "%u group and %u thread records arrived and %"PRIu64" were lost, expected %"PRIu64" in total.\n",
                        values[0], values[1], lost.records, tests[i].requested);
                ok(!lost.lower_bound, "The lost count is a lower bound.\n");
            }
            ok(lost.events == (lost.records ? 1 : 0), "Got %"PRIu64" events with lost records.\n", lost.events);
            if (tests[i].group_fit >= 0)
                ok(group_ok == (unsigned int)tests[i].group_fit, "Expected %d group allocations, got %u.\n",
                        tests[i].group_fit, group_ok);
            if (tests[i].thread_fit >= 0)
                ok(values[1] == (unsigned int)tests[i].thread_fit, "Expected %d thread records, got %u.\n",
                        tests[i].thread_fit, values[1]);
            if (tests[i].group_fit < 0)
                ok(values[0] + values[1] && lost.records, "Expected a partial result, got %u + %u, lost %"PRIu64".\n",
                        values[0], values[1], lost.records);

            workgraph_check_canaries(&context, scratch, &canaries);
        }
        else
        {
            ok(values[0] == 64 * min(tests[i].group_count, 64u), "Expected %u group records, got %u.\n",
                    64 * min(tests[i].group_count, 64u), values[0]);
            ok(values[1] == (tests[i].thread_count ? 4096u : 0u), "Expected %u thread records, got %u.\n",
                    tests[i].thread_count ? 4096u : 0u, values[1]);
        }

        ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(output);
        ID3D12StateObject_Release(pso);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);

    free(tail_pattern);
    destroy_workgraph_test_context(&context);
}

/* Runs one DispatchGraph of an entry point with one CPU record, for the tests of the node output bounds. The lab
 * switch AMDGPU_WDDM_WG_LIMITS is set to limits while the state object is created. On the engine, a case that can
 * write into the payload sinks (to_sinks) runs only in a bounded runner and with valid canary ranges; the canary
 * ranges are then checked. On all drivers 64 KiB after the backing memory are checked. If magic is not 0, the words
 * equal to magic in each payload sink are counted (engine only). Returns false if the case did not run. */
struct workgraph_case_result
{
    uint32_t values[16 + 512];
    struct workgraph_lost_records lost;
    unsigned int sink_words[2];
};

static unsigned int workgraph_count_equal_words(struct test_context_workgraph *context, ID3D12Resource *buffer,
        UINT64 offset, UINT64 size, uint32_t value)
{
    unsigned int i, count = 0;
    uint32_t *data;

    data = malloc(size);
    workgraph_read_buffer_range(context, buffer, offset, size, data);
    for (i = 0; i < size / sizeof(uint32_t); i++)
        count += data[i] == value;
    free(data);
    return count;
}

static bool workgraph_run_bounded_case(struct test_context_workgraph *context, D3D12_SHADER_BYTECODE dxil,
        const D3D12_NODE_ID *entry_ids, unsigned int entry_count, unsigned int entry, const char *limits,
        const void *record, UINT record_size, bool to_sinks, uint32_t magic, struct workgraph_case_result *result)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_canaries canaries;
    D3D12_PROGRAM_IDENTIFIER ident;
    unsigned int damaged, i;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    uint32_t *tail_pattern;
    ID3D12StateObject *pso;
    UINT entry_index[4];
    UINT64 scratch_size;
    bool is_engine;
    UINT size;
    HRESULT hr;

    memset(result, 0, sizeof(*result));
    is_engine = workgraph_is_vkd3d_proton(context->device);
    if (is_engine && to_sinks && !workgraph_is_bounded_runner())
    {
        skip("Set VKD3D_TEST_WORKGRAPH_BOUNDED=1 in a runner with a time bound to run this case.\n");
        return false;
    }

    workgraph_set_limits(limits);
    hr = try_create_workgraph_pso(context, dxil, NULL, &pso);
    workgraph_set_limits(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
        return false;
    if (entry_count > ARRAY_SIZE(entry_index)
            || !workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, entry_count))
    {
        ID3D12StateObject_Release(pso);
        return false;
    }

    scratch_size = wg_reqs.MinSizeInBytes;
    memset(&canaries, 0, sizeof(canaries));
    if (is_engine && !workgraph_get_sink_canaries(pso, &canaries, scratch_size) && to_sinks)
    {
        ID3D12StateObject_Release(pso);
        return false;
    }

    /* 64 KiB more than the backing memory that the graph gets, with a pattern that nothing may write. */
    tail_pattern = malloc(64 * 1024);
    for (i = 0; i < 64 * 1024 / sizeof(uint32_t); i++)
        tail_pattern[i] = 0x7a11c0deu;
    scratch = create_default_buffer(context->context.device, scratch_size + 64 * 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    upload_buffer_data(scratch, scratch_size, 64 * 1024, tail_pattern, context->context.queue,
            context->context.list);
    reset_command_list(context->context.list, context->context.allocator);
    free(tail_pattern);

    output = create_default_buffer(context->context.device, sizeof(result->values),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    memset(&dispatch_desc, 0, sizeof(dispatch_desc));
    dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index[entry];
    dispatch_desc.NodeCPUInput.NumRecords = 1;
    dispatch_desc.NodeCPUInput.pRecords = (void *)record;
    dispatch_desc.NodeCPUInput.RecordStrideInBytes = record_size;
    workgraph_record_dispatch(context, context->context.list, &ident, scratch, scratch_size, output, &dispatch_desc);
    workgraph_read_output(context, output, result->values, ARRAY_SIZE(result->values));

    damaged = workgraph_count_damaged_words(context, scratch, scratch_size, 64 * 1024, 0x7a11c0deu);
    ok(!damaged, "%u words after the backing memory were written.\n", damaged);

    if (is_engine)
    {
        size = sizeof(result->lost);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &size, &result->lost);
        ok(hr == S_OK && size == sizeof(result->lost), "The engine does not report lost records, hr #%x.\n", (int)hr);
        workgraph_check_canaries(context, scratch, &canaries);
        if (magic && canaries.count)
        {
            for (i = 0; i < ARRAY_SIZE(canaries.sinks); i++)
                result->sink_words[i] = workgraph_count_equal_words(context, scratch, canaries.sinks[i].offset,
                        canaries.sinks[i].size, magic);
        }
    }

    ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12StateObject_Release(pso);
    return true;
}

void test_workgraph_output_counts(void)
{
    /* GroupIncrementOutputCount and ThreadIncrementOutputCount add empty records. Empty records do not count against
     * the MaxRecords limit of a thread group, so a valid count can be large; the engine must launch all records that
     * fit in the payload memory of the level (an empty record takes 4 bytes there, for its unrolled offset), and
     * drop and count only the others. The payload is reserved with a compare/exchange loop, so the payload counter
     * cannot pass the capacity: with equal reservations that race at the capacity, exactly capacity / size of them
     * fit. Each case launches at most 1048640 records (case 2). The cases with "ring=" write no records into the sinks
     * (empty records have no payload), but run in a bounded runner like the other cases with limits. */
    static const D3D12_NODE_ID entry_ids[] = { { u"Producer", 0 } };
    static const struct
    {
        const char *limits;
        uint32_t groups, group_count, thread_count;
        uint32_t group_fit, thread_fit; /* Records that the engine launches. */
    }
    tests[] =
    {
        /* 16384 was the limit of one increment before. */
        { "canary=1", 1, 16384, 0, 16384, 0 },
        { "canary=1", 1, 16385, 0, 16385, 0 },
        { "canary=1", 1, 0, 16385, 0, 64 * 16385 },
        /* 65536 bytes hold 16384 empty records: exactly 16384 fit, one more does not. */
        { "ring=65536,canary=1", 1, 16384, 0, 16384, 0 },
        { "ring=65536,canary=1", 1, 16385, 0, 0, 0 },
        /* 8 group reservations of 16 KiB: 4 fit. */
        { "ring=65536,canary=1", 8, 4096, 0, 4 * 4096, 0 },
        /* 4096 thread reservations of 20 bytes race: 65536 / 20 = 3276 fit. */
        { "ring=65536,canary=1", 64, 0, 5, 0, 3276 * 5 },
    };
    struct test_context_workgraph context;
    struct workgraph_case_result result;
    uint32_t params[3];
    uint64_t requested;
    unsigned int i;
    bool is_engine;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("Test %u, %s, %u groups, counts %u %u", i, tests[i].limits, tests[i].groups,
                tests[i].group_count, tests[i].thread_count);
        params[0] = tests[i].groups;
        params[1] = tests[i].group_count;
        params[2] = tests[i].thread_count;
        if (!workgraph_run_bounded_case(&context, output_counts_dxil, entry_ids, 1, 0, tests[i].limits, params,
                sizeof(params), true, 0, &result))
            continue;

        requested = (uint64_t)tests[i].groups * (tests[i].group_count + 64ull * tests[i].thread_count);
        if (is_engine)
        {
            ok(result.values[0] == tests[i].group_fit, "Expected %u group records, got %u.\n",
                    tests[i].group_fit, result.values[0]);
            ok(result.values[1] == tests[i].thread_fit, "Expected %u thread records, got %u.\n",
                    tests[i].thread_fit, result.values[1]);
            ok(result.values[0] + result.values[1] + result.lost.records == requested,
                    "%u + %u records arrived and %"PRIu64" were lost, expected %"PRIu64" in total.\n",
                    result.values[0], result.values[1], result.lost.records, requested);
            ok(result.lost.events == (result.lost.records ? 1 : 0), "Got %"PRIu64" events with lost records.\n",
                    result.lost.events);
            ok(!result.lost.lower_bound, "The lost count is a lower bound.\n");
        }
        else
        {
            ok(result.values[0] == tests[i].groups * tests[i].group_count, "Expected %u group records, got %u.\n",
                    tests[i].groups * tests[i].group_count, result.values[0]);
            ok(result.values[1] == tests[i].groups * 64 * tests[i].thread_count,
                    "Expected %u thread records, got %u.\n", tests[i].groups * 64 * tests[i].thread_count,
                    result.values[1]);
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_output_index_bounds(void)
{
    /* The record count of an entry of the packed offsets has 8 bits, so an allocation of more than 256 records
     * (which is not valid) must be dropped also when its bytes fit, and an allocation of 256 records must be
     * launched. A write to a record index at or past the count of an allocation is not valid either; the engine
     * clamps the record index to the records of one allocation, so that the write stays in the payload memory and
     * its sink. For the cases that are not valid only the bounds are checked: the canary ranges, 64 KiB after the
     * backing memory, and that the device is not removed. The wave of ThreadProducer has the largest lane offset,
     * so its write past the count ends farthest after the allocation: with "ring=16384" the 64 allocated records
     * end at the capacity, and the write with index 1000 is clamped to index 254, 81152 bytes after the wave start.
     * Each case launches at most 16384 records. */
    static const D3D12_NODE_ID entry_ids[] = { { u"GroupProducer", 0 }, { u"ThreadProducer", 0 } };
    static const struct
    {
        unsigned int entry;
        const char *limits;
        uint32_t count, bad_index;
        bool valid;
        uint32_t small_fit, large_fit; /* Records that the engine launches. */
        uint64_t lost; /* Records that the engine reports as lost. */
    }
    tests[] =
    {
        { 0, "canary=1", 256, 0, true, 64 * 256, 0, 0 },
        /* 257 records of 4 bytes fit in the bytes of one allocation, but not in the count of an entry. */
        { 0, "canary=1", 257, 0, false, 0, 0, 64 * 257 },
        /* Dropped allocations, then writes with an index past the count. */
        { 0, "canary=1", 0x10000001, 300, false, 0, 0, 64ull * 0x10000001 },
        { 0, "canary=1", 0x10000001, 0x7fffffff, false, 0, 0, 64ull * 0x10000001 },
        /* Allocations that fit, then a write past the count of the last one. */
        { 0, "ring=1024,canary=1", 1, 70000, false, 64, 0, 0 },
        { 1, "ring=16384,canary=1", 1, 0, true, 0, 64, 0 },
        { 1, "ring=16384,canary=1", 1, 1000, false, 0, 64, 0 },
    };
    struct test_context_workgraph context;
    struct workgraph_case_result result;
    uint32_t params[2];
    unsigned int i;
    bool is_engine;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (!tests[i].valid && !is_engine)
            continue;
        vkd3d_test_set_context("Test %u, %s, count %#x, index %#x", i, tests[i].limits, tests[i].count,
                tests[i].bad_index);
        params[0] = tests[i].count;
        params[1] = tests[i].bad_index;
        if (!workgraph_run_bounded_case(&context, output_index_bounds_dxil, entry_ids, ARRAY_SIZE(entry_ids),
                tests[i].entry, tests[i].limits, params, sizeof(params), true, 0, &result))
            continue;

        ok(result.values[0] == tests[i].small_fit, "Expected %u small records, got %u.\n",
                tests[i].small_fit, result.values[0]);
        ok(result.values[1] == tests[i].large_fit, "Expected %u large records, got %u.\n",
                tests[i].large_fit, result.values[1]);
        if (tests[i].valid)
        {
            ok(result.values[2] == 0, "%u records have a wrong id.\n", result.values[2]);
            ok(result.values[3] == 0, "%u records arrived more than once.\n", result.values[3]);
        }
        if (is_engine)
        {
            ok(result.lost.records == tests[i].lost, "Expected %"PRIu64" lost records, got %"PRIu64".\n",
                    tests[i].lost, result.lost.records);
            ok(!result.lost.lower_bound, "The lost count is a lower bound.\n");
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_output_zero_counts(void)
{
    /* An allocation of 0 records is valid, and a group or a thread can make any number of them. Such an allocation
     * must reserve no payload memory and no packed offset entry, otherwise the work graph would need unbounded memory
     * for valid work. Each case makes 4096 allocations of 0 records and then one allocation of 64 records of 4 bytes,
     * with "ring=256" (the 256 bytes of the 64 records) and a packed offset limit of one entry for each allocation of
     * the 64 records (2 for the thread case, which takes one allocation per wave and has 2 waves with wave32). All 64
     * records must arrive. The controls show that the limits apply: with one allocation of 1 record first, the
     * allocation of 63 records does not fit in 256 bytes (16 + 256 bytes) or in a packed offset limit of 1. Each case
     * launches at most 64 records. */
    static const D3D12_NODE_ID entry_ids[] = { { u"GroupProducer", 0 }, { u"ThreadProducer", 0 } };
    static const struct
    {
        unsigned int entry;
        const char *limits;
        uint32_t calls, zero, count;
        uint32_t fit; /* Records that the engine launches. */
    }
    tests[] =
    {
        { 0, "ring=256,packed=1,canary=1", 0, 0, 64, 64 },
        { 0, "ring=256,packed=1,canary=1", 4096, 0, 64, 64 },
        { 0, "ring=256,canary=1", 1, 1, 63, 1 },
        { 0, "ring=1024,packed=1,canary=1", 1, 1, 63, 1 },
        { 1, "ring=256,packed=2,canary=1", 0, 0, 1, 64 },
        { 1, "ring=256,packed=2,canary=1", 4096, 0, 1, 64 },
    };
    struct test_context_workgraph context;
    struct workgraph_case_result result;
    uint32_t params[3], requested;
    unsigned int i;
    bool is_engine;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        vkd3d_test_set_context("Test %u, %s, %u calls of %u, count %u", i, tests[i].limits, tests[i].calls,
                tests[i].zero, tests[i].count);
        params[0] = tests[i].calls;
        params[1] = tests[i].zero;
        params[2] = tests[i].count;
        if (!workgraph_run_bounded_case(&context, output_zero_counts_dxil, entry_ids, ARRAY_SIZE(entry_ids),
                tests[i].entry, tests[i].limits, params, sizeof(params), true, 0, &result))
            continue;

        requested = tests[i].entry ? 64 * tests[i].count : tests[i].calls * tests[i].zero + tests[i].count;
        ok(result.values[0] == (is_engine ? tests[i].fit : requested), "Expected %u records, got %u.\n",
                is_engine ? tests[i].fit : requested, result.values[0]);
        ok(result.values[1] == 0, "%u records have a wrong id.\n", result.values[1]);
        ok(result.values[2] == 0, "%u records arrived more than once.\n", result.values[2]);
        if (is_engine)
        {
            ok(result.lost.records == requested - tests[i].fit, "Expected %u lost records, got %"PRIu64".\n",
                    requested - tests[i].fit, result.lost.records);
            ok(!result.lost.lower_bound, "The lost count is a lower bound.\n");
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_level_sinks(void)
{
    /* The outputs of level 1 go to the other payload ring than the outputs of level 0, with its own sink. 64 thread
     * groups of level 1 each allocate 16 KiB of Leaf records; with "ring=163840" 10 allocations fit. The others must
     * go to the sink of the second ring (payload ring 1), not to the ring of level 0 or past the sink. The test
     * checks the records that arrive (contents, once each, all or none of an allocation), the exact lost count, the
     * canary ranges, and that the Leaf records of the dropped allocations are in the sink of payload ring 1 and not
     * in the sink of payload ring 0. Without limits all 4096 records arrive. Each case launches at most 4096 Leaf
     * records. */
    static const D3D12_NODE_ID entry_ids[] = { { u"Entry", 0 } };
    static const struct
    {
        const char *limits;
        unsigned int fit; /* Allocations of 64 Leaf records that fit. */
    }
    tests[] =
    {
        { "canary=1", 64 },
        { "ring=163840,canary=1", 10 },
    };
    struct test_context_workgraph context;
    struct workgraph_case_result result;
    unsigned int i, j, bits, groups;
    uint32_t params = 0;
    bool is_engine;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    is_engine = workgraph_is_vkd3d_proton(context.device);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (tests[i].fit != 64 && !is_engine)
            continue;
        vkd3d_test_set_context("Test %u, %s", i, tests[i].limits);
        if (!workgraph_run_bounded_case(&context, level_sinks_dxil, entry_ids, 1, 0, tests[i].limits, &params,
                sizeof(params), tests[i].fit != 64, 0x51ab51abu, &result))
            continue;

        ok(result.values[0] == 64 * tests[i].fit, "Expected %u records, got %u.\n", 64 * tests[i].fit,
                result.values[0]);
        ok(result.values[2] == 0, "%u records have wrong contents.\n", result.values[2]);
        ok(result.values[3] == 0, "%u records arrived more than once.\n", result.values[3]);
        groups = 0;
        for (j = 0; j < 64; j++)
        {
            /* The 64 records of the allocation of group j have the ids j * 64 to j * 64 + 63. */
            bits = vkd3d_popcount(result.values[16 + 2 * j]) + vkd3d_popcount(result.values[16 + 2 * j + 1]);
            ok(bits == 0 || bits == 64, "Allocation %u: %u of its records arrived.\n", j, bits);
            groups += bits != 0;
        }
        ok(groups == tests[i].fit, "Expected %u allocations, got %u.\n", tests[i].fit, groups);
        if (is_engine)
        {
            ok(result.lost.records == 64 * (64 - tests[i].fit), "Expected %u lost records, got %"PRIu64".\n",
                    64 * (64 - tests[i].fit), result.lost.records);
            ok(!result.lost.lower_bound, "The lost count is a lower bound.\n");
            if (tests[i].fit != 64)
            {
                ok(result.sink_words[1] >= 64, "Expected Leaf records in the sink of payload ring 1, found %u.\n",
                        result.sink_words[1]);
                ok(!result.sink_words[0], "Found %u Leaf words in the sink of payload ring 0.\n",
                        result.sink_words[0]);
            }
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_canary_admission(void)
{
    /* workgraph_canaries_are_valid decides if a case that writes into the payload sinks may run. It must refuse every
     * answer of the engine that does not describe 3 canary ranges of 4096 bytes and 2 sinks, each nonempty, 4-byte
     * aligned, inside the backing memory and without overlap, with the canary of each sink right after it. CPU only,
     * no device. */
    static const uint64_t backing = 1024 * 1024;
    struct workgraph_canaries good, c;
    unsigned int i;

    memset(&good, 0, sizeof(good));
    good.count = 3;
    good.pattern = 0xc4a7c0deu;
    good.sinks[0].offset = 0x10000;
    good.sinks[0].size = 0x20000;
    good.ranges[0].offset = 0x30000;
    good.sinks[1].offset = 0x40000;
    good.sinks[1].size = 0x20000;
    good.ranges[1].offset = 0x60000;
    good.ranges[2].offset = 0x80000;
    for (i = 0; i < 3; i++)
        good.ranges[i].size = 4096;

    ok(workgraph_canaries_are_valid(S_OK, sizeof(good), &good, backing), "A valid answer was refused.\n");
    ok(!workgraph_canaries_are_valid(E_INVALIDARG, sizeof(good), &good, backing), "A failed call was accepted.\n");
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(good) - 8, &good, backing), "A short answer was accepted.\n");
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(good), &good, 0x80800), "A range past the backing was accepted.\n");

    c = good; c.count = 2;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "Two ranges were accepted.\n");
    c = good; c.ranges[2].size = 0;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "An empty range was accepted.\n");
    c = good; c.ranges[2].size = 4092;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "A short range was accepted.\n");
    c = good; c.ranges[2].offset = 0x80002;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "An unaligned range was accepted.\n");
    c = good; c.ranges[2].offset = UINT64_MAX - 100;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "A range that wraps was accepted.\n");
    c = good; c.ranges[2].offset = 0x30800;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "Overlapping ranges were accepted.\n");
    c = good; c.ranges[2].offset = c.ranges[0].offset;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "Equal ranges were accepted.\n");
    c = good; c.sinks[1].size = 0;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "An empty sink was accepted.\n");
    c = good; c.sinks[1].offset = 0x30000; c.sinks[1].size = 0x30000;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "A sink over a canary was accepted.\n");
    c = good; c.ranges[1].offset = 0x70000;
    ok(!workgraph_canaries_are_valid(S_OK, sizeof(c), &c, backing), "A canary apart from its sink was accepted.\n");
}

#define WG_PREDICATE_NONE (~0u)

struct workgraph_predicate_step
{
    /* Byte offset of the predicate in the predicate buffer (0: value 0, 8: value 1), or WG_PREDICATE_NONE for
     * SetPredication(NULL). */
    unsigned int predicate;
    D3D12_PREDICATION_OP op;
};

void test_workgraph_predication(void)
{
    /* SetPredication applies to DispatchGraph as to Dispatch. vkd3d begins the predicate of SetPredication only at
     * the next action command when SetPredication is the first action command of a list (render pass resume).
     * Each case runs with SetPredication as the first command after the reset ("deferred") and after a copy
     * ("direct"); the two must give the same result. 37 launches add 37. */
    static const struct
    {
        const char *name;
        struct workgraph_predicate_step steps[4];
        unsigned int step_count;
        uint32_t expected;
    }
    tests[] =
    {
        {"none", {{WG_PREDICATE_NONE, D3D12_PREDICATION_OP_EQUAL_ZERO}}, 1, 37},
        {"equal zero, value 0", {{0, D3D12_PREDICATION_OP_EQUAL_ZERO}}, 1, 0},
        {"equal zero, value 1", {{8, D3D12_PREDICATION_OP_EQUAL_ZERO}}, 1, 37},
        {"not equal zero, value 1", {{8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}}, 1, 0},
        {"not equal zero, value 0", {{0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}}, 1, 37},
        /* One DispatchGraph per step in one list: skipped, launched, skipped, then without predication. */
        {"false true false none", {{0, D3D12_PREDICATION_OP_EQUAL_ZERO}, {8, D3D12_PREDICATION_OP_EQUAL_ZERO},
                {8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}, {WG_PREDICATE_NONE, D3D12_PREDICATION_OP_EQUAL_ZERO}}, 4, 74},
    };
    static const uint64_t predicate_values[2] = {0, 1};
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    ID3D12Resource *predicates, *copy_src, *copy_dst;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct test_context_workgraph context;
    ID3D12GraphicsCommandList10 *list10;
    D3D12_SET_PROGRAM_DESC program_desc;
    ID3D12GraphicsCommandList *list;
    D3D12_PROGRAM_IDENTIFIER ident;
    unsigned int i, j, direct;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12StateObject *pso;
    uint32_t values[16];
    UINT entry_index;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    list = context.context.list;

    hr = try_create_workgraph_pso(&context, empty_entry_dxil, NULL, &pso);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (FAILED(hr))
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    {
        static const D3D12_NODE_ID entry_id = {u"EmptyEntry", 0};

        if (!workgraph_get_program(pso, &ident, &wg_reqs, &entry_index, &entry_id, 1))
        {
            ID3D12StateObject_Release(pso);
            destroy_workgraph_test_context(&context);
            return;
        }
    }

    hr = ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&list10);
    ok(SUCCEEDED(hr), "Failed to query ID3D12GraphicsCommandList10, hr #%x.\n", (int)hr);
    if (FAILED(hr))
    {
        ID3D12StateObject_Release(pso);
        destroy_workgraph_test_context(&context);
        return;
    }

    predicates = create_upload_buffer(context.context.device, sizeof(predicate_values), predicate_values);
    copy_src = create_default_buffer(context.context.device, 256,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_dst = create_default_buffer(context.context.device, 256,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    scratch = workgraph_create_backing_memory(&context, &wg_reqs);

    memset(&dispatch_desc, 0, sizeof(dispatch_desc));
    dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index;
    dispatch_desc.NodeCPUInput.NumRecords = 37;

    memset(&program_desc, 0, sizeof(program_desc));
    program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    if (scratch)
    {
        program_desc.WorkGraph.BackingMemory.SizeInBytes = wg_reqs.MinSizeInBytes;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(scratch);
    }
    program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    program_desc.WorkGraph.ProgramIdentifier = ident;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        for (direct = 0; direct < 2; direct++)
        {
            vkd3d_test_set_context("%s, %s", tests[i].name, direct ? "direct" : "deferred");
            output = create_default_buffer(context.context.device, sizeof(values),
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

            /* The list was reset just before. A copy is an action command, so SetPredication after it begins
             * the predicate at once. */
            if (direct)
                ID3D12GraphicsCommandList_CopyBufferRegion(list, copy_dst, 0, copy_src, 0, 256);

            for (j = 0; j < tests[i].step_count; j++)
            {
                if (tests[i].steps[j].predicate == WG_PREDICATE_NONE)
                    ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                else
                    ID3D12GraphicsCommandList_SetPredication(list, predicates, tests[i].steps[j].predicate,
                            tests[i].steps[j].op);

                ID3D12GraphicsCommandList_SetComputeRootSignature(list, context.default_root_uav_rs);
                ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0,
                        ID3D12Resource_GetGPUVirtualAddress(output));
                ID3D12GraphicsCommandList10_SetProgram(list10, &program_desc);
                ID3D12GraphicsCommandList10_DispatchGraph(list10, &dispatch_desc);
                uav_barrier(list, NULL);
            }
            ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

            workgraph_read_output(&context, output, values, ARRAY_SIZE(values));
            ok(values[0] == tests[i].expected, "Expected %u launches, got %u.\n", tests[i].expected, values[0]);
            ID3D12Resource_Release(output);
        }
    }
    vkd3d_test_set_context(NULL);

    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(copy_dst);
    ID3D12Resource_Release(copy_src);
    ID3D12Resource_Release(predicates);
    ID3D12GraphicsCommandList10_Release(list10);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* The lab flavour of the amdgpu-wddm engine gives the record pools of the draining scheduler through GetPrivateData
 * with this GUID (amdgpu_wddm_wg_pool_guid in libs/vkd3d/workgraphs.c). The last byte is the version of the struct. */
static const GUID workgraph_pool_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x09 } };

#define WG_POOL_LEVELS 32
#define WG_POOL_PAYLOAD 0
#define WG_POOL_ENTRIES 1
#define WG_POOL_OFFSETS 2
#define WG_POOL_COMPONENTS 3

struct workgraph_pool_region
{
    uint64_t offset;
    uint64_t size;
};

/* struct amdgpu_wddm_wg_pools of the engine. Pool L (1 <= L < pools) holds the input records of level L. w[L] is W of
 * pool L: payload bytes, then packed entries and unrolled offsets in records. bound[L] is the largest demand on pool L
 * of one record of level L - 1. The layouts are for Min - 16 bytes, Min, Min + 64 KiB + 8 bytes, Max and Max + 64 KiB;
 * used is 0 if there is no layout for that size. */
#define WG_POOL_LAYOUTS 5

struct workgraph_pools
{
    uint32_t valid;
    uint32_t pools;
    uint32_t execution;
    uint32_t padding;
    uint64_t fixed_size;
    uint64_t staging_size;
    uint64_t sink_size;
    uint64_t min_size;
    uint64_t max_size;
    uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS];
    uint64_t bound[WG_POOL_LEVELS][WG_POOL_COMPONENTS];
    struct workgraph_pool_layout
    {
        uint64_t size;
        uint64_t used;
        struct workgraph_pool_region fixed;
        struct workgraph_pool_region entries;
        struct workgraph_pool_region offsets;
        struct workgraph_pool_region empty_state;
        struct workgraph_pool_region payload[WG_POOL_LEVELS][2];
        struct workgraph_pool_region sink[WG_POOL_LEVELS][2];
        struct workgraph_pool_region staging;
    } layouts[WG_POOL_LAYOUTS];
};

/* The lab flavour of the amdgpu-wddm engine gives the round frame of a work graph through GetPrivateData with this
 * GUID (amdgpu_wddm_wg_drain_guid in libs/vkd3d/workgraphs.c). */
static const GUID workgraph_drain_guid =
        { 0x5c2e9a41, 0x7d3b, 0x4f0e, { 0x9a, 0x61, 0x2b, 0x8e, 0x4d, 0x17, 0xc3, 0x0b } };

struct workgraph_drain_info
{
    uint32_t rounds; /* Extra rounds after round 1 (AMDGPU_WDDM_WG_LIMITS=rounds=<n>). */
    uint32_t pending_rounds; /* The pending dispatches of the rounds below this report waiting work (pending=<k>). */
    uint64_t control_offset; /* struct workgraph_drain_control in the backing memory. */
    uint32_t abandoned_events;
    uint32_t abandoned_reasons; /* Bit 1 << reason of each close that abandoned work. */
    uint32_t abandoned_last_reason;
    uint32_t abandoned_flags;
    uint32_t hook_calls; /* Calls of the predication hook (AMDGPU_WDDM_WG_FAIL=drain-hook). */
    uint32_t violations; /* Engine blocks that found the predicate of SetPredication active. */
    uint64_t abandoned_records; /* Records that the closes left in the record pools. */
    uint64_t pool_state_offset; /* pool_states of struct vkd3d_workgraph_pool_state, pool_state_stride bytes apart. */
    uint32_t pool_state_stride;
    uint32_t pool_states;
};

#define WG_DRAIN_MAX_ROUNDS 64u
#define WG_DRAIN_REASON_COMPLETE 0u
#define WG_DRAIN_REASON_EXHAUSTED 1u
#define WG_DRAIN_REASON_FAULT 2u
#define WG_DRAIN_ABANDONED 1u
#define WG_DRAIN_NO_STALL (~0u)

/* struct vkd3d_workgraph_drain_control of the engine. */
struct workgraph_drain_control
{
    uint32_t pending[WG_DRAIN_MAX_ROUNDS + 1];
    uint32_t executed[3];
    uint32_t fault;
    uint32_t closes;
    uint32_t close_reason;
    uint32_t admitted; /* A record was admitted in the round of the pending word below. */
    uint32_t waiting; /* The pools held records at the last pending word. */
    uint32_t reserved[3];
};

/* Checks the control block after one DispatchGraph with "rounds" extra rounds and the pending hook pending_rounds.
 * Round 1 is r = 0. run is false if the predicate of SetPredication skipped the call. stall is the round of
 * "drain-stall:<r>", or WG_DRAIN_NO_STALL. Returns the close reason. */
static unsigned int workgraph_check_drain_control(const struct workgraph_drain_control *control,
        unsigned int rounds, unsigned int pending_rounds, unsigned int stall, bool run)
{
    unsigned int r, last, reason;
    bool executed, fault = false;
    uint32_t expected;

    /* Round r + 1 runs if pending[r] is 1, which the hook makes true for r < pending_rounds. A stall ends the
     * rounds in its own round. */
    last = pending_rounds < rounds ? pending_rounds : rounds;
    if (run && stall <= last)
    {
        last = stall;
        fault = true;
    }

    for (r = 0; r <= WG_DRAIN_MAX_ROUNDS; r++)
    {
        executed = run && r <= last;
        ok(!!(control->executed[r / 32] & (1u << (r % 32))) == executed, "Round %u: expected executed %u.\n",
                r, executed);
        expected = executed && r < pending_rounds && !(fault && r == last);
        ok(control->pending[r] == expected, "Round %u: expected pending %u, got %u.\n", r, expected,
                control->pending[r]);
    }

    if (fault)
        reason = WG_DRAIN_REASON_FAULT;
    else if (run && last == rounds && rounds < pending_rounds)
        reason = WG_DRAIN_REASON_EXHAUSTED;
    else
        reason = WG_DRAIN_REASON_COMPLETE;
    ok(control->fault == fault, "Expected fault %u, got %u.\n", fault, control->fault);
    ok(control->closes == 1, "Expected 1 close, got %u.\n", control->closes);
    ok(control->close_reason == reason, "Expected close reason %u, got %u.\n", reason, control->close_reason);
    return reason;
}

/* Checks a reply of GetPrivateData with workgraph_drain_guid before the test uses it. min_size is the size of the
 * backing memory of the calls. Returns NULL if the reply is valid, else the reason. */
static const char *workgraph_drain_info_error(HRESULT hr, UINT size, const struct workgraph_drain_info *info,
        unsigned int rounds, unsigned int pending_rounds, uint64_t min_size)
{
    if (hr != S_OK)
        return "no reply";
    if (size != sizeof(*info))
        return "wrong size";
    if (info->rounds != rounds)
        return "wrong number of extra rounds";
    if (info->pending_rounds != pending_rounds)
        return "wrong pending hook";
    /* The control block is only in the backing memory if there are extra rounds. */
    if (!rounds)
        return NULL;
    if (info->control_offset % sizeof(uint32_t))
        return "control block not aligned";
    /* A subtraction, because control_offset + size can wrap. */
    if (min_size < sizeof(struct workgraph_drain_control) ||
            info->control_offset > min_size - sizeof(struct workgraph_drain_control))
        return "control block not inside the backing memory";
    return NULL;
}

/* VKD3D_TEST_WORKGRAPH_DRAIN=frame requires the lab diagnostics of the draining scheduler (the round frame and the
 * pools), "none" requires that there are none (the release flavour or a native driver). If it is not set, they are
 * required on the amdgpu-wddm engine. Returns false, after a failure, if the value or the device is wrong. */
static bool workgraph_drain_expected(ID3D12Device5 *device, bool *frame)
{
    const char *env = getenv("VKD3D_TEST_WORKGRAPH_DRAIN");
    bool is_engine = workgraph_is_vkd3d_proton(device);

    if (!env || !env[0])
        *frame = is_engine;
    else if (!strcmp(env, "frame") || !strcmp(env, "none"))
        *frame = !strcmp(env, "frame");
    else
    {
        ok(false, "Unknown VKD3D_TEST_WORKGRAPH_DRAIN=%s.\n", env);
        return false;
    }
    if (*frame && !is_engine)
    {
        ok(false, "The draining scheduler diagnostics need the amdgpu-wddm engine.\n");
        return false;
    }
    trace("Draining scheduler diagnostics %s.\n", *frame ? "required" : "not expected");
    return true;
}

/* Negative controls of workgraph_drain_info_error, on the CPU only: each reply must be refused. */
static void workgraph_check_drain_admission(void)
{
    const uint64_t min_size = 0x10000;
    struct workgraph_drain_info info;
    uint64_t wrap;

    memset(&info, 0, sizeof(info));
    info.rounds = 2;
    info.pending_rounds = 2;
    info.control_offset = 0x100;
    ok(!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, min_size), "A valid reply was refused.\n");
    ok(!!workgraph_drain_info_error(E_INVALIDARG, sizeof(info), &info, 2, 2, min_size),
            "A missing reply was accepted.\n");
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info) - 4, &info, 2, 2, min_size), "A short reply was accepted.\n");
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 3, 2, min_size), "Wrong rounds were accepted.\n");
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 1, min_size), "A wrong hook was accepted.\n");

    info.control_offset = 0x102;
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, min_size),
            "A control block that is not aligned was accepted.\n");
    info.control_offset = min_size - sizeof(struct workgraph_drain_control);
    ok(!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, min_size),
            "A control block at the end was refused.\n");
    info.control_offset += 4;
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, min_size),
            "A control block across the end was accepted.\n");
    info.control_offset = 0;
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, sizeof(struct workgraph_drain_control) - 4),
            "A control block in a smaller backing memory was accepted.\n");

    /* This offset passes the check offset + size <= min_size, because the sum wraps. */
    wrap = UINT64_MAX - 15;
    ok(wrap + sizeof(struct workgraph_drain_control) <= min_size, "The sum does not wrap.\n");
    info.control_offset = wrap;
    ok(!!workgraph_drain_info_error(S_OK, sizeof(info), &info, 2, 2, min_size),
            "A control block offset that wraps was accepted.\n");

    /* Without extra rounds the offset is not used. */
    info.rounds = info.pending_rounds = 0;
    ok(!workgraph_drain_info_error(S_OK, sizeof(info), &info, 0, 0, min_size), "A reply without rounds was refused.\n");
}

struct workgraph_drain_call
{
    const char *fault; /* AMDGPU_WDDM_WG_FAIL while the call is recorded, or NULL. */
    unsigned int predicate; /* As in struct workgraph_predicate_step. */
    D3D12_PREDICATION_OP op;
    bool same_backing; /* The call uses the backing memory of the call before, without the INITIALIZE flag. */
};

#define WG_DRAIN_MAX_CALLS 3

struct workgraph_drain_case
{
    const char *limits;
    unsigned int rounds, pending_rounds;
    bool direct; /* A copy comes before the first SetPredication, so that it is not deferred. */
    struct workgraph_drain_call calls[WG_DRAIN_MAX_CALLS];
    unsigned int call_count;
    bool bare; /* No application barrier and no ordinary Dispatch between the calls. */
};

/* Records the calls of one case in one command list, each DispatchGraph of level_sinks followed by an ordinary
 * Dispatch of drain_dispatch with the same predicate (not for a bare case), and checks the records of each call, the
 * Dispatch, 64 KiB after each backing memory and, if frame is true, the control block and the reports. If frame is
 * false, the state object must not give the round frame (a native driver or the release flavour). If pools is false,
 * the extra rounds run on the payload rings (pools=0) and the backing memory is MinSizeInBytes. If pools is true, the
 * engine runs them on the record pools, and the backing memory is WG_DRAIN_POOL_EXTRA bytes larger, so that round 1
 * holds all records of a call and the records of each call stay the same. */
#define WG_DRAIN_POOL_EXTRA (4u * 1024 * 1024)

static void workgraph_run_drain_case(struct test_context_workgraph *context, ID3D12PipelineState *cs_pso,
        ID3D12Resource *predicates, unsigned int index, const struct workgraph_drain_case *test, bool frame,
        bool pools)
{
    static const D3D12_NODE_ID entry_id = { u"Entry", 0 };
    ID3D12Resource *backing[WG_DRAIN_MAX_CALLS], *output[WG_DRAIN_MAX_CALLS], *copy_src, *copy_dst;
    unsigned int i, damaged, stall, hook_calls, events, reasons, reason;
    struct workgraph_drain_control control;
    struct workgraph_pools *pool_info;
    uint64_t backing_size;
    char limits[128];
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    ID3D12GraphicsCommandList10 *list10;
    D3D12_SET_PROGRAM_DESC program_desc;
    struct workgraph_drain_info info;
    ID3D12GraphicsCommandList *list;
    D3D12_PROGRAM_IDENTIFIER ident;
    bool run[WG_DRAIN_MAX_CALLS];
    uint32_t values[16 + 512];
    uint32_t *tail_pattern;
    ID3D12StateObject *pso;
    uint32_t params = 0;
    UINT entry_index;
    const char *error;
    UINT size;
    HRESULT hr;

    list = context->context.list;
    snprintf(limits, sizeof(limits), "%s%s", test->limits, pools ? "" : ",pools=0");
    workgraph_set_limits(limits);
    hr = try_create_workgraph_pso(context, level_sinks_dxil, NULL, &pso);
    workgraph_set_limits(NULL);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
        return;
    if (!workgraph_get_program(pso, &ident, &wg_reqs, &entry_index, &entry_id, 1))
    {
        ID3D12StateObject_Release(pso);
        return;
    }

    /* The reply is checked before any GPU work: a missing or bad round frame fails the case. */
    memset(&info, 0, sizeof(info));
    size = sizeof(info);
    hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_drain_guid, &size, &info);
    if (frame)
    {
        error = workgraph_drain_info_error(hr, size, &info, test->rounds, test->pending_rounds,
                wg_reqs.MinSizeInBytes);
        ok(!error, "Round frame: %s (hr #%x, size %u, rounds %u, pending %u, control block %#"PRIx64").\n",
                error, (int)hr, size, info.rounds, info.pending_rounds, info.control_offset);
        if (error)
        {
            ID3D12StateObject_Release(pso);
            return;
        }
    }
    else
    {
        ok(FAILED(hr), "Got a round frame, but none was expected.\n");
    }

    /* The extra rounds run on the record pools only if they are asked for. */
    if (frame)
    {
        pool_info = calloc(1, sizeof(*pool_info));
        size = sizeof(*pool_info);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_pool_guid, &size, pool_info);
        ok(hr == S_OK && size == sizeof(*pool_info), "Record pools: hr #%x, size %u.\n", (int)hr, size);
        ok(pool_info->execution == (pools && test->rounds), "Expected pool execution %u, got %u.\n",
                pools && test->rounds, pool_info->execution);
        free(pool_info);
    }

    backing_size = wg_reqs.MinSizeInBytes + (pools ? WG_DRAIN_POOL_EXTRA : 0);
    /* 64 KiB more than the backing memory that the graph gets, with a pattern that nothing may write. */
    tail_pattern = malloc(64 * 1024);
    for (i = 0; i < 64 * 1024 / sizeof(uint32_t); i++)
        tail_pattern[i] = 0x7a11c0deu;
    for (i = 0; i < test->call_count; i++)
    {
        backing[i] = NULL;
        if (!test->calls[i].same_backing || !i)
        {
            backing[i] = create_default_buffer(context->context.device, backing_size + 64 * 1024,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            upload_buffer_data(backing[i], backing_size, 64 * 1024, tail_pattern, context->context.queue,
                    list);
            reset_command_list(list, context->context.allocator);
        }
        output[i] = create_default_buffer(context->context.device, sizeof(values),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    free(tail_pattern);

    copy_src = create_default_buffer(context->context.device, 256, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_dst = create_default_buffer(context->context.device, 256, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    hr = ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&list10);
    ok(SUCCEEDED(hr), "Failed to query ID3D12GraphicsCommandList10, hr #%x.\n", (int)hr);

    memset(&dispatch_desc, 0, sizeof(dispatch_desc));
    dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index;
    dispatch_desc.NodeCPUInput.NumRecords = 1;
    dispatch_desc.NodeCPUInput.pRecords = &params;
    dispatch_desc.NodeCPUInput.RecordStrideInBytes = sizeof(params);

    if (test->direct)
        ID3D12GraphicsCommandList_CopyBufferRegion(list, copy_dst, 0, copy_src, 0, 256);
    for (i = 0; i < test->call_count; i++)
    {
        const struct workgraph_drain_call *call = &test->calls[i];

        if (!backing[i])
            backing[i] = backing[i - 1];
        run[i] = call->predicate == WG_PREDICATE_NONE ||
                (call->predicate == 8) == (call->op == D3D12_PREDICATION_OP_EQUAL_ZERO);

        if (call->predicate == WG_PREDICATE_NONE)
            ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        else
            ID3D12GraphicsCommandList_SetPredication(list, predicates, call->predicate, call->op);

        ID3D12GraphicsCommandList_SetComputeRootSignature(list, context->default_root_uav_rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0,
                ID3D12Resource_GetGPUVirtualAddress(output[i]));
        memset(&program_desc, 0, sizeof(program_desc));
        program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        program_desc.WorkGraph.BackingMemory.SizeInBytes = backing_size;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(backing[i]);
        program_desc.WorkGraph.Flags = call->same_backing ? D3D12_SET_WORK_GRAPH_FLAG_NONE :
                D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        program_desc.WorkGraph.ProgramIdentifier = ident;
        ID3D12GraphicsCommandList10_SetProgram(list10, &program_desc);

        workgraph_set_fault(call->fault);
        ID3D12GraphicsCommandList10_DispatchGraph(list10, &dispatch_desc);
        workgraph_set_fault(NULL);
        /* A bare case leaves the order of calls on the same backing memory to the work graph implementation. */
        if (test->bare)
            continue;
        uav_barrier(list, NULL);

        ID3D12GraphicsCommandList_SetPipelineState(list, cs_pso);
        ID3D12GraphicsCommandList_SetComputeRootSignature(list, context->default_root_uav_rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0,
                ID3D12Resource_GetGPUVirtualAddress(output[i]));
        ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
        uav_barrier(list, NULL);
    }
    ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    ID3D12GraphicsCommandList10_Release(list10);

    hook_calls = events = reasons = 0;
    for (i = 0; i < test->call_count; i++)
    {
        const struct workgraph_drain_call *call = &test->calls[i];

        /* The first read back executes the list. */
        workgraph_read_output(context, output[i], values, ARRAY_SIZE(values));
        ok(values[0] == (run[i] ? 4096 : 0), "Call %u: expected %u records, got %u.\n", i, run[i] ? 4096 : 0,
                values[0]);
        ok(values[2] == 0, "Call %u: %u records have wrong contents.\n", i, values[2]);
        ok(values[3] == 0, "Call %u: %u records arrived more than once.\n", i, values[3]);
        ok(values[8] == (run[i] && !test->bare), "Call %u: expected %u launches of the Dispatch after it, got %u.\n",
                i, run[i] && !test->bare, values[8]);

        stall = WG_DRAIN_NO_STALL;
        if (call->fault && !strncmp(call->fault, "drain-stall:", 12))
            stall = atoi(call->fault + 12);
        /* An engine block on the predicate of SetPredication in round 1, and one for each extra round. */
        if (call->fault && !strcmp(call->fault, "drain-hook"))
            hook_calls += (call->predicate != WG_PREDICATE_NONE) + test->rounds;

        /* The control block of a backing memory shows its last call. */
        if (frame && test->rounds && (i + 1 == test->call_count || !test->calls[i + 1].same_backing))
        {
            vkd3d_test_set_context("Test %u, %s, call %u", index, limits, i);
            workgraph_read_buffer_range(context, backing[i], info.control_offset, sizeof(control),
                    (uint32_t *)&control);
            reason = workgraph_check_drain_control(&control, test->rounds, test->pending_rounds, stall, run[i]);
        }
        else
        {
            reason = WG_DRAIN_REASON_COMPLETE;
            if (run[i] && stall != WG_DRAIN_NO_STALL)
                reason = WG_DRAIN_REASON_FAULT;
            else if (run[i] && test->rounds < test->pending_rounds)
                reason = WG_DRAIN_REASON_EXHAUSTED;
        }
        if (reason != WG_DRAIN_REASON_COMPLETE)
        {
            events++;
            reasons |= 1u << reason;
        }
    }

    for (i = 0; i < test->call_count; i++)
    {
        if (i && backing[i] == backing[i - 1])
            continue;
        damaged = workgraph_count_damaged_words(context, backing[i], backing_size, 64 * 1024, 0x7a11c0deu);
        ok(!damaged, "%u words after backing memory %u were written.\n", damaged, i);
    }

    if (frame && test->rounds)
    {
        memset(&info, 0, sizeof(info));
        size = sizeof(info);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_drain_guid, &size, &info);
        ok(hr == S_OK && size == sizeof(info), "Failed to get the round frame, hr #%x, size %u.\n", (int)hr, size);
    }
    if (frame && test->rounds && hr == S_OK && size == sizeof(info))
    {
        ok(info.abandoned_events == events, "Expected %u abandonment events, got %u.\n", events,
                info.abandoned_events);
        ok(info.abandoned_reasons == reasons, "Expected reasons %#x, got %#x.\n", reasons, info.abandoned_reasons);
        ok(info.abandoned_flags == (events ? WG_DRAIN_ABANDONED : 0), "Got flags %#x.\n", info.abandoned_flags);
        ok(info.hook_calls == hook_calls, "Expected %u hook calls, got %u.\n", hook_calls, info.hook_calls);
        ok(!info.violations, "%u engine blocks found the predicate of SetPredication active.\n", info.violations);
    }

    for (i = 0; i < test->call_count; i++)
    {
        if (!i || backing[i] != backing[i - 1])
            ID3D12Resource_Release(backing[i]);
        ID3D12Resource_Release(output[i]);
    }
    ID3D12Resource_Release(copy_dst);
    ID3D12Resource_Release(copy_src);
    ID3D12StateObject_Release(pso);
}

void test_workgraph_drain_rounds(void)
{
    /* The lab flavour of the amdgpu-wddm engine records "rounds" extra rounds after round 1 of a DispatchGraph, each
     * in a conditional rendering block on the pending word of the round before (AMDGPU_WDDM_WG_LIMITS=rounds=<n>).
     * The hook pending=<k> makes rounds 1 to k report waiting work also when no record waits, and an extra round then
     * runs the levels after the entry level again with nothing to do. Each case runs on the rings (pools=0, at Min)
     * and on the record pools (at Min + 4 MiB). The records of each call must not change
     * (test_workgraph_drain_retention keeps records across rounds). AMDGPU_WDDM_WG_FAIL=drain-stall:<r> makes round r
     * a round without progress (the self-check sets the fault word and later rounds do not run), and drain-hook tries
     * to begin the predicate of SetPredication inside each engine block, which the engine must suppress. Each DispatchGraph is followed by an ordinary Dispatch that
     * must be predicated as the DispatchGraph. Other drivers ignore the switches and must give the same records. Each
     * call launches 4096 Leaf records; the largest case runs 65 rounds of 2 levels. */
#define CALL(fault, predicate, op, same) { fault, predicate, op, same }
#define NONE WG_PREDICATE_NONE
#define EQ D3D12_PREDICATION_OP_EQUAL_ZERO
#define NE D3D12_PREDICATION_OP_NOT_EQUAL_ZERO
    static const struct workgraph_drain_case tests[] =
    {
        /* Rounds and the pending hook: k < R, k = R, k = R + 1 (R exhausted), k = 0, R = 64. */
        { "canary=1", 0, 0, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=1,canary=1", 1, 0, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=1,pending=1,canary=1", 1, 1, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=1,pending=2,canary=1", 1, 2, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=4,pending=2,canary=1", 4, 2, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=4,pending=4,canary=1", 4, 4, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=4,pending=5,canary=1", 4, 5, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=64,pending=64,canary=1", 64, 64, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        { "rounds=64,pending=65,canary=1", 64, 65, false, { CALL(NULL, NONE, EQ, false) }, 1 },
        /* A round without progress in round 1, a middle round and the last round. */
        { "rounds=4,pending=5,canary=1", 4, 5, false, { CALL("drain-stall:0", NONE, EQ, false) }, 1 },
        { "rounds=4,pending=5,canary=1", 4, 5, false, { CALL("drain-stall:2", NONE, EQ, false) }, 1 },
        { "rounds=4,pending=5,canary=1", 4, 5, false, { CALL("drain-stall:4", NONE, EQ, false) }, 1 },
        /* After a fault and after R exhausted, the next call on the same backing memory starts from init. */
        { "rounds=2,pending=2,canary=1", 2, 2, false,
                { CALL("drain-stall:1", NONE, EQ, false), CALL(NULL, NONE, EQ, true) }, 2 },
        { "rounds=1,pending=2,canary=1", 1, 2, false,
                { CALL(NULL, NONE, EQ, false), CALL(NULL, NONE, EQ, true) }, 2 },
        /* SetPredication false, true, false with both operations, deferred and direct; the skipped calls do not
         * run round 1 and so no extra round. */
        { "rounds=2,pending=2,canary=1", 2, 2, false,
                { CALL(NULL, 0, EQ, false), CALL(NULL, 8, EQ, false), CALL(NULL, 0, EQ, false) }, 3 },
        { "rounds=2,pending=2,canary=1", 2, 2, true,
                { CALL(NULL, 0, EQ, false), CALL(NULL, 8, EQ, false), CALL(NULL, 0, EQ, false) }, 3 },
        { "rounds=2,pending=2,canary=1", 2, 2, false,
                { CALL(NULL, 8, NE, false), CALL(NULL, 0, NE, false), CALL(NULL, 8, NE, false) }, 3 },
        { "rounds=2,pending=2,canary=1", 2, 2, true,
                { CALL(NULL, 8, NE, false), CALL(NULL, 0, NE, false), CALL(NULL, 8, NE, false) }, 3 },
        /* The hook inside the engine blocks, with a true and a false predicate and without one. */
        { "rounds=2,pending=2,canary=1", 2, 2, false,
                { CALL("drain-hook", 8, EQ, false), CALL("drain-hook", 0, EQ, false),
                CALL("drain-hook", NONE, EQ, false) }, 3 },
        { "rounds=2,pending=2,canary=1", 2, 2, true,
                { CALL("drain-hook", 0, NE, false), CALL("drain-hook", 8, NE, false) }, 2 },
        /* Executed, skipped and executed on one backing memory, with no application barrier between the calls:
         * the third call must start from the state that the close of the first call left (B5). The first call
         * must run, because a skipped call does not initialize the backing memory on every driver. */
        { "rounds=2,pending=2,canary=1", 2, 2, false,
                { CALL(NULL, 8, EQ, false), CALL(NULL, 0, EQ, true), CALL(NULL, 8, EQ, true) }, 3, true },
        { "rounds=1,pending=2,canary=1", 1, 2, true,
                { CALL(NULL, 0, NE, false), CALL(NULL, 8, NE, true), CALL(NULL, 0, NE, true) }, 3, true },
        { "rounds=4,pending=5,canary=1", 4, 5, false,
                { CALL("drain-stall:2", 8, EQ, false), CALL(NULL, 0, EQ, true), CALL(NULL, 8, EQ, true) }, 3, true },
    };
#undef NE
#undef EQ
#undef NONE
#undef CALL
    static const uint64_t predicate_values[2] = {0, 1};
    struct test_context_workgraph context;
    ID3D12PipelineState *cs_pso;
    ID3D12Resource *predicates;
    unsigned int i, pools;
    bool frame;
    HRESULT hr;

    workgraph_check_drain_admission();

    if (!init_workgraph_test_context(&context))
        return;
    if (!workgraph_drain_expected(context.device, &frame))
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    cs_pso = create_compute_pipeline_state(context.context.device, context.default_root_uav_rs,
            drain_dispatch_dxil);
    predicates = create_upload_buffer(context.context.device, sizeof(predicate_values), predicate_values);

    /* Each case runs on the payload rings, then on the record pools. */
    for (pools = 0; pools < 2; pools++)
    {
        for (i = 0; i < ARRAY_SIZE(tests); i++)
        {
            char name[16];

            /* VKD3D_TEST_WORKGRAPH_CASE=case<i>. runs only case i. */
            sprintf(name, "case%u.", i);
            if (!workgraph_case_enabled(name))
                continue;
            vkd3d_test_set_context("Test %u, %s, pools %u", i, tests[i].limits, pools);
            workgraph_run_drain_case(&context, cs_pso, predicates, i, &tests[i], frame, pools);
        }
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    ID3D12Resource_Release(predicates);
    ID3D12PipelineState_Release(cs_pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_dispatch_failure(void)
{
    /* The lab flavour of the amdgpu-wddm engine reads AMDGPU_WDDM_WG_FAIL at each scratch memory allocation of a
     * DispatchGraph call. "scratch:<n>" fails allocation n of the call (counted from 0), as if the memory was
     * exhausted. The command list must then fail Close with E_OUTOFMEMORY (it is not submitted), without a crash.
     * When n is past the last allocation of the call, Close succeeds and the graph gives the correct result.
     * This runs for each dispatch mode. Set VKD3D_TEST_WORKGRAPH_FAULT_INJECTION=1 to run this test. */
    static const D3D12_NODE_ID entry_ids[] = { { u"EntryA", 0 }, { u"EntryB", 0 } };
    static const char *mode_names[] = { "CPU input", "multi CPU input", "GPU input", "multi GPU input" };
    struct workgraph_count_input inputs[3] = { { 0, 0, 3000 }, { 1, 3000, 7 }, { 0, 3100, 2000 } };
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct workgraph_multi_input multi;
    D3D12_NODE_CPU_INPUT cpu_inputs[3];
    D3D12_NODE_GPU_INPUT node_inputs[3];
    uint32_t expected[8], values[8];
    unsigned int failures, n, i, j;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *gpu_input;
    ID3D12Resource *scratch;
    ID3D12Resource *output;
    ID3D12Resource *records;
    unsigned int mode, used;
    UINT entry_index[2];
    ID3D12StateObject *pso;
    uint8_t *cpu_records;
    char fault[32];
    const char *env;
    HRESULT hr;

    env = getenv("VKD3D_TEST_WORKGRAPH_FAULT_INJECTION");
    if (!env || env[0] != '1')
    {
        skip("Set VKD3D_TEST_WORKGRAPH_FAULT_INJECTION=1 to run the fault injection test.\n");
        return;
    }

    if (!init_workgraph_test_context(&context))
        return;

    if (!workgraph_is_vkd3d_proton(context.device))
    {
        skip("Fault injection needs the amdgpu-wddm engine.\n");
        destroy_workgraph_test_context(&context);
        return;
    }

    hr = try_create_workgraph_pso(&context, multi_input_count_dxil, NULL, &pso);
    ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
    if (!pso)
    {
        destroy_workgraph_test_context(&context);
        return;
    }
    if (!workgraph_get_program(pso, &ident, &wg_reqs, entry_index, entry_ids, ARRAY_SIZE(entry_ids)))
        goto out_pso;

    records = workgraph_create_count_records(&context, 6000, WG_COUNT_RECORD_SIZE, 1);
    cpu_records = calloc(6000, WG_COUNT_RECORD_SIZE);
    for (i = 0; i < 6000; i++)
    {
        uint32_t value = (i % 7) + 1, tail = 1;

        memcpy(cpu_records + (size_t)i * WG_COUNT_RECORD_SIZE, &value, sizeof(value));
        memcpy(cpu_records + (size_t)i * WG_COUNT_RECORD_SIZE + WG_COUNT_RECORD_SIZE - 4, &tail, sizeof(tail));
    }
    for (i = 0; i < ARRAY_SIZE(inputs); i++)
    {
        node_inputs[i].EntrypointIndex = entry_index[inputs[i].entry];
        node_inputs[i].NumRecords = inputs[i].count;
        node_inputs[i].Records.StartAddress = ID3D12Resource_GetGPUVirtualAddress(records) +
                (UINT64)inputs[i].first * WG_COUNT_RECORD_SIZE;
        node_inputs[i].Records.StrideInBytes = WG_COUNT_RECORD_SIZE;
        cpu_inputs[i].EntrypointIndex = entry_index[inputs[i].entry];
        cpu_inputs[i].NumRecords = inputs[i].count;
        cpu_inputs[i].pRecords = cpu_records + (size_t)inputs[i].first * WG_COUNT_RECORD_SIZE;
        cpu_inputs[i].RecordStrideInBytes = WG_COUNT_RECORD_SIZE;
    }
    workgraph_create_multi_input(&context, node_inputs, ARRAY_SIZE(node_inputs), ARRAY_SIZE(node_inputs), 32, &multi);
    gpu_input = create_upload_buffer(context.context.device, sizeof(node_inputs[0]), &node_inputs[0]);
    scratch = workgraph_create_backing_memory(&context, &wg_reqs);

    for (mode = 0; mode < ARRAY_SIZE(mode_names); mode++)
    {
        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        switch (mode)
        {
            case 0:
                dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
                dispatch_desc.NodeCPUInput = cpu_inputs[0];
                used = 1;
                break;
            case 1:
                dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
                dispatch_desc.MultiNodeCPUInput.NumNodeInputs = ARRAY_SIZE(cpu_inputs);
                dispatch_desc.MultiNodeCPUInput.pNodeInputs = cpu_inputs;
                dispatch_desc.MultiNodeCPUInput.NodeInputStrideInBytes = sizeof(cpu_inputs[0]);
                used = ARRAY_SIZE(cpu_inputs);
                break;
            case 2:
                dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
                dispatch_desc.NodeGPUInput = ID3D12Resource_GetGPUVirtualAddress(gpu_input);
                used = 1;
                break;
            default:
                dispatch_desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
                dispatch_desc.MultiNodeGPUInput = multi.va;
                used = ARRAY_SIZE(node_inputs);
                break;
        }

        memset(expected, 0, sizeof(expected));
        for (i = 0; i < used; i++)
            workgraph_count_expected(&inputs[i], 1, 1, expected);

        failures = 0;
        for (n = 0; n < 256; n++)
        {
            vkd3d_test_set_context("%s, scratch:%u", mode_names[mode], n);
            output = create_default_buffer(context.context.device, 4 * 1024,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

            snprintf(fault, sizeof(fault), "scratch:%u", n);
            workgraph_set_fault(fault);
            workgraph_record_dispatch(&context, context.context.list, &ident, scratch, wg_reqs.MinSizeInBytes,
                    output, &dispatch_desc);
            workgraph_set_fault(NULL);

            transition_resource_state(context.context.list, output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            hr = ID3D12GraphicsCommandList_Close(context.context.list);
            if (hr == E_OUTOFMEMORY)
            {
                failures++;
                ID3D12Resource_Release(output);
                reset_command_list(context.context.list, context.context.allocator);
                continue;
            }

            ok(hr == S_OK, "Expected S_OK or E_OUTOFMEMORY, got hr #%x.\n", (int)hr);
            /* The list is closed, so read back with a second recording. */
            exec_command_list(context.context.queue, context.context.list);
            wait_queue_idle(context.context.device, context.context.queue);
            reset_command_list(context.context.list, context.context.allocator);
            {
                struct resource_readback rb;

                get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb,
                        context.context.queue, context.context.list);
                reset_command_list(context.context.list, context.context.allocator);
                for (j = 0; j < ARRAY_SIZE(values); j++)
                    values[j] = get_readback_uint(&rb, j, 0, 0);
                release_resource_readback(&rb);
            }
            for (j = 0; j < ARRAY_SIZE(values); j++)
                ok(values[j] == expected[j], "Value %u: expected %u, got %u.\n", j, expected[j], values[j]);
            ID3D12Resource_Release(output);
            break;
        }
        vkd3d_test_set_context("%s", mode_names[mode]);
        ok(failures > 0, "No allocation failed.\n");
        ok(n < 256, "The allocations did not end.\n");

        hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
        ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    }
    vkd3d_test_set_context(NULL);

    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(gpu_input);
    ID3D12Resource_Release(multi.buffer);
    ID3D12Resource_Release(records);
    free(cpu_records);
out_pso:
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_broadcast_input(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12StateObject *pso;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, broadcast_custom_input_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL, 8, &ident, &wg_reqs);

    {
        const uint32_t records[] = { 19, 800, 400, 90 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = broadcast_input_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 8;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("Plain broadcast");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);

    /* Proves that the input struct to a broadcast node is interpreted as-is, which will complicate any attempt to
     * do GPU input nodes. 1D and 2D dispatch is supported, which we'd have to massage to indirect 3D dispatch somehow.
     * Raw 16-bit packing can be used too, which complicates things even more ... */

    pso = create_workgraph_pso(&context, broadcast_custom_input_uint2_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL, 16, &ident, &wg_reqs);

    {
        const uint32_t records[] = { 19, 3, 2, 800, 400, 2, 1, 90 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = broadcast_input_uint2_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 16;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("uint2 broadcast");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);

    pso = create_workgraph_pso(&context, broadcast_custom_input_uint_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL, 12, &ident, &wg_reqs);

    {
        const uint32_t records[] = { 19, 3, 800, 400, 2, 90 };
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = broadcast_input_uint_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 12;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("uint broadcast");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);

    pso = create_workgraph_pso(&context, broadcast_custom_input_u16_2_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"BroadcastNode", NULL, 12, &ident, &wg_reqs);

    {
        const uint32_t records[] = { 19, 3 | (2 << 16), 800, 400, 2 | (1 << 16), 90};
        struct workgraph_test_desc desc;
        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = broadcast_input_uint16x2_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 12;
        desc.records = records;
        desc.num_threads[0] = 2;
        desc.num_threads[1] = 3;
        desc.num_threads[2] = 4;

        vkd3d_test_set_context("uint16_t broadcast");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);

    destroy_workgraph_test_context(&context);
    vkd3d_test_set_context(NULL);
}

void test_workgraph_thread_input(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12StateObject *pso;
    unsigned int i;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, thread_custom_input_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"ThreadNode", NULL, 8, &ident, &wg_reqs);

    {
        struct workgraph_test_desc desc;
        uint32_t records[2 * 256];
        memset(&desc, 0, sizeof(desc));

        for (i = 0; i < ARRAY_SIZE(records) / 2; i++)
        {
            records[2 * i + 0] = (i ^ 7) * 3;
            records[2 * i + 1] = i ^ 0xabcd;
        }

        desc.expected_cb = thread_input_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = ARRAY_SIZE(records) / 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 2 * sizeof(records[0]);
        desc.records = records;

        vkd3d_test_set_context("CPU");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        vkd3d_test_set_context("GPU");
        desc.mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        /* amdgpu-wddm fork: the multi GPU input path copies the records of each input. */
        vkd3d_test_set_context("Multi GPU");
        desc.mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
    vkd3d_test_set_context(NULL);
}

void test_workgraph_coalesced_input(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12StateObject *pso;

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, coalesce_custom_input_dxil, u"Dummy", context.default_root_uav_rs);
    check_work_graph_properties(pso, u"Dummy", u"CoalesceNode", NULL, 8, &ident, &wg_reqs);

    {
        static const uint32_t records[] =
        {
            100, 50,
            150, 40,
            200, 79,
            300, 40,
            400, 30,
            490, 20,
            530, 10,
            32, 32,
        };
        struct workgraph_test_desc desc;

        memset(&desc, 0, sizeof(desc));

        desc.expected_cb = coalesced_input_expected;
        desc.mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.num_graph_dispatches = 1;
        desc.num_multi_instances = 1;
        desc.num_records = ARRAY_SIZE(records) / 2;
        desc.record_size = sizeof(records);
        desc.record_stride = 2 * sizeof(records[0]);
        desc.records = records;
        desc.num_threads[0] = 24;

        vkd3d_test_set_context("CPU");
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        vkd3d_test_set_context("GPU");
        desc.mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
        /* amdgpu-wddm fork: the multi GPU input path copies the records of each input. */
        vkd3d_test_set_context("Multi GPU");
        desc.mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        execute_workgraph_test(&context, pso, &ident, &wg_reqs, &desc);
    }

    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
    vkd3d_test_set_context(NULL);
}

static void test_workgraph_two_level_broadcast_inner(struct test_context_workgraph *context,
    ID3D12StateObject *pso)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12Resource *output[2];
    unsigned i, j, k;

    struct entry_data
    {
        uint32_t grid;
        uint32_t node_idx;
        uint32_t size;
        uint32_t offset;
        uint32_t increment;
    };

    static const struct entry_data node_data[] = {
        { 2, 0, 1, 1, 1 },
        { 2, 1, 1, 3, 2 },
        { 3, 0, 3, 5, 3 },
        { 1, 1, 3, 4, 4 },
    };

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, sizeof(struct entry_data), &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(
        context->context.list, context->default_root_uav_rs);

    for (i = 0; i < ARRAY_SIZE(output); i++)
    {
        output[i] = create_default_buffer(context->context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context->context.list, i,
            ID3D12Resource_GetGPUVirtualAddress(output[i]));
    }

    execute_workgraph_pso_simple(context, pso, &ident, &wg_reqs, node_data, sizeof(node_data[0]), ARRAY_SIZE(node_data), NULL, &scratch);

    for (i = 0; i < ARRAY_SIZE(output); i++)
    {
        uint32_t reference_output[1024];
        memset(reference_output, 0, sizeof(reference_output));

        for (j = 0; j < ARRAY_SIZE(node_data); j++)
        {
            if (node_data[j].node_idx != i)
                continue;
            for (k = 0; k < node_data[j].size * node_data[j].grid * 64; k++)
            {
                assert(k + node_data[j].offset < ARRAY_SIZE(reference_output));
                reference_output[k + node_data[j].offset] += node_data[j].increment;
            }
        }

        transition_resource_state(context->context.list, output[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(output[i], DXGI_FORMAT_R32_UINT, &rb, context->context.queue, context->context.list);

        for (j = 0; j < ARRAY_SIZE(reference_output); j++)
        {
            ok(reference_output[j] == get_readback_uint(&rb, j, 0, 0),
                "Buffer %u[%u] = %u, expected %u\n", i, j, get_readback_uint(&rb, j, 0, 0), reference_output[j]);
        }

        release_resource_readback(&rb);
        reset_command_list(context->context.list, context->context.allocator);
    }

    ID3D12Resource_Release(output[0]);
    ID3D12Resource_Release(output[1]);
    if (scratch)
        ID3D12Resource_Release(scratch);
}

void test_workgraph_two_level_broadcast(void)
{
    struct test_context_workgraph context;
    ID3D12StateObject *pso[4];
    unsigned int i;

#include "shaders/workgraph/headers/two_level_broadcast.h"
#include "shaders/workgraph/headers/two_level_broadcast_node_array.h"
#include "shaders/workgraph/headers/two_level_thread.h"
#include "shaders/workgraph/headers/two_level_thread_node_array.h"

    if (!init_workgraph_test_context(&context))
        return;

    pso[0] = create_workgraph_pso(&context, two_level_broadcast_dxil, u"Dummy", context.default_root_uav_rs);
    pso[1] = create_workgraph_pso(&context, two_level_broadcast_node_array_dxil, u"Dummy", context.default_root_uav_rs);
    pso[2] = create_workgraph_pso(&context, two_level_thread_dxil, u"Dummy", context.default_root_uav_rs);
    pso[3] = create_workgraph_pso(&context, two_level_thread_node_array_dxil, u"Dummy", context.default_root_uav_rs);

    vkd3d_test_set_context("Broadcast - Node");
    test_workgraph_two_level_broadcast_inner(&context, pso[0]);
    vkd3d_test_set_context("Broadcast - NodeArray");
    test_workgraph_two_level_broadcast_inner(&context, pso[1]);
    vkd3d_test_set_context("Thread - Node");
    test_workgraph_two_level_broadcast_inner(&context, pso[2]);
    vkd3d_test_set_context("Thread - NodeArray");
    test_workgraph_two_level_broadcast_inner(&context, pso[3]);
    vkd3d_test_set_context(NULL);

    for (i = 0; i < ARRAY_SIZE(pso); i++)
        ID3D12StateObject_Release(pso[i]);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_two_level_empty(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12StateObject *pso;
    ID3D12Resource *output;
    unsigned i;

#include "shaders/workgraph/headers/two_level_empty_node.h"

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, two_level_empty_node_dxil, u"Dummy", context.default_root_uav_rs);

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, 0, &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
    output = create_default_buffer(context.context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    execute_workgraph_pso_simple(&context, pso, &ident, &wg_reqs, NULL, 0, 1, NULL, &scratch);

    transition_resource_state(context.context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb, context.context.queue, context.context.list);

    {
        static const uint32_t reference[16] = { 5, 5, 4, 6, 4, 6, 3, 2, 3, 2, 9, 7, 9, 7 };

        for (i = 0; i < ARRAY_SIZE(reference); i++)
        {
            uint32_t value = get_readback_uint(&rb, i, 0, 0);
            ok(value == reference[i], "Value %u: expected %u, got %u.\n", i, reference[i], value);
        }
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_basic_recursion(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12StateObject *pso;
    ID3D12Resource *output;
    unsigned i;

#include "shaders/workgraph/headers/basic_recursion.h"

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, basic_recursion_dxil, u"Dummy", context.default_root_uav_rs);

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, 0, &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
    output = create_default_buffer(context.context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    execute_workgraph_pso_simple(&context, pso, &ident, &wg_reqs, NULL, 0, 1, NULL, &scratch);

    transition_resource_state(context.context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb, context.context.queue, context.context.list);

    {
        static const uint32_t reference[64] = { 0, 7, 13, 17, 25, 34, 46, 59, 71, 76, 68, 47, 23, 7, 1 };

        for (i = 0; i < ARRAY_SIZE(reference); i++)
        {
            uint32_t value = get_readback_uint(&rb, i, 0, 0);
            ok(value == reference[i], "Value %u: expected %u, got %u.\n", i, reference[i], value);
        }
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_cross_group_sharing(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12StateObject *pso;
    ID3D12Resource *output;
    unsigned i;

#include "shaders/workgraph/headers/cross_group_sharing.h"

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, cross_group_sharing_dxil, u"Dummy", context.default_root_uav_rs);

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, 0, &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
    output = create_default_buffer(context.context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    execute_workgraph_pso_simple(&context, pso, &ident, &wg_reqs, NULL, 0, 1, NULL, &scratch);

    transition_resource_state(context.context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb, context.context.queue, context.context.list);

    {
        static const uint32_t reference[4] = { 512 * 64 + 9, 256 * 64 + 11 };

        for (i = 0; i < ARRAY_SIZE(reference) * 64; i++)
        {
            uint32_t value = get_readback_uint(&rb, i, 0, 0);
            ok(value == reference[i / 64], "Value %u: expected %u, got %u.\n", i, reference[i / 64], value);
        }
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_shared_inputs(void)
{
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12StateObject *pso;
    ID3D12Resource *output;
    unsigned i;

#include "shaders/workgraph/headers/shared_inputs.h"

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, shared_inputs_dxil, u"Dummy", context.default_root_uav_rs);

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, 0, &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
    output = create_default_buffer(context.context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    execute_workgraph_pso_simple(&context, pso, &ident, &wg_reqs, NULL, 0, 1, NULL, &scratch);

    transition_resource_state(context.context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb, context.context.queue, context.context.list);

    {
        static const uint32_t reference[4] = { 512 * 9, 256 * 9 };

        for (i = 0; i < ARRAY_SIZE(reference) * 64; i++)
        {
            uint32_t value = get_readback_uint(&rb, i, 0, 0);
            ok(value == reference[i / 64], "Value %u: expected %u, got %u.\n", i, reference[i / 64], value);
        }
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

void test_workgraph_local_root_signature(void)
{
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE local_table;
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct test_context_workgraph context;
    D3D12_PROGRAM_IDENTIFIER ident;
    ID3D12Resource *scratch = NULL;
    struct resource_readback rb;
    ID3D12StateObject *pso;
    ID3D12Resource *output;
    ID3D12Resource *local;

#include "shaders/workgraph/headers/local_root_signature.h"

    struct
    {
        D3D12_GPU_VIRTUAL_ADDRESS va;
        uint32_t const0;
        uint32_t const1;
    } local_data[] = {
        { 0, 30, 20 },
        { 0, 40, 50 },
    };

    if (!init_workgraph_test_context(&context))
        return;

    pso = create_workgraph_pso(&context, local_root_signature_dxil, u"Dummy", context.default_root_uav_rs);

    check_work_graph_properties(pso, u"Dummy", u"EntryNode", NULL, 0, &ident, &wg_reqs);

    ID3D12GraphicsCommandList_SetComputeRootSignature(context.context.list, context.default_root_uav_rs);
    output = create_default_buffer(context.context.device, 4 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.context.list, 0, ID3D12Resource_GetGPUVirtualAddress(output));

    local_data[0].va = ID3D12Resource_GetGPUVirtualAddress(output);
    local_data[1].va = local_data[0].va;
    local = create_upload_buffer(context.context.device, sizeof(local_data), local_data);

    local_table.StartAddress = ID3D12Resource_GetGPUVirtualAddress(local);
    local_table.StrideInBytes = sizeof(local_data[0]);
    local_table.SizeInBytes = sizeof(local_data);
    execute_workgraph_pso_simple(&context, pso, &ident, &wg_reqs, NULL, 0, 1, &local_table, &scratch);

    transition_resource_state(context.context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output, DXGI_FORMAT_R32_UINT, &rb, context.context.queue, context.context.list);

    {
        uint32_t value0 = get_readback_uint(&rb, local_data[0].const0, 0, 0);
        uint32_t value1 = get_readback_uint(&rb, local_data[1].const1, 0, 0);
        ok(value0 == local_data[0].const1 * 30, "Expected %u, got %u\n", local_data[0].const1 * 30, value0);
        ok(value1 == local_data[1].const0 * 60, "Expected %u, got %u\n", local_data[1].const0 * 60, value1);
    }

    release_resource_readback(&rb);
    if (scratch)
        ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(local);
    ID3D12StateObject_Release(pso);
    destroy_workgraph_test_context(&context);
}

/* One output declaration of a producer, from the HLSL source of the graph. */
struct workgraph_pool_decl
{
    unsigned int pool; /* The level of the producer + 1. */
    unsigned int producer; /* The outputs of one producer add up. */
    bool thread; /* The producer is a thread launch node: one item is a wave of up to 64 threads. */
    uint32_t max_records; /* MaxRecords, or the shared budget for MaxRecordsSharedWith. */
    uint32_t record_size; /* The largest record of its targets, 0 for an empty node. */
    unsigned int shares; /* 1 + the index of the declaration whose budget it shares (MaxRecordsSharedWith), or 0. */
    uint32_t items; /* The work items of one input record of the producer: its grid if it broadcasts, else 1. */
    unsigned int input_of; /* 1 + the producer whose input record this producer shares (NodeShareInputOf), or 0. */
};

#define WG_POOL_MAX_DECLS 8

struct workgraph_pool_graph
{
    const char *name;
    const D3D12_SHADER_BYTECODE *code;
    unsigned int levels;
    struct workgraph_pool_decl decls[WG_POOL_MAX_DECLS];
    unsigned int decl_count;
};

static const unsigned int workgraph_pool_unit[WG_POOL_COMPONENTS] = { 1, 8, 4 };
static const unsigned int workgraph_pool_alignment[WG_POOL_COMPONENTS] = { 16, 8, 4 };

/* W and the record bound of each pool from the declarations. With exact true, a MaxRecordsSharedWith set is charged
 * once, with its budget and the largest record of the set (the spec bound). With exact false, the engine's rule: the
 * shader metadata does not say which declarations share a budget, so each declaration is charged on its own, and a
 * shared one with the largest MaxRecords of its producer. records and stride, if not 0, replace MaxRecords of each
 * declaration and the record size of each declaration with a payload (the pool_records and pool_stride hooks). An
 * empty record is not stored, but the node output allocator charges it 4 bytes of the payload capacity, and the
 * distributor gives it an unrolled offset. The bound of a pool is the largest demand of one input record of its
 * producers: W x items of the producer plus W x items of each producer that shares that input record, because a
 * record is admitted with all its groups and all its readers. */
static void workgraph_pool_oracle(const struct workgraph_pool_graph *graph, bool exact, uint32_t records_hook,
        uint32_t stride_hook, uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS],
        uint64_t bound[WG_POOL_LEVELS][WG_POOL_COMPONENTS])
{
    uint64_t demand[WG_POOL_MAX_DECLS][WG_POOL_COMPONENTS], item[WG_POOL_COMPONENTS], payload, records, stride;
    unsigned int producer, i, j, c, pool, pools[WG_POOL_MAX_DECLS], input_of[WG_POOL_MAX_DECLS];
    uint64_t group[WG_POOL_MAX_DECLS][WG_POOL_COMPONENTS], largest, items;
    unsigned int group_pool[WG_POOL_MAX_DECLS];
    const struct workgraph_pool_decl *decl, *other;
    bool found;

    memset(w, 0, sizeof(uint64_t) * WG_POOL_LEVELS * WG_POOL_COMPONENTS);
    memset(bound, 0, sizeof(uint64_t) * WG_POOL_LEVELS * WG_POOL_COMPONENTS);
    memset(demand, 0, sizeof(demand));
    memset(pools, 0, sizeof(pools));
    memset(input_of, 0, sizeof(input_of));
    for (producer = 0; producer < WG_POOL_MAX_DECLS; producer++)
    {
        memset(item, 0, sizeof(item));
        found = false;
        pool = 0;
        largest = 0;
        items = 1;
        for (i = 0; i < graph->decl_count; i++)
            if (graph->decls[i].producer == producer && !graph->decls[i].shares)
                largest = max(largest, graph->decls[i].max_records);

        for (i = 0; i < graph->decl_count; i++)
        {
            decl = &graph->decls[i];
            if (decl->producer != producer)
                continue;
            found = true;
            pool = decl->pool;
            items = decl->items;
            input_of[producer] = decl->input_of;
            if (exact && decl->shares)
                continue;
            records = decl->shares ? largest : decl->max_records;
            stride = (decl->record_size + 15) & ~15u;
            if (exact)
            {
                for (j = 0; j < graph->decl_count; j++)
                {
                    other = &graph->decls[j];
                    if (other->shares == i + 1)
                        stride = max(stride, (other->record_size + 15) & ~15u);
                }
            }
            if (records_hook)
                records = records_hook;
            if (stride_hook && stride)
                stride = ((uint64_t)stride_hook + 15) & ~(uint64_t)15;
            payload = records * (stride ? stride : 4);
            if (decl->thread)
            {
                payload *= 64;
                records *= 64;
            }
            item[WG_POOL_PAYLOAD] += payload;
            item[WG_POOL_OFFSETS] += records;
            if (stride)
                item[WG_POOL_ENTRIES] += records;
        }
        if (!found)
            continue;
        pools[producer] = pool;
        for (c = 0; c < WG_POOL_COMPONENTS; c++)
        {
            w[pool][c] = max(w[pool][c], item[c]);
            demand[producer][c] = item[c] * items;
        }
    }

    /* The readers of one input record: the producer that owns it and the producers that share it. A reader without
     * outputs has no declaration and adds nothing. */
    memset(group, 0, sizeof(group));
    memset(group_pool, 0, sizeof(group_pool));
    for (producer = 0; producer < WG_POOL_MAX_DECLS; producer++)
    {
        if (!pools[producer])
            continue;
        j = input_of[producer] ? input_of[producer] - 1 : producer;
        group_pool[j] = pools[producer];
        for (c = 0; c < WG_POOL_COMPONENTS; c++)
            group[j][c] += demand[producer][c];
    }
    for (j = 0; j < WG_POOL_MAX_DECLS; j++)
        for (c = 0; group_pool[j] && c < WG_POOL_COMPONENTS; c++)
            bound[group_pool[j]][c] = max(bound[group_pool[j]][c], group[j][c]);
}

/* Checks one region: aligned, at or after *end, inside used bytes. */
static bool workgraph_check_pool_region(const struct workgraph_pool_region *region, uint64_t alignment, uint64_t used,
        uint64_t *end, const char *what)
{
    bool valid = !(region->offset % alignment) && region->offset >= *end && region->offset <= used &&
            region->size <= used - region->offset;

    ok(valid, "%s at %#"PRIx64", %#"PRIx64" bytes: not aligned, overlapping or outside %#"PRIx64" bytes.\n", what,
            region->offset, region->size, used);
    if (valid)
        *end = region->offset + region->size;
    return valid;
}

#define WG_POOL_REGIONS (2 + 2 * (WG_POOL_LEVELS - 1))

/* The regions that get a part of the arena, with the isolated size of each from W: the packed entries and the
 * unrolled offsets (the largest W of the pools), then the payload of each pool half (W rounded up to 16 bytes). */
static unsigned int workgraph_pool_regions(const struct workgraph_pool_graph *graph,
        const struct workgraph_pool_layout *layout, const uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS],
        const struct workgraph_pool_region **regions, uint64_t *isolated)
{
    unsigned int level, half, count = 2;

    regions[0] = &layout->entries;
    regions[1] = &layout->offsets;
    isolated[0] = isolated[1] = 0;
    for (level = 1; level < graph->levels; level++)
    {
        isolated[0] = max(isolated[0], w[level][WG_POOL_ENTRIES] * workgraph_pool_unit[WG_POOL_ENTRIES]);
        isolated[1] = max(isolated[1], w[level][WG_POOL_OFFSETS] * workgraph_pool_unit[WG_POOL_OFFSETS]);
        for (half = 0; half < 2; half++)
        {
            regions[count] = &layout->payload[level][half];
            isolated[count++] = (w[level][WG_POOL_PAYLOAD] + 15) & ~(uint64_t)15;
        }
    }
    /* The unrolled offsets hold at least one record for each 4 bytes of a payload half of pool 1: the entry level
     * is not throttled. */
    if (graph->levels > 1)
        isolated[1] = max(isolated[1], (w[1][WG_POOL_PAYLOAD] + 15) & ~(uint64_t)15);
    return count;
}

/* Min from W, independently of the layouts of the engine: the fixed bookkeeping at 0, the packed entries and the
 * unrolled offsets with the largest W of the pools (the offsets also at least a payload half of pool 1), the
 * empty-record state (no size yet), the payload of each pool half with W rounded up to 16 bytes and its sink, then the
 * staging range at a multiple of 16 bytes, all rounded up to the
 * 64 KiB granularity of the backing memory. */
static uint64_t workgraph_pool_min(const struct workgraph_pool_graph *graph, const struct workgraph_pools *pools,
        const uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS])
{
    uint64_t cursor, entries = 0, offsets = 0;
    unsigned int level, half;

    for (level = 1; level < graph->levels; level++)
    {
        entries = max(entries, w[level][WG_POOL_ENTRIES] * 8);
        offsets = max(offsets, w[level][WG_POOL_OFFSETS] * 4);
    }
    if (graph->levels > 1)
        offsets = max(offsets, (w[1][WG_POOL_PAYLOAD] + 15) & ~(uint64_t)15);
    cursor = pools->fixed_size;
    cursor = ((cursor + 7) & ~(uint64_t)7) + entries;
    cursor = ((cursor + 3) & ~(uint64_t)3) + offsets;
    cursor = (cursor + 15) & ~(uint64_t)15;
    for (level = 1; level < graph->levels; level++)
    {
        for (half = 0; half < 2; half++)
        {
            cursor = ((cursor + 15) & ~(uint64_t)15) + ((w[level][WG_POOL_PAYLOAD] + 15) & ~(uint64_t)15);
            cursor = ((cursor + 15) & ~(uint64_t)15) + pools->sink_size;
        }
    }
    cursor = ((cursor + 15) & ~(uint64_t)15) + pools->staging_size;
    return (cursor + 65535) & ~(uint64_t)65535;
}

/* Checks one layout and returns the bytes that its regions have above their isolated sizes (the arena). */
static uint64_t workgraph_check_pool_layout(const struct workgraph_pool_graph *graph, const struct workgraph_pools *pools,
        const struct workgraph_pool_layout *layout, const uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS],
        const uint64_t exact[WG_POOL_LEVELS][WG_POOL_COMPONENTS])
{
    const struct workgraph_pool_region *regions[WG_POOL_REGIONS], *last;
    uint64_t end, isolated[WG_POOL_REGIONS], arena = 0, unused;
    const uint64_t used = layout->used;
    unsigned int level, half, count, a, b;
    uint64_t total;
    char what[64];

    end = 0;
    workgraph_check_pool_region(&layout->fixed, 64, used, &end, "Fixed bookkeeping");
    ok(layout->fixed.size == pools->fixed_size, "Got fixed size %#"PRIx64".\n", layout->fixed.size);
    /* The producers find the output guard, the last 32 bytes of the fixed bookkeeping, before the packed entries. */
    workgraph_check_pool_region(&layout->entries, 8, used, &end, "Packed entries");
    ok(layout->fixed.size >= 32 && layout->entries.offset == layout->fixed.offset + layout->fixed.size,
            "The packed entries at %#"PRIx64" do not follow the fixed bookkeeping.\n", layout->entries.offset);
    workgraph_check_pool_region(&layout->offsets, 4, used, &end, "Unrolled offsets");
    workgraph_check_pool_region(&layout->empty_state, 16, used, &end, "Empty-record state");
    ok(!layout->empty_state.size, "Got empty-record state size %#"PRIx64".\n", layout->empty_state.size);

    for (level = 1; level < graph->levels; level++)
    {
        ok(layout->entries.size >= exact[level][WG_POOL_ENTRIES] * 8 &&
                layout->offsets.size >= exact[level][WG_POOL_OFFSETS] * 4,
                "Pool %u: the shared regions are below the spec bound.\n", level);
        for (half = 0; half < 2; half++)
        {
            const struct workgraph_pool_region *payload = &layout->payload[level][half];
            const struct workgraph_pool_region *sink = &layout->sink[level][half];

            sprintf(what, "Pool %u half %u payload", level, half);
            workgraph_check_pool_region(payload, 16, used, &end, what);
            ok(payload->size >= exact[level][WG_POOL_PAYLOAD], "%s: %#"PRIx64" bytes is below the spec bound.\n",
                    what, payload->size);
            sprintf(what, "Pool %u half %u sink", level, half);
            workgraph_check_pool_region(sink, 16, used, &end, what);
            /* The producers address the payload and the sink with 32-bit offsets from the start of the payload. */
            ok(sink->offset == payload->offset + payload->size && sink->size == pools->sink_size &&
                    payload->size + sink->size <= UINT32_MAX, "%s: %#"PRIx64" bytes at %#"PRIx64".\n", what,
                    sink->size, sink->offset);
        }
    }
    for (level = graph->levels; level < WG_POOL_LEVELS; level++)
        for (half = 0; half < 2; half++)
            ok(!layout->payload[level][half].size && !layout->sink[level][half].size,
                    "Pool %u after the last level has %#"PRIx64" bytes.\n", level, layout->payload[level][half].size);
    for (half = 0; half < 2; half++)
        ok(!layout->payload[0][half].size, "Level 0 has a pool of %#"PRIx64" bytes.\n", layout->payload[0][half].size);

    workgraph_check_pool_region(&layout->staging, 16, used, &end, "Staging");
    ok(layout->staging.size == pools->staging_size, "Got staging size %#"PRIx64".\n", layout->staging.size);
    ok(layout->staging.offset == ((used - pools->staging_size) & ~(uint64_t)15), "Got staging at %#"PRIx64".\n",
            layout->staging.offset);

    /* Each region has at least its isolated size. The arena goes to the regions in proportion to their isolated sizes:
     * a region with the larger isolated size never gets the smaller part, and only alignment is left unused. */
    count = workgraph_pool_regions(graph, layout, w, regions, isolated);
    total = 0;
    for (a = 0; a < count; a++)
    {
        total += isolated[a];
        ok(regions[a]->size >= isolated[a], "Region %u: %#"PRIx64" bytes, isolated %#"PRIx64".\n", a,
                regions[a]->size, isolated[a]);
        if (regions[a]->size < isolated[a])
            return 0;
        arena += regions[a]->size - isolated[a];
        for (b = 0; b < count; b++)
        {
            if (isolated[a] >= isolated[b])
                ok(regions[a]->size - isolated[a] >= regions[b]->size - isolated[b],
                        "Region %u gets less of the arena than region %u.\n", a, b);
        }
    }
    /* Each part is the arena x isolated / total, rounded down to 16 bytes: the sum of the parts is at most 16 bytes per
     * region below the arena, so a part is at most 16 bytes per region away from its share of the sum. */
    for (a = 0; total && a < count; a++)
    {
        double share = (double)arena * (double)isolated[a] / (double)total;
        double part = (double)(regions[a]->size - isolated[a]);

        ok(part <= share + 16.0 * (count + 1) && part + 16.0 * (count + 1) >= share,
                "Region %u: %.0f arena bytes, its share is %.0f.\n", a, part, share);
    }
    last = graph->levels > 1 ? &layout->sink[graph->levels - 1][1] : &layout->empty_state;
    unused = layout->staging.offset - (last->offset + last->size);
    /* A graph without pool records has no region that grows. */
    if (total)
        ok(unused <= 32 * count + 16, "%#"PRIx64" bytes before the staging range are not used.\n", unused);
    return arena;
}

static void workgraph_check_pool_layouts(const struct workgraph_pool_graph *graph, const struct workgraph_pools *pools,
        const uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS], const uint64_t exact[WG_POOL_LEVELS][WG_POOL_COMPONENTS],
        const char *limits)
{
    static const uint64_t granule = 65536;
    uint64_t expected_size, expected_used, arena[WG_POOL_LAYOUTS];
    unsigned int n;

    for (n = 0; n < WG_POOL_LAYOUTS; n++)
    {
        const struct workgraph_pool_layout *layout = &pools->layouts[n];

        vkd3d_test_set_context("%s, %s, layout %u", graph->name, limits, n);
        arena[n] = 0;
        switch (n)
        {
            case 0: expected_size = pools->min_size - 16; break;
            case 1: expected_size = pools->min_size; break;
            case 2: expected_size = pools->min_size + granule + 8; break;
            case 3: expected_size = pools->max_size; break;
            default: expected_size = pools->max_size + granule; break;
        }
        /* Below Min there is no layout; above Max no region grows. */
        expected_used = n ? min(expected_size, pools->max_size) : 0;
        ok(layout->size == expected_size, "Expected size %#"PRIx64", got %#"PRIx64".\n", expected_size, layout->size);
        ok(layout->used == expected_used, "Expected %#"PRIx64" bytes used, got %#"PRIx64".\n", expected_used,
                layout->used);
        if (!n || layout->used != expected_used)
            continue;
        arena[n] = workgraph_check_pool_layout(graph, pools, layout, w, exact);
    }
    vkd3d_test_set_context("%s, %s", graph->name, limits);

    /* At Min the arena is what the 64 KiB granularity leaves; one more granule gives it more, unless Max is Min or no
     * region can grow. The layout above Max is the layout at Max. */
    ok(arena[1] < granule + 16, "Got %#"PRIx64" arena bytes at Min.\n", arena[1]);
    if (pools->max_size > pools->min_size && graph->levels > 1)
        ok(arena[2] > arena[1], "Expected more arena bytes at Min + 64 KiB, got %#"PRIx64".\n", arena[2]);
    else
        ok(arena[2] == arena[1], "Expected the arena of Min, got %#"PRIx64" bytes.\n", arena[2]);
    ok(arena[3] >= arena[2], "Got %#"PRIx64" arena bytes at Max.\n", arena[3]);
    ok(!memcmp(&pools->layouts[4].used, &pools->layouts[3].used,
            sizeof(pools->layouts[3]) - offsetof(struct workgraph_pool_layout, used)),
            "The layout above Max is not the layout at Max.\n");
}

/* The state object of try_create_workgraph_pso() with one explicitly defined node, Reader of pool_shared_reader, whose
 * first output gets MaxRecords 16 from a node output override. */
static HRESULT workgraph_create_output_override_pso(struct test_context_workgraph *context, D3D12_SHADER_BYTECODE code,
        ID3D12StateObject **pso)
{
    static const UINT max_records = 16;
    D3D12_BROADCASTING_LAUNCH_OVERRIDES launch;
    D3D12_NODE_OUTPUT_OVERRIDES output;
    D3D12_GLOBAL_ROOT_SIGNATURE grs_desc;
    D3D12_STATE_SUBOBJECT subobjects[3];
    D3D12_STATE_OBJECT_DESC pso_desc;
    D3D12_WORK_GRAPH_DESC wg_desc;
    D3D12_DXIL_LIBRARY_DESC dxil;
    D3D12_NODE node;

    memset(&pso_desc, 0, sizeof(pso_desc));
    memset(subobjects, 0, sizeof(subobjects));
    memset(&dxil, 0, sizeof(dxil));
    memset(&wg_desc, 0, sizeof(wg_desc));
    memset(&launch, 0, sizeof(launch));
    memset(&output, 0, sizeof(output));
    memset(&node, 0, sizeof(node));
    pso_desc.NumSubobjects = ARRAY_SIZE(subobjects);
    pso_desc.pSubobjects = subobjects;
    pso_desc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    grs_desc.pGlobalRootSignature = context->default_root_uav_rs;

    subobjects[0].pDesc = &dxil;
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = &wg_desc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    subobjects[2].pDesc = &grs_desc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;

    output.OutputIndex = 0;
    output.pMaxRecords = &max_records;
    launch.NumOutputOverrides = 1;
    launch.pOutputOverrides = &output;
    node.NodeType = D3D12_NODE_TYPE_SHADER;
    node.Shader.Shader = u"Reader";
    node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_BROADCASTING_LAUNCH;
    node.Shader.pBroadcastingLaunchOverrides = &launch;

    dxil.DXILLibrary = code;
    wg_desc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    wg_desc.ProgramName = u"Dummy";
    wg_desc.NumExplicitlyDefinedNodes = 1;
    wg_desc.pExplicitlyDefinedNodes = &node;

    *pso = NULL;
    return ID3D12Device5_CreateStateObject(context->device, &pso_desc, &IID_ID3D12StateObject, (void **)pso);
}

void test_workgraph_drain_layout(void)
{
    /* The lab flavour of the amdgpu-wddm engine computes the record pools of the draining scheduler at state object
     * creation: W of each pool (the worst-case output of one producer item into it, componentwise maximum over the
     * producers), the record bound (the most that one record with all its groups can write into the next pool), and a
     * layout of the backing memory for each size from Min. Only the lab flavour with rounds=<n> runs on them
     * (test_workgraph_drain_retention), and only then are the memory requirements the Min and Max of the pools. The
     * empty-record state has no size yet, so Min is not final. The test computes W and the bound
     * from the declarations of the HLSL source, independently of the engine, and checks them and the layouts below Min,
     * at Min, Min + 64 KiB + 8 bytes, Max and above Max: every region aligned, in order, inside the backing size, each
     * payload region with its sink directly after it, the packed entries directly after the output guard, each region
     * at least its isolated size and the spec bound, and the arena shared in proportion. The hooks pool_records and
     * pool_stride (AMDGPU_WDDM_WG_LIMITS) make Min larger than the requirement of today, so that Max is Min, and make a
     * payload region exceed its 32-bit limit or W exceed 64 bits, so that the pools are not valid while the state
     * object is. Nothing is dispatched. */
#include "shaders/workgraph/headers/basic_recursion.h"
#include "shaders/workgraph/headers/shared_inputs.h"
#include "shaders/workgraph/headers/two_level_broadcast_node_array.h"
#include "shaders/workgraph/headers/two_level_thread.h"
#include "shaders/workgraph/headers/pool_shared.h"
#include "shaders/workgraph/headers/pool_empty.h"
#include "shaders/workgraph/headers/pool_shared_reader.h"
#include "shaders/workgraph/headers/pool_retain.h"
#include "shaders/workgraph/headers/pool_charge.h"
    static const struct workgraph_pool_graph graphs[] =
    {
        /* Entry (grid 8) -> Mid (grid 8, MaxRecords 1, 4 bytes) -> Leaf (MaxRecords 64, 256 bytes). */
        { "level_sinks", &level_sinks_dxil, 3, { { 1, 0, false, 1, 4, 0, 8 }, { 2, 1, false, 64, 256, 0, 8 } }, 2 },
        /* Two entry points (broadcasting, grid 1), each MaxRecords 64 of 4 bytes into Child. */
        { "output_zero_counts", &output_zero_counts_dxil, 2,
                { { 1, 0, false, 64, 4, 0, 1 }, { 1, 1, false, 64, 4, 0, 1 } }, 2 },
        /* A thread launch entry with MaxRecords 2 into Recursive, which recurses 6 times with MaxRecords 2: level 7,
         * the last depth, writes nothing. */
        { "basic_recursion", &basic_recursion_dxil, 8,
                { { 1, 0, true, 2, 4, 0, 1 }, { 2, 1, true, 2, 4, 0, 1 }, { 3, 2, true, 2, 4, 0, 1 },
                { 4, 3, true, 2, 4, 0, 1 }, { 5, 4, true, 2, 4, 0, 1 }, { 6, 5, true, 2, 4, 0, 1 },
                { 7, 6, true, 2, 4, 0, 1 } }, 7 },
        /* A thread launch entry with MaxRecords 1 of 8 bytes into Broadcast, whose input TappedInput shares. */
        { "shared_inputs", &shared_inputs_dxil, 2, { { 1, 0, true, 1, 8, 0, 1 } }, 1 },
        /* A broadcasting entry (NodeMaxDispatchGrid 256), MaxRecords 8 for a node array of 2 nodes with 12-byte
         * records. */
        { "two_level_broadcast_node_array", &two_level_broadcast_node_array_dxil, 2,
                { { 1, 0, false, 8, 12, 0, 256 } }, 1 },
        /* A broadcasting entry (NodeMaxDispatchGrid 256), MaxRecords 256 of 8 bytes, and MaxRecordsSharedWith it of
         * 8 bytes. */
        { "two_level_thread", &two_level_thread_dxil, 2,
                { { 1, 0, false, 256, 8, 0, 256 }, { 1, 0, false, 256, 8, 1, 256 } }, 2 },
        /* MaxRecords 16 of 4 bytes and MaxRecordsSharedWith it of 64 bytes; MaxRecords 32 of an empty node and
         * MaxRecordsSharedWith it of 160 bytes. */
        { "pool_shared", &pool_shared_dxil, 2,
                { { 1, 0, false, 16, 4, 0, 1 }, { 1, 0, false, 16, 64, 1, 1 }, { 1, 0, false, 32, 0, 0, 1 },
                { 1, 0, false, 32, 160, 3, 1 } }, 4 },
        /* Empty records only: 4 payload bytes and one unrolled offset each, no packed entries. */
        { "pool_empty", &pool_empty_dxil, 2, { { 1, 0, false, 8, 0, 0, 1 } }, 1 },
        /* One level, no pool. */
        { "empty_count64", &empty_count64_dxil, 1, { { 0 } }, 0 },
        /* Entry (grid 1) -> Reader (grid 2, MaxRecords 2 of 16 bytes) and Tapped (grid 3, NodeShareInputOf Reader,
         * MaxRecords 8 of 64 bytes): W of pool 2 is the W of the shared reader, and the bound adds both readers. */
        { "pool_shared_reader", &pool_shared_reader_dxil, 3,
                { { 1, 0, false, 1, 4, 0, 1 }, { 2, 1, false, 2, 16, 0, 2 }, { 2, 2, false, 8, 64, 0, 3, 2 } }, 3 },
        /* Entry (grid 4, 256-byte records) -> Mid (grid 8: 32 Leaf records of 256 bytes, one empty record, one Side
         * record) and Tap (shares the input of Mid, one Fan record) -> Done (coalescing 8, one Join record for each
         * empty record), Side (grid 64, 2 Join records) and Fan (coalescing 4) -> Join. W of pool 1 is larger than 4
         * bytes for each unrolled offset of the largest W, so the offsets floor of pool 1 sets the unrolled offsets. */
        { "pool_retain", &pool_retain_dxil, 4,
                { { 1, 0, false, 1, 256, 0, 4 }, { 2, 1, false, 32, 256, 0, 8 }, { 2, 1, false, 1, 0, 0, 8 },
                { 2, 1, false, 1, 8, 0, 8 }, { 2, 2, false, 1, 8, 0, 1, 2 }, { 3, 3, false, 8, 8, 0, 1 },
                { 3, 4, false, 2, 8, 0, 64 }, { 3, 5, false, 4, 8, 0, 1 } }, 8 },
        /* Entry (grid 1) -> A and B (17 records of 8 bytes each; coalescing 4, 128 records of 256 bytes) -> Sink. */
        { "pool_charge", &pool_charge_dxil, 3,
                { { 1, 0, false, 17, 8, 0, 1 }, { 1, 0, false, 17, 8, 0, 1 }, { 2, 1, false, 128, 256, 0, 1 },
                { 2, 2, false, 128, 256, 0, 1 } }, 4 },
    };
    static const struct
    {
        unsigned int graph;
        const char *limits;
        uint32_t records, stride;
        bool valid, max_is_min, output_override;
    }
    tests[] =
    {
        { 0, NULL, 0, 0, true }, { 1, NULL, 0, 0, true }, { 2, NULL, 0, 0, true }, { 3, NULL, 0, 0, true },
        { 4, NULL, 0, 0, true }, { 5, NULL, 0, 0, true }, { 6, NULL, 0, 0, true }, { 7, NULL, 0, 0, true },
        { 8, NULL, 0, 0, true },
        /* One record per output: the unrolled offsets take 4 bytes, so the next region needs its alignment. */
        { 0, "pool_records=1", 1, 0, true },
        /* 256 MiB and 768 MiB payload regions: Min is above the requirement of today, so Max is Min. */
        { 0, "pool_records=4096,pool_stride=65536", 4096, 65536, true, true },
        { 6, "pool_records=4096,pool_stride=65536", 4096, 65536, true, true },
        /* 4 GiB for one output: the payload region and its sink exceed the 32-bit limit. */
        { 0, "pool_records=65536,pool_stride=65536", 65536, 65536, false },
        /* About 2^64 payload bytes for one output: the pools do not fit in 64 bits, for a group and for a wave. */
        { 0, "pool_records=4294967295,pool_stride=4294967280", 4294967295u, 4294967280u, false },
        { 2, "pool_records=4294967295,pool_stride=4294967280", 4294967295u, 4294967280u, false },
        { 9, NULL, 0, 0, true },
        /* The engine does not apply node output overrides: its metadata does not describe this graph, so the pools
         * are not valid while the state object is. */
        { 9, "output override", 0, 0, false, false, true },
        { 10, NULL, 0, 0, true },
        { 11, NULL, 0, 0, true },
    };
    uint64_t w[WG_POOL_LEVELS][WG_POOL_COMPONENTS], exact[WG_POOL_LEVELS][WG_POOL_COMPONENTS];
    uint64_t bound[WG_POOL_LEVELS][WG_POOL_COMPONENTS], exact_bound[WG_POOL_LEVELS][WG_POOL_COMPONENTS];
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs, default_reqs[ARRAY_SIZE(graphs)];
    struct test_context_workgraph context;
    ID3D12WorkGraphProperties *props;
    struct workgraph_pools *pools;
    ID3D12StateObject *pso;
    unsigned int i, level, c;
    bool frame;
    UINT size;
    HRESULT hr;

    if (!init_workgraph_test_context(&context))
        return;
    if (!workgraph_drain_expected(context.device, &frame))
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    pools = malloc(sizeof(*pools));
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        const struct workgraph_pool_graph *graph = &graphs[tests[i].graph];
        const char *limits = tests[i].limits ? tests[i].limits : "default";

        /* The hooks need the round frame of the lab flavour; other drivers have nothing to check. */
        if (tests[i].limits && !frame)
            continue;
        vkd3d_test_set_context("%s, %s", graph->name, limits);
        if (tests[i].output_override)
        {
            hr = workgraph_create_output_override_pso(&context, *graph->code, &pso);
        }
        else
        {
            workgraph_set_limits(tests[i].limits);
            hr = try_create_workgraph_pso(&context, *graph->code, NULL, &pso);
            workgraph_set_limits(NULL);
        }
        ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        if (!pso)
            continue;

        hr = ID3D12StateObject_QueryInterface(pso, &IID_ID3D12WorkGraphProperties, (void **)&props);
        ok(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties, hr #%x.\n", (int)hr);
        memset(&wg_reqs, 0, sizeof(wg_reqs));
        ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(props, 0, &wg_reqs);
        ID3D12WorkGraphProperties_Release(props);
        /* The pools do not change the requirements: the default rows of each graph come first. */
        if (!tests[i].limits)
            default_reqs[tests[i].graph] = wg_reqs;
        else if (!tests[i].output_override)
            ok(wg_reqs.MinSizeInBytes == default_reqs[tests[i].graph].MinSizeInBytes &&
                    wg_reqs.MaxSizeInBytes == default_reqs[tests[i].graph].MaxSizeInBytes,
                    "The hooks changed the requirements to %#"PRIx64" to %#"PRIx64".\n",
                    (uint64_t)wg_reqs.MinSizeInBytes, (uint64_t)wg_reqs.MaxSizeInBytes);

        memset(pools, 0, sizeof(*pools));
        size = sizeof(*pools);
        hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_pool_guid, &size, pools);
        if (!frame)
        {
            ok(FAILED(hr), "Got the record pools, but none were expected.\n");
            ID3D12StateObject_Release(pso);
            continue;
        }
        ok(hr == S_OK && size == sizeof(*pools), "Record pools: hr #%x, size %u.\n", (int)hr, size);
        ok(pools->valid == tests[i].valid, "Expected valid %u, got %u.\n", tests[i].valid, pools->valid);
        if (hr != S_OK || size != sizeof(*pools) || !pools->valid || !tests[i].valid)
        {
            ID3D12StateObject_Release(pso);
            continue;
        }
        ok(pools->pools == graph->levels, "Expected %u levels, got %u.\n", graph->levels, pools->pools);
        ok(!pools->execution, "Execution uses the pools.\n");
        ok(pools->sink_size >= 65536 && !(pools->sink_size % 16), "Got sink size %#"PRIx64".\n", pools->sink_size);
        if (pools->pools != graph->levels)
        {
            ID3D12StateObject_Release(pso);
            continue;
        }

        workgraph_pool_oracle(graph, false, tests[i].records, tests[i].stride, w, bound);
        workgraph_pool_oracle(graph, true, tests[i].records, tests[i].stride, exact, exact_bound);
        for (level = 0; level < WG_POOL_LEVELS; level++)
        {
            for (c = 0; c < WG_POOL_COMPONENTS; c++)
            {
                ok(pools->w[level][c] == w[level][c], "Pool %u component %u: expected W %"PRIu64", got %"PRIu64".\n",
                        level, c, w[level][c], pools->w[level][c]);
                ok(w[level][c] >= exact[level][c], "Pool %u component %u: W %"PRIu64" is below the spec bound %"
                        PRIu64".\n", level, c, w[level][c], exact[level][c]);
                ok(pools->bound[level][c] == bound[level][c], "Pool %u component %u: expected record bound %"PRIu64
                        ", got %"PRIu64".\n", level, c, bound[level][c], pools->bound[level][c]);
            }
        }

        /* Min is a multiple of 64 KiB, and Max is the requirement of today, or Min if it is larger. */
        ok(pools->min_size && !(pools->min_size % 65536), "Got Min %#"PRIx64".\n", pools->min_size);
        ok(pools->min_size == workgraph_pool_min(graph, pools, w), "Expected Min %#"PRIx64", got %#"PRIx64".\n",
                workgraph_pool_min(graph, pools, w), pools->min_size);
        ok(pools->max_size == max(pools->min_size, (uint64_t)wg_reqs.MaxSizeInBytes),
                "Got Max %#"PRIx64", requirement %#"PRIx64".\n", pools->max_size, (uint64_t)wg_reqs.MaxSizeInBytes);
        if (tests[i].max_is_min)
            ok(pools->max_size == pools->min_size, "Expected Max = Min, got %#"PRIx64".\n", pools->max_size);
        trace("%s, %s: Min %#"PRIx64", Max %#"PRIx64", requirements %#"PRIx64" to %#"PRIx64".\n", graph->name, limits,
                pools->min_size, pools->max_size, (uint64_t)wg_reqs.MinSizeInBytes, (uint64_t)wg_reqs.MaxSizeInBytes);
        workgraph_check_pool_layouts(graph, pools, w, exact, limits);

        ID3D12StateObject_Release(pso);
    }
    vkd3d_test_set_context(NULL);

    free(pools);
    destroy_workgraph_test_context(&context);
}

/* One half of the regions of the pool layout that get a part of the arena, for one backing size. Both halves of a pool
 * have the same size. */
struct workgraph_pool_sizes
{
    uint64_t entries;
    uint64_t offsets;
    uint64_t payload[WG_POOL_LEVELS];
};

static uint64_t workgraph_pool_align(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* The layout of the engine for backing_size bytes, from its reply alone: the isolated sizes from W (the unrolled
 * offsets also at least a payload half of pool 1), the regions placed from offset 0 with their alignments, and the
 * arena between them and the staging range shared in proportion to the isolated sizes, 16 bytes kept for each region,
 * each part rounded down to 16 bytes, then the 32-bit limits. Returns false if there is no layout. The test checks it
 * against the layouts of the reply before it uses it. */
static bool workgraph_pool_sizes_at(const struct workgraph_pools *pools, uint64_t backing_size,
        struct workgraph_pool_sizes *sizes)
{
    uint64_t isolated[WG_POOL_REGIONS], cursor, used, limit, arena, total = 0, part[WG_POOL_REGIONS];
    unsigned int level, half, count = 2, n;

    memset(sizes, 0, sizeof(*sizes));
    if (!pools->valid || backing_size < pools->min_size || pools->pools > WG_POOL_LEVELS)
        return false;
    used = min(backing_size, pools->max_size);
    if (used < pools->staging_size)
        return false;
    limit = (used - pools->staging_size) & ~(uint64_t)15;

    isolated[0] = isolated[1] = 0;
    for (level = 1; level < pools->pools; level++)
    {
        isolated[0] = max(isolated[0], pools->w[level][WG_POOL_ENTRIES] * 8);
        isolated[1] = max(isolated[1], pools->w[level][WG_POOL_OFFSETS] * 4);
        for (half = 0; half < 2; half++)
            isolated[count++] = workgraph_pool_align(pools->w[level][WG_POOL_PAYLOAD], 16);
    }
    if (pools->pools > 1)
        isolated[1] = max(isolated[1], workgraph_pool_align(pools->w[1][WG_POOL_PAYLOAD], 16));

    cursor = pools->fixed_size;
    cursor = workgraph_pool_align(cursor, 8) + isolated[0];
    cursor = workgraph_pool_align(cursor, 4) + isolated[1];
    cursor = workgraph_pool_align(cursor, 16);
    for (n = 2; n < count; n++)
    {
        cursor = workgraph_pool_align(cursor, 16) + isolated[n];
        cursor = workgraph_pool_align(cursor, 16) + pools->sink_size;
    }
    if (cursor > limit)
        return false;

    for (n = 0; n < count; n++)
        total += isolated[n];
    arena = limit - cursor;
    arena = arena > 16 * count ? arena - 16 * count : 0;
    for (n = 0; n < count; n++)
    {
        part[n] = 0;
        if (!total)
            continue;
        /* The sizes of the test are far below 2^32, so the product does not wrap. */
        if (isolated[n] && arena > UINT64_MAX / isolated[n])
            return false;
        part[n] = (arena * isolated[n] / total) & ~(uint64_t)15;
    }
    sizes->entries = min(isolated[0] + part[0], (uint64_t)UINT32_MAX * 8);
    sizes->offsets = min(isolated[1] + part[1], (uint64_t)UINT32_MAX * 4);
    for (level = 1; level < pools->pools; level++)
    {
        n = 2 + 2 * (level - 1);
        sizes->payload[level] = min(isolated[n] + part[n], ((uint64_t)UINT32_MAX - pools->sink_size) & ~(uint64_t)15);
    }
    return true;
}

/* The capacities of one half of pool L, as the engine gives them to the output guard and to the admission: the payload
 * bytes (for pool 1 at most 4 bytes for each unrolled offset), the packed entries and the unrolled offsets. */
static void workgraph_pool_caps(const struct workgraph_pool_sizes *sizes, unsigned int pool,
        uint64_t caps[WG_POOL_COMPONENTS])
{
    uint64_t offsets = min(sizes->offsets / 4, (uint64_t)UINT32_MAX);
    uint64_t payload = sizes->payload[pool];

    if (pool == 1)
        payload = min(payload, 4 * offsets);
    caps[WG_POOL_PAYLOAD] = min(payload, (uint64_t)UINT32_MAX) & ~(uint64_t)15;
    caps[WG_POOL_ENTRIES] = min(sizes->entries / 8, (uint64_t)UINT32_MAX);
    caps[WG_POOL_OFFSETS] = offsets;
}

/* True if records / divisor records of the level before pool L, each with the bound of the pool, fit one half of
 * it. */
static bool workgraph_pool_fits(const struct workgraph_pools *pools, const struct workgraph_pool_sizes *sizes,
        unsigned int pool, uint64_t records, uint64_t divisor)
{
    uint64_t caps[WG_POOL_COMPONENTS];
    unsigned int c;

    workgraph_pool_caps(sizes, pool, caps);
    for (c = 0; c < WG_POOL_COMPONENTS; c++)
    {
        if (pools->bound[pool][c] * records > caps[c] * divisor)
            return false;
    }
    return true;
}

/* A reference of the admission of one slot (cs_workgraph_pool.comp), for the CPU controls below and for the oracle of
 * level_sinks: k of count records with u records a group and the demand of one group, and the free space after it. */
static uint64_t workgraph_pool_admit(uint64_t count, uint64_t u, const uint64_t demand[WG_POOL_COMPONENTS],
        uint64_t free_space[WG_POOL_COMPONENTS])
{
    uint64_t groups = 0, k = count, charged, fit;
    bool bounded = false;
    unsigned int c;

    for (c = 0; c < WG_POOL_COMPONENTS; c++)
    {
        if (!demand[c])
            continue;
        fit = free_space[c] / demand[c];
        groups = bounded ? min(groups, fit) : fit;
        bounded = true;
    }
    if (!bounded)
        return k;
    k = min(k, groups * u);
    charged = k / u + (k % u ? 1 : 0);
    for (c = 0; c < WG_POOL_COMPONENTS; c++)
        free_space[c] -= charged * demand[c];
    return k;
}

/* The close of the round frame adds the records that the pools hold to a lo/hi pair of 32-bit words with a carry
 * (cs_workgraph_drain.comp). */
static void workgraph_pool_add_records(uint32_t *lo, uint32_t *hi, uint64_t records)
{
    uint32_t add_lo = (uint32_t)records, add_hi = (uint32_t)(records >> 32), old = *lo;

    *lo = old + add_lo;
    if (*lo < old)
        ++add_hi;
    *hi += add_hi;
}

/* CPU controls of the admission arithmetic and of the record sum of the close: zero demand, the largest u and free
 * space, the ceiling at a group boundary, and a sum of exactly 2^32 records over 8 pools. */
static void workgraph_check_pool_arithmetic(void)
{
    uint64_t free_space[WG_POOL_COMPONENTS], demand[WG_POOL_COMPONENTS], k;
    uint32_t lo, hi;
    unsigned int i;

    /* No demand: every record, no charge. */
    memset(demand, 0, sizeof(demand));
    free_space[0] = free_space[1] = free_space[2] = 0;
    k = workgraph_pool_admit(UINT32_MAX, 32, demand, free_space);
    ok(k == UINT32_MAX && !free_space[0], "Zero demand: got k %"PRIu64".\n", k);

    /* The largest u and free space: groups x u is (2^32 - 1)^2, below 2^64. */
    demand[0] = 1;
    free_space[0] = UINT32_MAX;
    k = workgraph_pool_admit(UINT32_MAX, UINT32_MAX, demand, free_space);
    ok(k == UINT32_MAX && free_space[0] == UINT32_MAX - 1, "Largest u: got k %"PRIu64", free %"PRIu64".\n", k,
            free_space[0]);

    /* u = 4, 3 groups fit: 12 records, 11 records cost 3 groups, 13 records are cut to 12. */
    demand[0] = 100;
    free_space[0] = 399;
    k = workgraph_pool_admit(11, 4, demand, free_space);
    ok(k == 11 && free_space[0] == 99, "Ceiling: got k %"PRIu64", free %"PRIu64".\n", k, free_space[0]);
    free_space[0] = 300;
    k = workgraph_pool_admit(13, 4, demand, free_space);
    ok(k == 12 && !free_space[0], "Boundary: got k %"PRIu64", free %"PRIu64".\n", k, free_space[0]);
    free_space[0] = 99;
    k = workgraph_pool_admit(13, 4, demand, free_space);
    ok(!k && free_space[0] == 99, "No group: got k %"PRIu64", free %"PRIu64".\n", k, free_space[0]);

    /* Two components: the smaller fit decides. */
    demand[1] = 2;
    free_space[0] = 1000;
    free_space[1] = 5;
    k = workgraph_pool_admit(100, 1, demand, free_space);
    ok(k == 2 && free_space[0] == 800 && free_space[1] == 1, "Two components: got k %"PRIu64".\n", k);

    /* 8 pools of 2^29 empty records: the sum is exactly 2^32, which a 32-bit sum gives as 0. */
    lo = hi = 0;
    for (i = 0; i < 8; i++)
        workgraph_pool_add_records(&lo, &hi, 1u << 29);
    ok(!lo && hi == 1, "Records 2^32: got lo %#x, hi %#x.\n", lo, hi);
    workgraph_pool_add_records(&lo, &hi, ((uint64_t)1 << 32) + UINT32_MAX);
    ok(lo == UINT32_MAX && hi == 2, "Records 2^33 - 1: got lo %#x, hi %#x.\n", lo, hi);
}

/* The backing size of a case. Pool 1 holds the records of the entry level in each case with extra rounds. */
enum workgraph_retain_size
{
    WG_RETAIN_FIT, /* The smallest size from Min (64 KiB steps) at which each pool holds the bound of one record. */
    WG_RETAIN_HALF, /* The smallest size at which a pool holds half the bound of one record, but not the bound. */
    WG_RETAIN_MIN, /* MinSizeInBytes. */
    WG_RETAIN_GROUPS, /* The smallest size from Min (16-byte steps) at which pool 2 holds 2 groups, but not the payload
                       * of 3. */
};

struct workgraph_retain_case
{
    const char *name;
    unsigned int graph;
    unsigned int rounds; /* AMDGPU_WDDM_WG_LIMITS=rounds=<n>. */
    enum workgraph_retain_size size;
    unsigned int reason; /* The expected close reason, or WG_RETAIN_ORACLE if the oracle of the graph gives it. */
    uint64_t abandoned; /* The expected abandoned records for WG_DRAIN_REASON_FAULT. */
    bool fewer; /* The rounds are 2 less than the case before used, which must be at least 3. */
};

#define WG_RETAIN_VALUES 1024
#define WG_RETAIN_ORACLE (~0u)

/* The pool states at the close (struct vkd3d_workgraph_pool_state: the head, then one slot of kept records and block
 * offset for each node). The slots of a pool add up to its record counts, each block begins at a multiple of 16 bytes
 * inside the payload bytes of the pool, and the pools hold the abandoned records. */
static void workgraph_check_pool_states(struct test_context_workgraph *context, ID3D12Resource *backing,
        const struct workgraph_drain_info *info)
{
    unsigned int pool, slot, slots;
    uint64_t records = 0, kept;
    const uint32_t *state;
    uint32_t *words;

    ok(info->pool_state_stride >= 16 && !(info->pool_state_stride % 16) && info->pool_states >= 2 &&
            info->pool_states <= WG_POOL_LEVELS, "Got %u pool states of %u bytes.\n", info->pool_states,
            info->pool_state_stride);
    if (info->pool_state_stride < 16 || info->pool_state_stride % 16 || info->pool_states < 2 ||
            info->pool_states > WG_POOL_LEVELS)
        return;
    words = malloc(info->pool_states * info->pool_state_stride);
    workgraph_read_buffer_range(context, backing, info->pool_state_offset,
            info->pool_states * info->pool_state_stride, words);
    slots = (info->pool_state_stride - 16) / 8;
    for (pool = 1; pool < info->pool_states; pool++)
    {
        state = words + pool * (info->pool_state_stride / 4);
        for (slot = 0, kept = 0; slot < slots; slot++)
        {
            kept += state[4 + 2 * slot];
            ok(!(state[5 + 2 * slot] % 16) && state[5 + 2 * slot] <= state[0],
                    "Pool %u, slot %u: block at %#x, %#x bytes used.\n", pool, slot, state[5 + 2 * slot], state[0]);
        }
        ok(kept == (uint64_t)state[2] + state[3], "Pool %u: %"PRIu64" records in the slots, %u + %u in the head.\n",
                pool, kept, state[2], state[3]);
        records += (uint64_t)state[2] + state[3];
    }
    ok(records == info->abandoned_records, "The pools hold %"PRIu64" records, %"PRIu64" abandoned.\n", records,
            info->abandoned_records);
    free(words);
}

/* The outcome of level_sinks for k Mid records admitted in each round: 8 Mid records, 512 Leaf records each. Pool 2 is
 * the input of the last level, so it holds nothing at the start of a round. Returns the close reason. */
static unsigned int workgraph_level_sinks_oracle(uint64_t k, unsigned int rounds, unsigned int *mids,
        uint32_t pending[WG_DRAIN_MAX_ROUNDS + 1], uint32_t executed[3])
{
    unsigned int r, left = 8;

    memset(pending, 0, (WG_DRAIN_MAX_ROUNDS + 1) * sizeof(*pending));
    memset(executed, 0, 3 * sizeof(*executed));
    for (r = 0; r <= rounds; r++)
    {
        executed[r / 32] |= 1u << (r % 32);
        if (!k && r)
        {
            /* Records waited and none was admitted: the self-check sets the fault, pending is 0. */
            *mids = 8 - left;
            return WG_DRAIN_REASON_FAULT;
        }
        left -= (unsigned int)min((uint64_t)left, k);
        pending[r] = !!left;
        if (!left)
            break;
    }
    *mids = 8 - left;
    return left ? WG_DRAIN_REASON_EXHAUSTED : WG_DRAIN_REASON_COMPLETE;
}

void test_workgraph_drain_retention(void)
{
    /* The lab flavour of the amdgpu-wddm engine runs the extra rounds of the draining scheduler on the record pools
     * (AMDGPU_WDDM_WG_LIMITS=rounds=<n>): a level with outputs admits the whole records that fit the next pool, and
     * keeps the other records for the next round. The cases run with a backing memory at which each pool holds the
     * bound of one record (the records then take several rounds), with too few rounds, and with a backing memory at
     * which a pool does not hold one record, which must end in a fault and not in a hang. Each case checks the
     * records, the close reason, the abandoned records, that no record is lost, and 64 KiB after the backing memory.
     * At each close of the lab flavour, the pool states must agree with the abandoned records, and each retained block
     * must begin at a multiple of 16 bytes.
     * The graphs: level_sinks (a chain with an exact oracle), pool_retain (shared readers, empty and nonempty records
     * in one pool, coalescing readers, a diamond, 256-byte entry records, retention at level 2), basic_recursion (8
     * levels), cross_group_sharing (an RW input, with no throttled level), pool_wide_pair (one record of two readers
     * with a grid of 512 each) and pool_charge (groups that are not full, blocks of 8-byte records). Other drivers
     * ignore the switches and give the same records. Each case launches at most 16384 records. */
#include "shaders/workgraph/headers/basic_recursion.h"
#include "shaders/workgraph/headers/cross_group_sharing.h"
#include "shaders/workgraph/headers/pool_retain.h"
#include "shaders/workgraph/headers/pool_wide_pair.h"
#include "shaders/workgraph/headers/pool_charge.h"
    static const struct
    {
        const D3D12_SHADER_BYTECODE *code;
        const WCHAR *entry;
        bool input; /* The entry point has an input record of 4 bytes. */
        unsigned int entry_records; /* The records of the entry level that pool 1 must hold. */
    }
    graphs[] =
    {
        { &level_sinks_dxil, u"Entry", true, 1 },
        { &pool_retain_dxil, u"Entry", false, 1 },
        { &basic_recursion_dxil, u"EntryNode", false, 1 },
        { &cross_group_sharing_dxil, u"EntryNode", false, 1 },
        { &pool_wide_pair_dxil, u"Entry", false, 1 },
        { &pool_charge_dxil, u"Entry", false, 1 },
    };
    static const struct workgraph_retain_case tests[] =
    {
        { "chain", 0, 64, WG_RETAIN_FIT, WG_RETAIN_ORACLE },
        { "chain, too few rounds", 0, 1, WG_RETAIN_FIT, WG_RETAIN_ORACLE },
        { "chain, drain off", 0, 0, WG_RETAIN_MIN, WG_DRAIN_REASON_COMPLETE },
        { "shared, empty, diamond", 1, 64, WG_RETAIN_FIT, WG_RETAIN_ORACLE },
        { "shared, empty, diamond, too few rounds", 1, 0, WG_RETAIN_FIT, WG_RETAIN_ORACLE, 0, true },
        { "recursion", 2, 8, WG_RETAIN_FIT, WG_DRAIN_REASON_COMPLETE },
        { "RW sharing", 3, 4, WG_RETAIN_FIT, WG_DRAIN_REASON_COMPLETE },
        { "two readers", 4, 4, WG_RETAIN_FIT, WG_DRAIN_REASON_COMPLETE },
        { "two readers, grid larger than the pool", 4, 4, WG_RETAIN_MIN, WG_DRAIN_REASON_FAULT, 1 },
        { "two readers, each fits but not both", 4, 4, WG_RETAIN_HALF, WG_DRAIN_REASON_FAULT, 1 },
        { "partial groups", 5, 64, WG_RETAIN_GROUPS, WG_RETAIN_ORACLE },
        { "partial groups, too few rounds", 5, 1, WG_RETAIN_GROUPS, WG_RETAIN_ORACLE },
    };
    /* pool_charge: the Sink records after each round, at 2 groups of pool 2. Round 1 and 2 admit 8 records of the
     * first slot, round 3 its last record and 4 records of the other slot, round 4 8 and round 5 5 records. */
    static const unsigned int charge_admitted[] = { 8, 16, 21, 29, 34 };
    static const uint32_t recursion_reference[64] = { 0, 7, 13, 17, 25, 34, 46, 59, 71, 76, 68, 47, 23, 7, 1 };
    uint32_t expected_pending[WG_DRAIN_MAX_ROUNDS + 1], expected_executed[3], *values, *tail_pattern;
    ID3D12Resource *backing, *output;
    struct workgraph_pool_sizes sizes, check_sizes;
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS wg_reqs;
    struct workgraph_drain_control control;
    D3D12_DISPATCH_GRAPH_DESC dispatch_desc;
    struct test_context_workgraph context;
    ID3D12GraphicsCommandList10 *list10;
    D3D12_SET_PROGRAM_DESC program_desc;
    uint64_t caps[WG_POOL_COMPONENTS], k;
    struct workgraph_lost_records lost;
    unsigned int i, j, n, reason, mids;
    struct workgraph_drain_info info;
    ID3D12GraphicsCommandList *list;
    D3D12_PROGRAM_IDENTIFIER ident;
    struct workgraph_pools *pools;
    uint64_t backing_size, size;
    D3D12_NODE_ID entry_id;
    ID3D12StateObject *pso;
    uint32_t params = 0;
    UINT entry_index;
    unsigned int rounds, used_rounds = 0;
    bool frame, fits, half;
    char limits[64];
    UINT data_size;
    HRESULT hr;

    workgraph_check_pool_arithmetic();

    if (!workgraph_is_bounded_runner())
    {
        skip("Set VKD3D_TEST_WORKGRAPH_BOUNDED=1 in a runner with a time bound to run the retention cases.\n");
        return;
    }
    if (!init_workgraph_test_context(&context))
        return;
    if (!workgraph_drain_expected(context.device, &frame))
    {
        destroy_workgraph_test_context(&context);
        return;
    }

    list = context.context.list;
    pools = malloc(sizeof(*pools));
    values = malloc(WG_RETAIN_VALUES * sizeof(*values));
    tail_pattern = malloc(64 * 1024);
    for (i = 0; i < 64 * 1024 / sizeof(uint32_t); i++)
        tail_pattern[i] = 0x7a11c0deu;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        const struct workgraph_retain_case *test = &tests[i];
        char name[16];

        /* VKD3D_TEST_WORKGRAPH_CASE=case<i>. runs only case i. */
        sprintf(name, "case%u.", i);
        if (!workgraph_case_enabled(name))
            continue;
        /* The case before must have used at least 3 rounds, so that 2 less is at least 1. */
        rounds = tests[i].rounds;
        if (test->fewer && frame)
        {
            ok(used_rounds >= 3, "The case before used %u rounds.\n", used_rounds);
            if (used_rounds < 3)
                continue;
            rounds = used_rounds - 2;
        }
        else if (test->fewer)
        {
            rounds = 1;
        }
        if (rounds)
            sprintf(limits, "rounds=%u,canary=1", rounds);
        else
            strcpy(limits, "ring=8388608,canary=1");
        vkd3d_test_set_context("Test %u (%s), %s", i, test->name, limits);

        workgraph_set_limits(limits);
        hr = try_create_workgraph_pso(&context, *graphs[test->graph].code, NULL, &pso);
        workgraph_set_limits(NULL);
        ok(hr == S_OK, "Failed to create state object, hr #%x.\n", (int)hr);
        if (!pso)
            continue;
        entry_id.Name = graphs[test->graph].entry;
        entry_id.ArrayIndex = 0;
        if (!workgraph_get_program(pso, &ident, &wg_reqs, &entry_index, &entry_id, 1))
        {
            ID3D12StateObject_Release(pso);
            continue;
        }

        /* The backing size. Without the lab diagnostics there is no layout: the case runs at Min, and only the records
         * of a complete run are checked. */
        backing_size = wg_reqs.MinSizeInBytes;
        memset(pools, 0, sizeof(*pools));
        memset(&info, 0, sizeof(info));
        if (frame)
        {
            data_size = sizeof(*pools);
            hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_pool_guid, &data_size, pools);
            ok(hr == S_OK && data_size == sizeof(*pools), "Record pools: hr #%x, size %u.\n", (int)hr, data_size);
            ok(pools->execution == !!rounds, "Expected pool execution %u, got %u.\n", !!rounds,
                    pools->execution);
            if (rounds && (!pools->valid || !pools->execution))
            {
                ID3D12StateObject_Release(pso);
                continue;
            }
            data_size = sizeof(info);
            hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_drain_guid, &data_size, &info);
            ok(!workgraph_drain_info_error(hr, data_size, &info, rounds, 0, wg_reqs.MinSizeInBytes),
                    "Round frame not valid, hr #%x, size %u.\n", (int)hr, data_size);
        }
        if (frame && rounds)
        {
            ok(pools->min_size == wg_reqs.MinSizeInBytes, "Expected Min %#"PRIx64", got %#"PRIx64".\n",
                    pools->min_size, (uint64_t)wg_reqs.MinSizeInBytes);
            /* The layout of the test must be the layout of the engine at each size of the reply. */
            for (n = 0; n < WG_POOL_LAYOUTS; n++)
            {
                const struct workgraph_pool_layout *layout = &pools->layouts[n];

                fits = workgraph_pool_sizes_at(pools, layout->size, &check_sizes);
                ok(fits == !!layout->used, "Layout %u: expected a layout %u.\n", n, !!layout->used);
                if (!fits || !layout->used)
                    continue;
                ok(check_sizes.entries == layout->entries.size && check_sizes.offsets == layout->offsets.size,
                        "Layout %u: expected entries %#"PRIx64", offsets %#"PRIx64", got %#"PRIx64", %#"PRIx64".\n",
                        n, check_sizes.entries, check_sizes.offsets, layout->entries.size, layout->offsets.size);
                for (j = 1; j < pools->pools; j++)
                {
                    ok(check_sizes.payload[j] == layout->payload[j][0].size &&
                            check_sizes.payload[j] == layout->payload[j][1].size,
                            "Layout %u, pool %u: expected payload %#"PRIx64", got %#"PRIx64", %#"PRIx64".\n", n, j,
                            check_sizes.payload[j], layout->payload[j][0].size, layout->payload[j][1].size);
                }
            }

            backing_size = 0;
            for (size = pools->min_size; size <= pools->max_size && !backing_size;
                    size += test->size == WG_RETAIN_GROUPS ? 16 : 65536)
            {
                if (!workgraph_pool_sizes_at(pools, size, &sizes) ||
                        !workgraph_pool_fits(pools, &sizes, 1, graphs[test->graph].entry_records, 1))
                {
                    /* Pool 1 must hold the records of the entry level at Min for a case at Min. */
                    if (test->size == WG_RETAIN_MIN)
                        break;
                    continue;
                }
                if (test->size == WG_RETAIN_GROUPS)
                {
                    workgraph_pool_caps(&sizes, 2, caps);
                    if (workgraph_pool_fits(pools, &sizes, 2, 2, 1) &&
                            caps[WG_POOL_PAYLOAD] < 3 * pools->bound[2][WG_POOL_PAYLOAD])
                        backing_size = size;
                    continue;
                }
                fits = half = true;
                for (j = 2; j < pools->pools; j++)
                {
                    fits = fits && workgraph_pool_fits(pools, &sizes, j, 1, 1);
                    half = half && workgraph_pool_fits(pools, &sizes, j, 1, 2);
                }
                if ((test->size == WG_RETAIN_MIN && !fits) || (test->size == WG_RETAIN_FIT && fits) ||
                        (test->size == WG_RETAIN_HALF && half && !fits))
                    backing_size = size;
                /* A case at Min runs at Min only. */
                if (test->size == WG_RETAIN_MIN)
                    break;
            }
            ok(backing_size, "No backing size for the case.\n");
            if (!backing_size)
            {
                ID3D12StateObject_Release(pso);
                continue;
            }
            trace("Backing size %#"PRIx64" (Min %#"PRIx64").\n", backing_size, pools->min_size);
        }

        backing = create_default_buffer(context.context.device, backing_size + 64 * 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        upload_buffer_data(backing, backing_size, 64 * 1024, tail_pattern, context.context.queue, list);
        reset_command_list(list, context.context.allocator);
        output = create_default_buffer(context.context.device, WG_RETAIN_VALUES * sizeof(*values),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        hr = ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&list10);
        ok(SUCCEEDED(hr), "Failed to query ID3D12GraphicsCommandList10, hr #%x.\n", (int)hr);
        ID3D12GraphicsCommandList_SetComputeRootSignature(list, context.default_root_uav_rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0,
                ID3D12Resource_GetGPUVirtualAddress(output));
        memset(&program_desc, 0, sizeof(program_desc));
        program_desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        program_desc.WorkGraph.BackingMemory.SizeInBytes = backing_size;
        program_desc.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(backing);
        program_desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        program_desc.WorkGraph.ProgramIdentifier = ident;
        ID3D12GraphicsCommandList10_SetProgram(list10, &program_desc);
        memset(&dispatch_desc, 0, sizeof(dispatch_desc));
        dispatch_desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dispatch_desc.NodeCPUInput.EntrypointIndex = entry_index;
        dispatch_desc.NodeCPUInput.NumRecords = 1;
        dispatch_desc.NodeCPUInput.pRecords = graphs[test->graph].input ? &params : NULL;
        dispatch_desc.NodeCPUInput.RecordStrideInBytes = graphs[test->graph].input ? sizeof(params) : 0;
        ID3D12GraphicsCommandList10_DispatchGraph(list10, &dispatch_desc);
        ID3D12GraphicsCommandList10_Release(list10);

        workgraph_read_output(&context, output, values, WG_RETAIN_VALUES);
        if (test->graph <= 1 || test->graph == 5)
        {
            ok(values[2] == 0, "%u records have wrong contents.\n", values[2]);
            ok(values[3] == 0, "%u records arrived more than once.\n", values[3]);
        }
        n = workgraph_count_damaged_words(&context, backing, backing_size, 64 * 1024, 0x7a11c0deu);
        ok(!n, "%u words after the backing memory were written.\n", n);

        memset(&control, 0, sizeof(control));
        if (frame && rounds)
        {
            workgraph_read_buffer_range(&context, backing, info.control_offset, sizeof(control), (uint32_t *)&control);
            for (j = 0, used_rounds = 0; j <= WG_DRAIN_MAX_ROUNDS; j++)
                used_rounds += !!(control.executed[j / 32] & (1u << (j % 32)));
            trace("%u rounds.\n", used_rounds);
        }
        if (frame)
        {
            memset(&info, 0, sizeof(info));
            data_size = sizeof(info);
            hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_drain_guid, &data_size, &info);
            ok(hr == S_OK && data_size == sizeof(info), "Failed to get the round frame, hr #%x.\n", (int)hr);
            memset(&lost, 0, sizeof(lost));
            data_size = sizeof(lost);
            hr = ID3D12StateObject_GetPrivateData(pso, &workgraph_status_guid, &data_size, &lost);
            ok(hr == S_OK && data_size == sizeof(lost), "Failed to get the lost records, hr #%x.\n", (int)hr);
            ok(!lost.events && !lost.records, "Lost %"PRIu64" records in %"PRIu64" events.\n", lost.records,
                    lost.events);
            if (rounds)
                workgraph_check_pool_states(&context, backing, &info);
        }

        reason = test->reason;
        switch (test->graph)
        {
            case 0:
                /* level_sinks: Mid broadcasts (u = 1), and the bound of pool 2 is the demand of one Mid record. */
                k = 8;
                if (frame && rounds)
                {
                    workgraph_pool_sizes_at(pools, backing_size, &sizes);
                    workgraph_pool_caps(&sizes, 2, caps);
                    k = workgraph_pool_admit(8, 1, pools->bound[2], caps);
                    trace("%"PRIu64" Mid records a round.\n", k);
                    reason = workgraph_level_sinks_oracle(k, rounds, &mids, expected_pending,
                            expected_executed);
                    for (j = 0; j <= WG_DRAIN_MAX_ROUNDS; j++)
                        ok(control.pending[j] == expected_pending[j], "Round %u: expected pending %u, got %u.\n",
                                j, expected_pending[j], control.pending[j]);
                    for (j = 0; j < 3; j++)
                        ok(control.executed[j] == expected_executed[j], "Executed word %u: expected %#x, got %#x.\n",
                                j, expected_executed[j], control.executed[j]);
                    if (test->size == WG_RETAIN_FIT)
                        ok(k && k < 8, "Expected retention, got %"PRIu64" Mid records a round.\n", k);
                }
                else
                {
                    mids = 8;
                }
                ok(values[0] == 512 * mids, "Expected %u Leaf records, got %u.\n", 512 * mids, values[0]);
                if (frame && rounds)
                    ok(info.abandoned_records == 8 - mids, "Expected %u abandoned records, got %"PRIu64".\n",
                            8 - mids, info.abandoned_records);
                break;

            case 1:
                /* pool_retain: each admitted Mid record gives 256 Leaf records and 11 records of level 2 (2 Side, 1
                 * Fan, 8 empty). A Side record gives 128 Join records, a Fan record and an empty record one each. The
                 * records that are not in the outputs are the abandoned records: 4 - m Mid records, 11m - s - f - e
                 * records of level 2 and 128s + f + e - j Join records, for s Side records: 4 + 10m + 127s - j. */
                ok(!(values[0] % 256), "Got %u Leaf records.\n", values[0]);
                mids = values[0] / 256;
                ok(values[5] <= 2 * mids && values[1] <= 128 * values[5] + 9 * mids,
                        "Got %u Side and %u Join records for %u Mid records.\n", values[5], values[1], mids);
                if (frame && rounds)
                {
                    ok(info.abandoned_records == 4 + 10 * mids + 127 * values[5] - values[1],
                            "Expected %u abandoned records, got %"PRIu64".\n",
                            4 + 10 * mids + 127 * values[5] - values[1], info.abandoned_records);
                    /* Pool 3 holds less than 2 Side records, so level 2 keeps Side records across rounds. */
                    workgraph_pool_sizes_at(pools, backing_size, &sizes);
                    ok(!workgraph_pool_fits(pools, &sizes, 3, 2, 1), "Expected pool 3 to hold 1 Side record.\n");
                    if (rounds < WG_DRAIN_MAX_ROUNDS)
                    {
                        ok(info.abandoned_records, "Expected abandoned records.\n");
                        reason = WG_DRAIN_REASON_EXHAUSTED;
                    }
                    else
                    {
                        reason = WG_DRAIN_REASON_COMPLETE;
                        ok(control.pending[0] == 1, "Expected retention in round 1.\n");
                    }
                }
                else
                {
                    reason = WG_DRAIN_REASON_COMPLETE;
                }
                if (reason == WG_DRAIN_REASON_COMPLETE)
                {
                    ok(mids == 4 && values[5] == 8 && values[1] == 1060,
                            "Expected 1024 Leaf, 8 Side and 1060 Join records, got %u, %u, %u.\n", values[0],
                            values[5], values[1]);
                    for (j = 0; j < 32; j++)
                        ok(values[8 + j] == ~0u && values[64 + j] == ~0u, "Leaf word %u: %#x, Side Join word: %#x.\n",
                                j, values[8 + j], values[64 + j]);
                    ok(values[40] == 0xfu && values[41] == 32 && values[4] == 32, "Fan %#x, Done Join %u, Done %u.\n",
                            values[40], values[41], values[4]);
                }
                break;

            case 2:
                for (j = 0; j < ARRAY_SIZE(recursion_reference); j++)
                    ok(values[j] == recursion_reference[j], "Value %u: expected %u, got %u.\n", j,
                            recursion_reference[j], values[j]);
                break;

            case 3:
                for (j = 0; j < 128; j++)
                    ok(values[j] == (j < 64 ? 512 * 64 + 9 : 256 * 64 + 11), "Value %u: got %u.\n", j, values[j]);
                break;

            case 4:
                /* pool_wide_pair: one record, 8192 records for each reader, or none after a fault. */
                n = frame && rounds && reason == WG_DRAIN_REASON_FAULT ? 0 : 8192;
                ok(values[0] == n && values[1] == n && !values[2], "Expected %u records each, got %u, %u, %u bad.\n",
                        n, values[0], values[1], values[2]);
                if (frame && rounds)
                    ok(info.abandoned_records == (reason == WG_DRAIN_REASON_FAULT ? test->abandoned : 0),
                            "Expected %"PRIu64" abandoned records, got %"PRIu64".\n", test->abandoned,
                            info.abandoned_records);
                break;

            case 5:
                /* pool_charge: each admitted record gives one Sink record, and each group 128 Sink records. */
                n = frame && rounds ? charge_admitted[min(rounds, ARRAY_SIZE(charge_admitted) - 1)] : 34;
                ok(values[0] == n && !((values[0] + values[1]) % 128), "Expected %u Sink records, got %u and %u.\n",
                        n, values[0], values[1]);
                ok(vkd3d_popcount(values[8] & 0x1ffffu) + vkd3d_popcount(values[9] & 0x1ffffu) == n &&
                        !(values[8] & ~0x1ffffu) && !(values[9] & ~0x1ffffu), "Got ids %#x, %#x.\n", values[8],
                        values[9]);
                reason = n == 34 ? WG_DRAIN_REASON_COMPLETE : WG_DRAIN_REASON_EXHAUSTED;
                if (frame && rounds)
                {
                    ok(info.abandoned_records == 34 - n, "Expected %u abandoned records, got %"PRIu64".\n", 34 - n,
                            info.abandoned_records);
                    ok(used_rounds == min(rounds + 1, ARRAY_SIZE(charge_admitted)), "Got %u rounds.\n", used_rounds);
                }
                break;
        }

        if (frame && rounds)
        {
            ok(control.close_reason == reason, "Expected close reason %u, got %u.\n", reason, control.close_reason);
            ok(control.fault == (reason == WG_DRAIN_REASON_FAULT), "Got fault %u.\n", control.fault);
            ok(control.closes == 1, "Expected 1 close, got %u.\n", control.closes);
            ok(info.abandoned_events == (reason != WG_DRAIN_REASON_COMPLETE), "Got %u abandonment events.\n",
                    info.abandoned_events);
            ok(info.abandoned_reasons == (reason != WG_DRAIN_REASON_COMPLETE ? 1u << reason : 0),
                    "Got reasons %#x.\n", info.abandoned_reasons);
            if (reason == WG_DRAIN_REASON_COMPLETE)
                ok(!info.abandoned_records, "Got %"PRIu64" abandoned records.\n", info.abandoned_records);
        }

        ID3D12Resource_Release(output);
        ID3D12Resource_Release(backing);
        ID3D12StateObject_Release(pso);
    }
    vkd3d_test_set_context(NULL);

    hr = ID3D12Device5_GetDeviceRemovedReason(context.device);
    ok(hr == S_OK, "Device removed, hr #%x.\n", (int)hr);
    free(tail_pattern);
    free(values);
    free(pools);
    destroy_workgraph_test_context(&context);
}
