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
