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
