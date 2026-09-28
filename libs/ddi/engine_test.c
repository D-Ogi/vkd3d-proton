/* SPDX-License-Identifier: MIT
 *
 * bc250vkd3d_engine_test: checks bc250vkd3d.dll (bc250_vkd3d_engine.h, ABI 1.0) on its own, the way the
 * D3D12 shell will use it, without the Microsoft runtime:
 *   - the export and its version rules (E_NOINTERFACE, E_INVALIDARG);
 *   - CreateDevice over a caller-supplied Vulkan entry point, adapter chosen by LUID (V1, V2), an unknown LUID
 *     refused, two calls giving two devices (independent);
 *   - the capabilities the shell's GetCaps will mirror: feature level, the FL 12_0/12_1 tiers, shader model,
 *     ray tracing tier;
 *   - one GPU round trip: UPLOAD -> DEFAULT -> READBACK copy on a direct queue, fence wait, word-exact compare;
 *   - one compute dispatch: DXIL cs_6_0 with an embedded root signature writes a raw UAV reached through a
 *     shader-visible descriptor table at start + 3 * increment, word-exact compare; the handles are printed;
 *   - teardown: the device's final Release returns 0 (V4).
 *
 * Usage: bc250vkd3d_engine_test.exe <path to bc250vkd3d.dll> [adapter substring] [--icd <path>] [--fl <hex>]
 *   adapter substring  picks the DXGI adapter whose description contains it (default: first hardware adapter)
 *   --icd              loads that Vulkan driver DLL directly (entry vk_icdGetInstanceProcAddr), as the shell
 *                      loads hosted RADV, instead of the Vulkan loader (vulkan-1.dll)
 *   --fl               MinimumFeatureLevel for CreateDevice, default b000 (11_0)
 * Exit code 0 = all checks passed. "icd:" lines name every loaded Vulkan driver and its SHA-256.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_vkd3d_engine.h"
#include "engine_test_cs.h"

static unsigned int failures;

static void check(BOOL ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static void print_sha256(const WCHAR *path)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    UCHAR digest[32], buffer[65536];
    BOOL ok = TRUE;
    HANDLE file;
    DWORD read;
    unsigned int i;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        printf("icd: %ls sha256 unavailable (%lu)\n", path, GetLastError());
        return;
    }
    ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))
            && BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0));
    while (ok)
    {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL))
            ok = FALSE;
        else if (!read)
            break;
        else
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0));
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);

    printf("icd: %ls sha256 ", path);
    for (i = 0; i < sizeof(digest); ++i)
        printf(ok ? "%02X" : "", digest[i]);
    printf("%s\n", ok ? "" : "unavailable");
}

/* Which Vulkan driver answered: every loaded module exporting the ICD entry point. */
static void print_icd_modules(void)
{
    HMODULE modules[1024];
    WCHAR path[32768];
    DWORD needed = 0, i;

    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    {
        printf("icd: module list unavailable (%lu)\n", GetLastError());
        return;
    }
    for (i = 0; i < needed / sizeof(HMODULE) && i < ARRAYSIZE(modules); ++i)
    {
        if (!GetProcAddress(modules[i], "vk_icdGetInstanceProcAddr"))
            continue;
        if (GetModuleFileNameW(modules[i], path, ARRAYSIZE(path)))
            print_sha256(path);
    }
}

static BOOL find_adapter(const char *filter, LUID *luid)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter;
    DXGI_ADAPTER_DESC1 desc;
    char name[256];
    BOOL found = FALSE;
    UINT i;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return FALSE;
    for (i = 0; !found && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; ++i)
    {
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
            if (!filter || strstr(name, filter))
            {
                printf("adapter: %s, LUID %08lx:%08lx\n", name, (unsigned long)desc.AdapterLuid.HighPart,
                        (unsigned long)desc.AdapterLuid.LowPart);
                *luid = desc.AdapterLuid;
                found = TRUE;
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return found;
}

static void print_caps(ID3D12Device *device)
{
    static const D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2,
    };
    static const D3D_SHADER_MODEL models[] =
    {
        D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    HRESULT hr;
    unsigned int i;

    memset(&fl, 0, sizeof(fl));
    fl.NumFeatureLevels = ARRAYSIZE(levels);
    fl.pFeatureLevelsRequested = levels;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    check(SUCCEEDED(hr), "CheckFeatureSupport(FEATURE_LEVELS)");
    printf("caps: MaxSupportedFeatureLevel %x\n", (unsigned int)fl.MaxSupportedFeatureLevel);

    memset(&o, 0, sizeof(o));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    check(SUCCEEDED(hr), "CheckFeatureSupport(D3D12_OPTIONS)");
    printf("caps: TiledResourcesTier %u ResourceBindingTier %u ROVsSupported %u ConservativeRasterizationTier %u "
            "TypedUAVLoadAdditionalFormats %u\n", (unsigned int)o.TiledResourcesTier,
            (unsigned int)o.ResourceBindingTier, (unsigned int)o.ROVsSupported,
            (unsigned int)o.ConservativeRasterizationTier, (unsigned int)o.TypedUAVLoadAdditionalFormats);

    for (i = 0; i < ARRAYSIZE(models); ++i)
    {
        sm.HighestShaderModel = models[i];
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            break;
    }
    printf("caps: HighestShaderModel %x\n", i < ARRAYSIZE(models) ? (unsigned int)sm.HighestShaderModel : 0u);

    memset(&o5, 0, sizeof(o5));
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
        printf("caps: RaytracingTier %u\n", (unsigned int)o5.RaytracingTier);
}

static ID3D12Resource *create_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, UINT64 size,
        D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap;
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource *resource = NULL;

    memset(&heap, 0, sizeof(heap));
    heap.Type = type;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource)))
        return NULL;
    return resource;
}

/* UPLOAD -> DEFAULT -> READBACK on a direct queue, then a fence wait and a word-exact compare. */
static void copy_round_trip(ID3D12Device *device)
{
    enum { WORDS = 16384 };
    const UINT64 size = WORDS * sizeof(UINT32);
    ID3D12Resource *upload = NULL, *gpu = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    ID3D12CommandQueue *queue = NULL;
    D3D12_RESOURCE_BARRIER barrier;
    ID3D12Fence *fence = NULL;
    D3D12_RANGE range;
    HANDLE event = NULL;
    UINT32 *words;
    unsigned int i, bad = 0;
    DWORD wait;

    upload = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    gpu = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    check(upload && gpu && readback, "create UPLOAD, DEFAULT and READBACK buffers (64 KiB)");
    if (!upload || !gpu || !readback)
        goto done;
    printf("va: upload %llx default %llx readback %llx\n",
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(upload),
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(gpu),
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(readback));
    check(ID3D12Resource_GetGPUVirtualAddress(upload) && ID3D12Resource_GetGPUVirtualAddress(gpu)
            && ID3D12Resource_GetGPUVirtualAddress(readback), "GetGPUVirtualAddress nonzero for all three");

    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(upload, 0, &range, (void **)&words)))
    {
        check(FALSE, "Map UPLOAD");
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        words[i] = 0x9e3779b9u * (i + 1);
    ID3D12Resource_Unmap(upload, 0, NULL);

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue))
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)),
            "create direct queue, allocator, command list and fence");
    if (!queue || !allocator || !list || !fence)
        goto done;

    ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu, 0, upload, 0, size);
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = gpu;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, gpu, 0, size);
    check(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "record copy, barrier, copy");

    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    check(SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, 1)), "queue Signal(fence, 1)");
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    check(event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, 1, event)), "SetEventOnCompletion(1)");
    wait = event ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
    check(wait == WAIT_OBJECT_0 && ID3D12Fence_GetCompletedValue(fence) >= 1, "fence reaches 1 within 5 s");
    if (wait != WAIT_OBJECT_0)
        goto done;

    range.Begin = 0;
    range.End = (SIZE_T)size;
    if (FAILED(ID3D12Resource_Map(readback, 0, &range, (void **)&words)))
    {
        check(FALSE, "Map READBACK");
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        bad += words[i] != 0x9e3779b9u * (i + 1);
    memset(&range, 0, sizeof(range));
    ID3D12Resource_Unmap(readback, 0, &range);
    printf("copy: %u of %u words differ\n", bad, (unsigned int)WORDS);
    check(!bad, "READBACK equals UPLOAD word for word");

done:
    if (event)
        CloseHandle(event);
    if (fence)
        ID3D12Fence_Release(fence);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocator)
        ID3D12CommandAllocator_Release(allocator);
    if (queue)
        ID3D12CommandQueue_Release(queue);
    if (readback)
        ID3D12Resource_Release(readback);
    if (gpu)
        ID3D12Resource_Release(gpu);
    if (upload)
        ID3D12Resource_Release(upload);
}

/* A DXIL compute shader writes a raw UAV reached through a shader-visible descriptor table: root signature
 * (table + root constant), compute PSO, descriptor at index k written through start + k * increment, dispatch,
 * copy to READBACK, word-exact compare. The handle values are printed: the shell will hand exactly these to the
 * runtime (1:1 descriptor handles). */
static void compute_dispatch(ID3D12Device *device)
{
    enum { WORDS = 16384, SLOT = 3, SEED = 0x5eedu };
    const UINT64 size = WORDS * sizeof(UINT32);
    ID3D12Resource *uav_buffer = NULL, *readback = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start, cpu_slot;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start, gpu_slot;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *pso = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    ID3D12CommandQueue *queue = NULL;
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_RESOURCE_DESC desc;
    ID3D12Fence *fence = NULL;
    D3D12_RANGE range;
    HANDLE event = NULL;
    UINT increment;
    UINT32 *words;
    unsigned int i, bad = 0;

    /* Root signature { table { u0 }, one root constant at b0 }, from the shader's RTS0 part. */
    check(SUCCEEDED(ID3D12Device_CreateRootSignature(device, 0, g_engine_test_cs, sizeof(g_engine_test_cs),
            &IID_ID3D12RootSignature, (void **)&rs)), "root signature from the DXIL container (UAV table + constant)");
    if (!rs)
        goto done;

    memset(&pso_desc, 0, sizeof(pso_desc));
    pso_desc.pRootSignature = rs;
    pso_desc.CS.pShaderBytecode = g_engine_test_cs;
    pso_desc.CS.BytecodeLength = sizeof(g_engine_test_cs);
    check(SUCCEEDED(ID3D12Device_CreateComputePipelineState(device, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso)),
            "compute PSO from DXIL cs_6_0");
    if (!pso)
        goto done;

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&uav_buffer)), "UAV buffer");
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!uav_buffer || !readback)
        goto done;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 8;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap)),
            "shader-visible CBV_SRV_UAV heap (8)");
    if (!heap)
        goto done;
    increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap, &cpu_start);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap, &gpu_start);
    cpu_slot.ptr = cpu_start.ptr + (SIZE_T)SLOT * increment;
    gpu_slot.ptr = gpu_start.ptr + (UINT64)SLOT * increment;
    printf("descriptors: increment %u cpu start %llx gpu start %llx slot %u cpu %llx gpu %llx\n", increment,
            (unsigned long long)cpu_start.ptr, (unsigned long long)gpu_start.ptr, (unsigned int)SLOT,
            (unsigned long long)cpu_slot.ptr, (unsigned long long)gpu_slot.ptr);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = WORDS;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, uav_buffer, NULL, &uav_desc, cpu_slot);

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue))
            || FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
    {
        check(FALSE, "compute queue objects");
        goto done;
    }

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu_slot);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, 1, SEED, 0);
    ID3D12GraphicsCommandList_Dispatch(list, WORDS / 64, 1, 1);
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav_buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, uav_buffer, 0, size);
    check(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "record dispatch through the table at slot 3");

    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event || FAILED(ID3D12Fence_SetEventOnCompletion(fence, 1, event)) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
    {
        check(FALSE, "dispatch fence reaches 1 within 5 s");
        goto done;
    }

    range.Begin = 0;
    range.End = (SIZE_T)size;
    if (FAILED(ID3D12Resource_Map(readback, 0, &range, (void **)&words)))
    {
        check(FALSE, "Map READBACK");
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        bad += words[i] != i * 2654435761u + SEED;
    memset(&range, 0, sizeof(range));
    ID3D12Resource_Unmap(readback, 0, &range);
    printf("dispatch: %u of %u words differ\n", bad, (unsigned int)WORDS);
    check(!bad, "UAV written by the shader equals the expected pattern");

done:
    if (event)
        CloseHandle(event);
    if (fence)
        ID3D12Fence_Release(fence);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocator)
        ID3D12CommandAllocator_Release(allocator);
    if (queue)
        ID3D12CommandQueue_Release(queue);
    if (heap)
        ID3D12DescriptorHeap_Release(heap);
    if (readback)
        ID3D12Resource_Release(readback);
    if (uav_buffer)
        ID3D12Resource_Release(uav_buffer);
    if (pso)
        ID3D12PipelineState_Release(pso);
    if (rs)
        ID3D12RootSignature_Release(rs);
}

int main(int argc, char **argv)
{
    PFN_BC250_VKD3D_ENGINE_GET_FUNCS get_funcs;
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    BC250_VKD3D_ENGINE_FUNCS funcs;
    PFN_vkGetInstanceProcAddr gipa = NULL;
    ID3D12Device *device = NULL, *second = NULL;
    const char *filter = NULL, *icd = NULL;
    LARGE_INTEGER t0, t1, freq;
    UINT32 min_fl = 0xb000;
    HMODULE engine, driver;
    char full[MAX_PATH];
    LUID luid, device_luid;
    ULONG refs;
    HRESULT hr;
    int i;

    if (argc < 2)
    {
        printf("usage: bc250vkd3d_engine_test <bc250vkd3d.dll> [adapter substring] [--icd <path>] [--fl <hex>]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc)
            icd = argv[++i];
        else if (!strcmp(argv[i], "--fl") && i + 1 < argc)
            min_fl = (UINT32)strtoul(argv[++i], NULL, 16);
        else if (argv[i][0] == '-')
        {
            printf("FAIL  unknown or incomplete option %s\n", argv[i]);
            return 2;
        }
        else
            filter = argv[i];
    }

    /* The shell's loads: absolute paths, dependencies from the DLL's own directory and System32 only. */
    if (!GetFullPathNameA(argv[1], MAX_PATH, full, NULL)
            || !(engine = LoadLibraryExA(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)))
    {
        printf("FAIL  LoadLibrary %s: %lu\n", argv[1], GetLastError());
        return 1;
    }
    if (icd)
    {
        if (!GetFullPathNameA(icd, MAX_PATH, full, NULL)
                || !(driver = LoadLibraryExA(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)))
        {
            printf("FAIL  LoadLibrary %s: %lu\n", icd, GetLastError());
            return 1;
        }
        gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(driver, "vk_icdGetInstanceProcAddr");
    }
    else if ((driver = LoadLibraryW(L"vulkan-1.dll")))
    {
        gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(driver, "vkGetInstanceProcAddr");
    }
    if (!gipa)
    {
        printf("FAIL  no Vulkan entry point (%s)\n", icd ? icd : "vulkan-1.dll");
        return 1;
    }
    if (!find_adapter(filter, &luid))
    {
        printf("FAIL  no hardware DXGI adapter%s%s\n", filter ? " matching " : "", filter ? filter : "");
        return 1;
    }

    /* Export and version rules. */
    get_funcs = (PFN_BC250_VKD3D_ENGINE_GET_FUNCS)(void *)GetProcAddress(engine, BC250_VKD3D_ENGINE_GET_FUNCS_NAME);
    check(get_funcs != NULL, "export " BC250_VKD3D_ENGINE_GET_FUNCS_NAME);
    if (!get_funcs)
        return 1;
    memset(&funcs, 0, sizeof(funcs));
    funcs.Size = sizeof(funcs);
    check(get_funcs(BC250_VKD3D_ENGINE_ABI_VERSION + 1, &funcs) == E_NOINTERFACE, "later minor -> E_NOINTERFACE");
    check(get_funcs(0x00020000u, &funcs) == E_NOINTERFACE, "other major -> E_NOINTERFACE");
    funcs.Size = sizeof(funcs) - 1;
    check(get_funcs(BC250_VKD3D_ENGINE_ABI_VERSION, &funcs) == E_INVALIDARG, "short Size -> E_INVALIDARG");
    funcs.Size = sizeof(funcs);
    check(get_funcs(BC250_VKD3D_ENGINE_ABI_VERSION, &funcs) == S_OK && funcs.CreateDevice
            && funcs.AbiVersion == BC250_VKD3D_ENGINE_ABI_VERSION, "GetFuncs(1.0) fills the table");
    if (!funcs.CreateDevice)
        return 1;

    /* V2: an unknown LUID creates nothing. */
    memset(&info, 0, sizeof(info));
    info.Size = sizeof(info);
    info.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    info.GetInstanceProcAddr = gipa;
    info.MinimumFeatureLevel = min_fl;
    info.AdapterLuid.LowPart = 0xffffffffu;
    info.AdapterLuid.HighPart = 0x7fffffff;
    device = (ID3D12Device *)(void *)1;
    check(funcs.CreateDevice(&info, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
            "unknown LUID -> E_INVALIDARG, no device");

    info.AdapterLuid = luid;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = funcs.CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    QueryPerformanceCounter(&t1);
    printf("CreateDevice: hr %08lx in %.1f ms\n", (unsigned long)hr,
            1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    check(SUCCEEDED(hr) && device, "CreateDevice on the adapter's LUID");
    print_icd_modules();
    if (FAILED(hr) || !device)
        return 1;

    /* In C on Windows the SDK declares the struct return as a hidden out parameter. */
    ID3D12Device_GetAdapterLuid(device, &device_luid);
    check(!memcmp(&device_luid, &luid, sizeof(luid)), "device GetAdapterLuid equals the requested LUID");
    check(SUCCEEDED(funcs.CreateDevice(&info, &IID_ID3D12Device, (void **)&second)) && second && second != device,
            "a second CreateDevice returns a second device (independent)");
    if (second)
        check(ID3D12Device_Release(second) == 0, "second device final Release returns 0");

    print_caps(device);
    copy_round_trip(device);
    compute_dispatch(device);

    refs = ID3D12Device_Release(device);
    printf("device final Release: %lu\n", refs);
    check(refs == 0, "device final Release returns 0");

    printf(failures ? "FAILED (%u)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
