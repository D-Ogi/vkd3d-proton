/* SPDX-License-Identifier: MIT
 *
 * amdgpu_wddm_vkd3d_engine_test: checks amdgpu_wddm_vkd3d.dll (bc250_vkd3d_engine.h) on its own, the way the
 * D3D12 shell will use it, without the Microsoft runtime.
 *
 * Both runs:
 *   - the export and its version rules (E_NOINTERFACE, E_INVALIDARG); a 1.0-sized function table, whose 1.1 tail
 *     the engine must leave alone;
 *   - CreateInfo checks that create nothing: sizes, queue modes, missing services.
 * Default run, ABI 1.0 and the THREADED queue mode (V3), as a 1.0 shell uses it:
 *   - CreateDevice with a 1.0-sized CreateInfo over a caller-supplied Vulkan entry point, adapter chosen by LUID
 *     (V1, V2), an unknown LUID refused, two calls giving two devices (independent);
 *   - positive control of the thread census below: the THREADED device runs threads started in
 *     amdgpu_wddm_vkd3d.dll;
 *   - the capabilities the shell's GetCaps will mirror: feature level, the FL 12_0/12_1 tiers, shader model,
 *     ray tracing tier;
 *   - one GPU round trip: UPLOAD -> DEFAULT -> READBACK copy on a direct queue, fence wait, word-exact compare;
 *   - one compute dispatch: DXIL cs_6_0 with an embedded root signature writes a raw UAV reached through a
 *     shader-visible descriptor table at start + 3 * increment, word-exact compare; the handles are printed;
 *   - AbiVersion 1.0 with a 1.1-sized CreateInfo: INLINE refused, QueueMode 7 ignored (THREADED); 1.1 asking
 *     for THREADED: THREADED;
 *   - teardown: the device's final Release returns 0 (V4).
 * --inline, ABI 1.1 and the INLINE queue mode (V7, V8), with a logging BindQueue/UnbindQueue:
 *   - thread census (Toolhelp32 snapshot, start address from NtQueryInformationThread): no thread of the
 *     process may start inside amdgpu_wddm_vkd3d.dll, before CreateDevice, after it, after queue creation, after
 *     ExecuteCommandLists and after fence waits;
 *   - binding: the internal queue at CreateDevice (cookie NULL), every queue on the caller's thread, a BindQueue
 *     failure returned as is with nothing bound, every BindQueue balanced by UnbindQueue before the VkQueue is
 *     bound again;
 *   - queues: DIRECT A, DIRECT B and COPY C on distinct VkQueues, DIRECT queues until E_OUTOFMEMORY;
 *   - refusals: ID3D12Device::CreateCommandQueue, shared fences, a queue Wait before its Signal,
 *     SetEventOnMultipleFenceCompletion(ANY, NULL), the shader debug ring;
 *   - the copy round trip on C (event set by the GetCompletedValue poll that observes it), the compute dispatch
 *     on A (SetEventOnCompletion(v, NULL)), a cross-queue copy (A Signals F, B Waits F), all word-exact;
 *   - 100 command-allocator cycles with polling, private bytes within a quarter of what a positive control that
 *     keeps an allocator and list per cycle grows by, and allocator Reset as the only engine call after the GPU
 *     finished;
 *   - teardown: queues and device return 0 from their final Release.
 *
 * Usage: amdgpu_wddm_vkd3d_engine_test.exe <path to amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <path>]
 *        [--fl <hex>] [--inline]
 *   adapter substring  picks the DXGI adapter whose description contains it (default: first hardware adapter)
 *   --icd              loads that Vulkan driver DLL directly (entry vk_icdGetInstanceProcAddr), as the shell
 *                      loads hosted RADV, instead of the Vulkan loader (vulkan-1.dll)
 *   --fl               MinimumFeatureLevel for CreateDevice, default b000 (11_0)
 *   --inline           runs the INLINE suite instead of the THREADED one
 * Exit code 0 = all checks passed. "icd:" lines name every loaded Vulkan driver and its SHA-256.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_vkd3d_engine.h"
#include "engine_test_cs.h"

#define ABI_1_0 0x00010000u
#define ABI_1_1 0x00010001u

static unsigned int failures;

static void check(BOOL ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static void checkf(BOOL ok, const char *format, ...)
{
    char what[512];
    va_list args;

    va_start(args, format);
    vsnprintf(what, sizeof(what), format, args);
    va_end(args);
    check(ok, what);
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

/* Which Vulkan driver answered: every loaded module exporting the ICD entry point. "layer:" lines name the
 * loaded Vulkan layers (the loader's layer entry point), so a validation run shows that the layer was in. */
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
        if (GetProcAddress(modules[i], "vkNegotiateLoaderLayerInterfaceVersion")
                && GetModuleFileNameW(modules[i], path, ARRAYSIZE(path)))
            printf("layer: %ls\n", path);
        if (!GetProcAddress(modules[i], "vk_icdGetInstanceProcAddr"))
            continue;
        if (GetModuleFileNameW(modules[i], path, ARRAYSIZE(path)))
            print_sha256(path);
    }
}

/* Thread census. A thread belongs to the engine when its Win32 start address (the routine passed to
 * CreateThread, ThreadQuerySetWin32StartAddress) lies inside amdgpu_wddm_vkd3d.dll: vkd3d-proton's
 * pthread_create starts every thread at its own wrapper routine there. Threads of the Vulkan driver, the loader,
 * the layers and the Windows thread pool start elsewhere and are only counted. */
typedef LONG (WINAPI *PFN_NT_QUERY_INFORMATION_THREAD)(HANDLE thread, ULONG info_class, void *info, ULONG length,
        ULONG *ret_length);
#define THREAD_QUERY_SET_WIN32_START_ADDRESS 9u

static struct
{
    const BYTE *base;
    SIZE_T size;
    PFN_NT_QUERY_INFORMATION_THREAD query;
} census;

static BOOL census_init(HMODULE engine)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    MODULEINFO info;

    census.query = ntdll ? (PFN_NT_QUERY_INFORMATION_THREAD)(void *)GetProcAddress(ntdll, "NtQueryInformationThread")
            : NULL;
    if (!census.query || !GetModuleInformation(GetCurrentProcess(), engine, &info, sizeof(info)))
        return FALSE;
    census.base = info.lpBaseOfDll;
    census.size = info.SizeOfImage;
    printf("census: amdgpu_wddm_vkd3d.dll at %p, %llu bytes\n", (const void *)census.base,
            (unsigned long long)census.size);
    return TRUE;
}

/* Returns the number of threads started in amdgpu_wddm_vkd3d.dll; *unattributed counts threads whose start
 * address could not be read (they could be engine threads). */
static unsigned int census_count(const char *stage, unsigned int *unattributed)
{
    unsigned int total = 0, engine = 0, unknown = 0;
    DWORD pid = GetCurrentProcessId();
    const BYTE *start;
    THREADENTRY32 entry;
    HANDLE snapshot, thread;
    BOOL more;

    *unattributed = 0;
    if ((snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) == INVALID_HANDLE_VALUE)
    {
        printf("threads: %s: snapshot failed (%lu)\n", stage, GetLastError());
        *unattributed = ~0u;
        return 0;
    }
    entry.dwSize = sizeof(entry);
    for (more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid)
            continue;
        ++total;
        start = NULL;
        if (!(thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID))
                || census.query(thread, THREAD_QUERY_SET_WIN32_START_ADDRESS, (void *)&start, sizeof(start), NULL) < 0)
        {
            ++unknown;
        }
        else if (start >= census.base && start < census.base + census.size)
        {
            ++engine;
            printf("threads: %s: tid %lu starts at amdgpu_wddm_vkd3d.dll+%#llx\n", stage, entry.th32ThreadID,
                    (unsigned long long)(start - census.base));
        }
        if (thread)
            CloseHandle(thread);
    }
    CloseHandle(snapshot);
    printf("threads: %s: %u in the process, %u started in amdgpu_wddm_vkd3d.dll, %u unattributed\n", stage,
            total, engine, unknown);
    *unattributed = unknown;
    return engine;
}

static void census_expect_none(const char *stage)
{
    unsigned int unknown, engine;

    engine = census_count(stage, &unknown);
    checkf(!engine && !unknown, "threads %s: none started in amdgpu_wddm_vkd3d.dll (V7)", stage);
}

static SIZE_T private_bytes(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;

    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters)))
        return 0;
    return counters.PrivateUsage;
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

/* How a test waits for its fence. */
enum wait_mode
{
    WAIT_EVENT,         /* SetEventOnCompletion(v, event), then WaitForSingleObject: THREADED */
    WAIT_NULL_EVENT,    /* SetEventOnCompletion(v, NULL) blocks until the fence reaches v (V8) */
    WAIT_POLL,          /* SetEventOnCompletion(v, event), GetCompletedValue until v; the event is set then (V8) */
};

/* The inline queue mode's waits (V8). */
static BOOL wait_fence_inline(ID3D12Fence *fence, UINT64 value, enum wait_mode mode, const char *what)
{
    unsigned int polls = 0;
    LARGE_INTEGER t0, t1, freq;
    HANDLE event = NULL;
    BOOL done = FALSE, set;
    ULONGLONG start;
    HRESULT hr;

    if (mode == WAIT_NULL_EVENT)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        hr = ID3D12Fence_SetEventOnCompletion(fence, value, NULL);
        QueryPerformanceCounter(&t1);
        done = SUCCEEDED(hr) && ID3D12Fence_GetCompletedValue(fence) >= value;
        printf("%s: SetEventOnCompletion(%llu, NULL) hr %08lx after %.2f ms\n", what, (unsigned long long)value,
                (unsigned long)hr, 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        checkf(done, "%s: SetEventOnCompletion(%llu, NULL) returns with the fence at %llu", what,
                (unsigned long long)value, (unsigned long long)value);
        return done;
    }

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = event ? ID3D12Fence_SetEventOnCompletion(fence, value, event) : E_OUTOFMEMORY;
    checkf(SUCCEEDED(hr), "%s: SetEventOnCompletion(%llu, event)", what, (unsigned long long)value);
    start = GetTickCount64();
    while (SUCCEEDED(hr) && GetTickCount64() - start < 5000)
    {
        ++polls;
        if (ID3D12Fence_GetCompletedValue(fence) >= value)
        {
            done = TRUE;
            break;
        }
        Sleep(1);
    }
    set = done && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    printf("%s: GetCompletedValue reached %llu after %u polls, event %s\n", what, (unsigned long long)value, polls,
            set ? "set" : "not set");
    checkf(done, "%s: GetCompletedValue polls reach %llu within 5 s", what, (unsigned long long)value);
    checkf(set, "%s: the event is set once GetCompletedValue has observed %llu", what, (unsigned long long)value);
    if (event)
        CloseHandle(event);
    return done && set;
}

/* UPLOAD -> DEFAULT -> READBACK on a queue, then a fence wait and a word-exact compare. With given_queue NULL
 * the test creates a direct queue through the device (THREADED); otherwise it uses given_queue, of list type
 * type, and the inline wait mode. */
static void copy_round_trip(ID3D12Device *device, ID3D12CommandQueue *given_queue, D3D12_COMMAND_LIST_TYPE type,
        enum wait_mode mode, const char *tag)
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
    checkf(upload && gpu && readback, "%screate UPLOAD, DEFAULT and READBACK buffers (64 KiB)", tag);
    if (!upload || !gpu || !readback)
        goto done;
    printf("va: upload %llx default %llx readback %llx\n",
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(upload),
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(gpu),
            (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(readback));
    checkf(ID3D12Resource_GetGPUVirtualAddress(upload) && ID3D12Resource_GetGPUVirtualAddress(gpu)
            && ID3D12Resource_GetGPUVirtualAddress(readback), "%sGetGPUVirtualAddress nonzero for all three", tag);

    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(upload, 0, &range, (void **)&words)))
    {
        checkf(FALSE, "%sMap UPLOAD", tag);
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        words[i] = 0x9e3779b9u * (i + 1);
    ID3D12Resource_Unmap(upload, 0, NULL);

    if (given_queue)
    {
        queue = given_queue;
        ID3D12CommandQueue_AddRef(queue);
    }
    else
    {
        memset(&queue_desc, 0, sizeof(queue_desc));
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue);
    }
    checkf(queue && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, type,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, type, allocator, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)),
            "%screate %s queue, allocator, command list and fence", tag, given_queue ? "the given" : "direct");
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
    checkf(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "%srecord copy, barrier, copy", tag);

    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    checkf(SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, 1)), "%squeue Signal(fence, 1)", tag);
    if (mode == WAIT_EVENT)
    {
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        checkf(event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, 1, event)), "%sSetEventOnCompletion(1)", tag);
        wait = event ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
        checkf(wait == WAIT_OBJECT_0 && ID3D12Fence_GetCompletedValue(fence) >= 1, "%sfence reaches 1 within 5 s", tag);
        if (wait != WAIT_OBJECT_0)
            goto done;
    }
    else if (!wait_fence_inline(fence, 1, mode, "copy"))
    {
        goto done;
    }

    range.Begin = 0;
    range.End = (SIZE_T)size;
    if (FAILED(ID3D12Resource_Map(readback, 0, &range, (void **)&words)))
    {
        checkf(FALSE, "%sMap READBACK", tag);
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        bad += words[i] != 0x9e3779b9u * (i + 1);
    memset(&range, 0, sizeof(range));
    ID3D12Resource_Unmap(readback, 0, &range);
    printf("copy: %u of %u words differ\n", bad, (unsigned int)WORDS);
    checkf(!bad, "%sREADBACK equals UPLOAD word for word", tag);

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
 * runtime (1:1 descriptor handles). given_queue and mode as for copy_round_trip(); the list type is DIRECT. */
static void compute_dispatch(ID3D12Device *device, ID3D12CommandQueue *given_queue, enum wait_mode mode,
        const char *tag)
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
    checkf(SUCCEEDED(ID3D12Device_CreateRootSignature(device, 0, g_engine_test_cs, sizeof(g_engine_test_cs),
            &IID_ID3D12RootSignature, (void **)&rs)), "%sroot signature from the DXIL container (UAV table + constant)",
            tag);
    if (!rs)
        goto done;

    memset(&pso_desc, 0, sizeof(pso_desc));
    pso_desc.pRootSignature = rs;
    pso_desc.CS.pShaderBytecode = g_engine_test_cs;
    pso_desc.CS.BytecodeLength = sizeof(g_engine_test_cs);
    checkf(SUCCEEDED(ID3D12Device_CreateComputePipelineState(device, &pso_desc, &IID_ID3D12PipelineState,
            (void **)&pso)), "%scompute PSO from DXIL cs_6_0", tag);
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
    checkf(SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&uav_buffer)), "%sUAV buffer",
            tag);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!uav_buffer || !readback)
        goto done;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 8;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    checkf(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap,
            (void **)&heap)), "%sshader-visible CBV_SRV_UAV heap (8)", tag);
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

    if (given_queue)
    {
        queue = given_queue;
        ID3D12CommandQueue_AddRef(queue);
    }
    else
    {
        memset(&queue_desc, 0, sizeof(queue_desc));
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue);
    }
    if (!queue
            || FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
    {
        checkf(FALSE, "%scompute queue objects", tag);
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
    checkf(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "%srecord dispatch through the table at slot 3", tag);

    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    if (mode == WAIT_EVENT)
    {
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event || FAILED(ID3D12Fence_SetEventOnCompletion(fence, 1, event))
                || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        {
            checkf(FALSE, "%sdispatch fence reaches 1 within 5 s", tag);
            goto done;
        }
    }
    else if (!wait_fence_inline(fence, 1, mode, "dispatch"))
    {
        goto done;
    }

    range.Begin = 0;
    range.End = (SIZE_T)size;
    if (FAILED(ID3D12Resource_Map(readback, 0, &range, (void **)&words)))
    {
        checkf(FALSE, "%sMap READBACK", tag);
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        bad += words[i] != i * 2654435761u + SEED;
    memset(&range, 0, sizeof(range));
    ID3D12Resource_Unmap(readback, 0, &range);
    printf("dispatch: %u of %u words differ\n", bad, (unsigned int)WORDS);
    checkf(!bad, "%sUAV written by the shader equals the expected pattern", tag);

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

/* The logging shell of the inline suite: records every BindQueue and UnbindQueue, checks that they come on
 * the thread that makes the engine calls and that a VkQueue is never bound twice. */
#define TEST_BIND_FAILURE ((HRESULT)0x80bc2501)
#define COOKIE(x) ((void *)(ULONG_PTR)(x))

static struct test_shell
{
    CRITICAL_SECTION lock;
    DWORD thread;                   /* the only thread that makes engine calls */
    HRESULT fail_next;              /* BindQueue returns it once instead of binding */
    struct
    {
        void *cookie;
        VkQueue queue;
    } bound[64];
    unsigned int bound_count;
    unsigned int binds, unbinds, failed_binds, wrong_thread, double_binds, unknown_unbinds;
    VkQueue last_offered;
} shell;

static HRESULT APIENTRY shell_bind_queue(void *context, void *cookie, VkQueue queue)
{
    struct test_shell *s = context;
    HRESULT hr = S_OK;
    unsigned int i;

    EnterCriticalSection(&s->lock);
    s->last_offered = queue;
    if (GetCurrentThreadId() != s->thread)
        ++s->wrong_thread;
    for (i = 0; i < s->bound_count; ++i)
    {
        if (s->bound[i].queue == queue || (cookie && s->bound[i].cookie == cookie))
            ++s->double_binds;
    }
    if (!queue)
        ++s->double_binds;
    if (FAILED(s->fail_next))
    {
        hr = s->fail_next;
        s->fail_next = S_OK;
        ++s->failed_binds;
    }
    else if (s->bound_count < ARRAYSIZE(s->bound))
    {
        s->bound[s->bound_count].cookie = cookie;
        s->bound[s->bound_count].queue = queue;
        ++s->bound_count;
        ++s->binds;
    }
    else
    {
        hr = E_OUTOFMEMORY;
    }
    LeaveCriticalSection(&s->lock);
    printf("bind: cookie %p VkQueue %p thread %lu -> %08lx\n", cookie, (void *)queue, GetCurrentThreadId(),
            (unsigned long)hr);
    return hr;
}

static void APIENTRY shell_unbind_queue(void *context, void *cookie, VkQueue queue)
{
    struct test_shell *s = context;
    BOOL found = FALSE;
    unsigned int i;

    EnterCriticalSection(&s->lock);
    if (GetCurrentThreadId() != s->thread)
        ++s->wrong_thread;
    for (i = 0; i < s->bound_count; ++i)
    {
        if (s->bound[i].cookie == cookie && s->bound[i].queue == queue)
        {
            s->bound[i] = s->bound[--s->bound_count];
            found = TRUE;
            break;
        }
    }
    if (found)
        ++s->unbinds;
    else
        ++s->unknown_unbinds;
    LeaveCriticalSection(&s->lock);
    printf("unbind: cookie %p VkQueue %p thread %lu%s\n", cookie, (void *)queue, GetCurrentThreadId(),
            found ? "" : " (was not bound)");
}

static VkQueue shell_queue_of(void *cookie)
{
    VkQueue queue = NULL;
    unsigned int i;

    EnterCriticalSection(&shell.lock);
    for (i = 0; i < shell.bound_count; ++i)
    {
        if (shell.bound[i].cookie == cookie)
            queue = shell.bound[i].queue;
    }
    LeaveCriticalSection(&shell.lock);
    return queue;
}

static HRESULT create_inline_queue(const BC250_VKD3D_ENGINE_FUNCS *funcs, ID3D12Device *device,
        D3D12_COMMAND_LIST_TYPE type, void *cookie, ID3D12CommandQueue **queue)
{
    BC250_VKD3D_COMMAND_QUEUE_DESC desc;

    memset(&desc, 0, sizeof(desc));
    desc.Size = sizeof(desc);
    desc.Type = (UINT32)type;
    *queue = (ID3D12CommandQueue *)(void *)1;
    return funcs->CreateCommandQueue(device, &desc, cookie, &IID_ID3D12CommandQueue, (void **)queue);
}

/* A writes UPLOAD -> DEFAULT (16 MiB) and Signals (F, 1); B Waits (F, 1) and copies DEFAULT -> READBACK. If B
 * did not wait for A, it would copy the zeroed DEFAULT buffer, or part of it. */
static void cross_queue_copy(ID3D12Device *device, ID3D12CommandQueue *qa, ID3D12CommandQueue *qb)
{
    enum { WORDS = 4 * 1024 * 1024 };
    const UINT64 size = (UINT64)WORDS * sizeof(UINT32);
    ID3D12Resource *upload = NULL, *gpu = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator_a = NULL, *allocator_b = NULL;
    ID3D12GraphicsCommandList *list_a = NULL, *list_b = NULL;
    ID3D12Fence *f = NULL, *g = NULL, *h = NULL;
    D3D12_RANGE range;
    unsigned int i, bad = 0;
    UINT32 *words;

    upload = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    gpu = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    check(upload && gpu && readback
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator_a))
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator_b))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_a, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list_a))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_b, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list_b))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&h)),
            "cross-queue: 16 MiB buffers, two allocators and lists, fences F, G, H");
    if (!list_a || !list_b || !f || !g || !h)
        goto done;

    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(upload, 0, &range, (void **)&words)))
    {
        check(FALSE, "cross-queue: Map UPLOAD");
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        words[i] = 0x85ebca6bu * (i + 7);
    ID3D12Resource_Unmap(upload, 0, NULL);

    ID3D12GraphicsCommandList_CopyBufferRegion(list_a, gpu, 0, upload, 0, size);
    ID3D12GraphicsCommandList_CopyBufferRegion(list_b, readback, 0, gpu, 0, size);
    check(SUCCEEDED(ID3D12GraphicsCommandList_Close(list_a)) && SUCCEEDED(ID3D12GraphicsCommandList_Close(list_b)),
            "cross-queue: record A (UPLOAD -> DEFAULT) and B (DEFAULT -> READBACK)");

    /* V7: a wait before its signal would block the caller; refused, nothing queued. */
    check(ID3D12CommandQueue_Wait(qb, f, 1) == E_NOTIMPL, "queue B Wait(F, 1) before any Signal of F -> E_NOTIMPL");
    /* A value the CPU has signalled is reached: the Wait queues nothing that blocks. */
    check(SUCCEEDED(ID3D12Fence_Signal(h, 1)) && ID3D12CommandQueue_Wait(qb, h, 1) == S_OK,
            "fence H: CPU Signal(1), then queue B Wait(H, 1) -> S_OK");

    ID3D12CommandQueue_ExecuteCommandLists(qa, 1, (ID3D12CommandList **)&list_a);
    check(SUCCEEDED(ID3D12CommandQueue_Signal(qa, f, 1)), "queue A: ExecuteCommandLists, Signal(F, 1)");
    check(ID3D12CommandQueue_Wait(qb, f, 1) == S_OK, "queue B: Wait(F, 1) after A's Signal -> S_OK");
    ID3D12CommandQueue_ExecuteCommandLists(qb, 1, (ID3D12CommandList **)&list_b);
    check(SUCCEEDED(ID3D12CommandQueue_Signal(qb, g, 1)), "queue B: ExecuteCommandLists, Signal(G, 1)");
    if (!wait_fence_inline(g, 1, WAIT_NULL_EVENT, "cross-queue"))
        goto done;
    check(ID3D12Fence_GetCompletedValue(f) >= 1, "cross-queue: F is at 1 once G is");

    range.Begin = 0;
    range.End = (SIZE_T)size;
    if (FAILED(ID3D12Resource_Map(readback, 0, &range, (void **)&words)))
    {
        check(FALSE, "cross-queue: Map READBACK");
        goto done;
    }
    for (i = 0; i < WORDS; ++i)
        bad += words[i] != 0x85ebca6bu * (i + 7);
    memset(&range, 0, sizeof(range));
    ID3D12Resource_Unmap(readback, 0, &range);
    printf("cross-queue: %u of %u words differ\n", bad, (unsigned int)WORDS);
    check(!bad, "cross-queue: READBACK (queue B) equals UPLOAD (queue A) word for word");

done:
    if (h)
        ID3D12Fence_Release(h);
    if (g)
        ID3D12Fence_Release(g);
    if (f)
        ID3D12Fence_Release(f);
    if (list_b)
        ID3D12GraphicsCommandList_Release(list_b);
    if (list_a)
        ID3D12GraphicsCommandList_Release(list_a);
    if (allocator_b)
        ID3D12CommandAllocator_Release(allocator_b);
    if (allocator_a)
        ID3D12CommandAllocator_Release(allocator_a);
    if (readback)
        ID3D12Resource_Release(readback);
    if (gpu)
        ID3D12Resource_Release(gpu);
    if (upload)
        ID3D12Resource_Release(upload);
}

/* Command-allocator reuse on one queue, the way a frame loop does it: two allocators in turn, each Reset only
 * after GetCompletedValue shows its last submission done. Private bytes are sampled after a warm-up and at the
 * end. Then three serial cycles in which allocator Reset is the first engine call after the GPU finished: the
 * test learns that from the READBACK memory, not from the engine, so only Reset's own retirement (V8) can
 * release the allocator's submission; if it did not, vkd3d-proton logs an error that the run counts.
 * The memory bound comes from a positive control: as many cycles again, each keeping a new allocator and list
 * alive (what the loop would pile up if nothing were recycled). The loop may grow by a quarter of that. */
static void allocator_reuse(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    enum { CYCLES = 100, WARMUP = 10, WORDS = 1024, SERIAL = 3, CONTROL = CYCLES - WARMUP };
    ID3D12GraphicsCommandList *kept_lists[CONTROL] = {NULL};
    ID3D12CommandAllocator *kept_allocators[CONTROL] = {NULL};
    const UINT64 size = WORDS * sizeof(UINT32);
    ID3D12CommandAllocator *allocators[2] = {NULL, NULL};
    ID3D12Resource *upload = NULL, *readback = NULL;
    SIZE_T before = 0, after = 0, control_before, control_after;
    ID3D12GraphicsCommandList *list = NULL;
    long long growth, control_growth;
    unsigned int n, polls = 0, bad_resets = 0;
    volatile const UINT32 *mapped = NULL;
    UINT32 *upload_words = NULL;
    ID3D12Fence *fence = NULL;
    D3D12_RANGE range;
    ULONGLONG start;
    BOOL ok = TRUE;
    UINT32 marker;

    upload = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    check(upload && readback
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocators[0]))
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocators[1]))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12GraphicsCommandList_Close(list))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)),
            "allocator reuse: two allocators, one list, one fence");
    if (!list || !fence)
        goto done;
    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(upload, 0, &range, (void **)&upload_words))
            || FAILED(ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped)))
    {
        check(FALSE, "allocator reuse: Map UPLOAD and READBACK");
        goto done;
    }
    memset(upload_words, 0, (size_t)size);

    for (n = 1; n <= CYCLES && ok; ++n)
    {
        ID3D12CommandAllocator *allocator = allocators[n & 1];

        /* Cycle n - 2 used this allocator. */
        start = GetTickCount64();
        while (n > 2 && ID3D12Fence_GetCompletedValue(fence) < n - 2)
        {
            ++polls;
            if (GetTickCount64() - start > 5000)
            {
                ok = FALSE;
                break;
            }
            SwitchToThread();
        }
        if (!ok || FAILED(ID3D12CommandAllocator_Reset(allocator))
                || FAILED(ID3D12GraphicsCommandList_Reset(list, allocator, NULL)))
        {
            ++bad_resets;
            ok = FALSE;
            break;
        }
        ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, upload, 0, size);
        if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        {
            ok = FALSE;
            break;
        }
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        if (FAILED(ID3D12CommandQueue_Signal(queue, fence, n)))
            ok = FALSE;
        if (n == WARMUP)
            before = private_bytes();
    }
    ok = ok && wait_fence_inline(fence, CYCLES, WAIT_NULL_EVENT, "allocator reuse");
    after = private_bytes();
    growth = (long long)after - (long long)before;
    printf("allocator reuse: %u cycles, %u GetCompletedValue polls, private bytes %llu after cycle %u, %llu after "
            "cycle %u (%+lld)\n", n - 1, polls, (unsigned long long)before, (unsigned int)WARMUP,
            (unsigned long long)after, (unsigned int)CYCLES, growth);
    checkf(ok && !bad_resets, "allocator reuse: %u cycles, each allocator Reset after polling its fence value",
            (unsigned int)CYCLES);
    census_expect_none("after 100 allocator cycles");
    if (!ok)
        goto done;

    for (n = 1; n <= SERIAL && ok; ++n)
    {
        ID3D12CommandAllocator *allocator = allocators[0];

        if (FAILED(ID3D12CommandAllocator_Reset(allocator))
                || FAILED(ID3D12GraphicsCommandList_Reset(list, allocator, NULL)))
        {
            ok = FALSE;
            break;
        }
        marker = 0xc0de0000u + n;
        upload_words[0] = marker;
        ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, upload, 0, size);
        ok = SUCCEEDED(ID3D12GraphicsCommandList_Close(list));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        ok = ok && SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, CYCLES + n));
        /* No engine call from here to the next Reset: the copy's result says the GPU got there, and 20 ms
         * covers the timeline signal that follows it in the same submission. */
        start = GetTickCount64();
        while (ok && mapped[0] != marker)
        {
            if (GetTickCount64() - start > 5000)
                ok = FALSE;
            SwitchToThread();
        }
        Sleep(20);
    }
    ok = ok && SUCCEEDED(ID3D12CommandAllocator_Reset(allocators[0]));
    checkf(ok, "allocator reuse: %u serial cycles with allocator Reset as the first engine call after the GPU "
            "finished (vkd3d-proton logs an error if Reset found the submission still pending)", (unsigned int)SERIAL);
    ok = ok && wait_fence_inline(fence, CYCLES + SERIAL, WAIT_NULL_EVENT, "allocator reuse (serial)");
    if (!ok)
        goto done;

    control_before = private_bytes();
    for (n = 0; n < CONTROL && ok; ++n)
    {
        if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&kept_allocators[n]))
                || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    kept_allocators[n], NULL, &IID_ID3D12GraphicsCommandList, (void **)&kept_lists[n])))
        {
            ok = FALSE;
            break;
        }
        ID3D12GraphicsCommandList_CopyBufferRegion(kept_lists[n], readback, 0, upload, 0, size);
        ok = SUCCEEDED(ID3D12GraphicsCommandList_Close(kept_lists[n]));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&kept_lists[n]);
        ok = ok && SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, CYCLES + SERIAL + 1 + n));
    }
    ok = ok && wait_fence_inline(fence, CYCLES + SERIAL + CONTROL, WAIT_NULL_EVENT, "allocator reuse (control)");
    control_after = private_bytes();
    control_growth = (long long)control_after - (long long)control_before;
    printf("allocator reuse: control keeps %u allocators and lists: private bytes %+lld (%lld per cycle)\n",
            (unsigned int)CONTROL, control_growth, control_growth / CONTROL);
    checkf(ok && control_growth >= 1024 * 1024, "allocator reuse: positive control, %u kept allocators and lists "
            "grow private bytes by %lld KiB (at least 1024)", (unsigned int)CONTROL, control_growth / 1024);
    checkf(ok && before && growth <= control_growth / 4, "allocator reuse: cycles %u to %u grow private bytes by "
            "%+lld KiB, at most a quarter of the control's %lld KiB", (unsigned int)WARMUP, (unsigned int)CYCLES,
            growth / 1024, control_growth / 1024);

done:
    for (n = 0; n < CONTROL; ++n)
    {
        if (kept_lists[n])
            ID3D12GraphicsCommandList_Release(kept_lists[n]);
        if (kept_allocators[n])
            ID3D12CommandAllocator_Release(kept_allocators[n]);
    }
    if (mapped)
        ID3D12Resource_Unmap(readback, 0, NULL);
    if (upload_words)
        ID3D12Resource_Unmap(upload, 0, NULL);
    if (fence)
        ID3D12Fence_Release(fence);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocators[1])
        ID3D12CommandAllocator_Release(allocators[1]);
    if (allocators[0])
        ID3D12CommandAllocator_Release(allocators[0]);
    if (readback)
        ID3D12Resource_Release(readback);
    if (upload)
        ID3D12Resource_Release(upload);
}

/* SetEventOnMultipleFenceCompletion without an event: ANY is refused (V7), ALL blocks on each fence. */
static void multiple_fence_waits(ID3D12Device *device)
{
    ID3D12Fence *fences[2] = {NULL, NULL};
    ID3D12Device1 *device1 = NULL;
    const UINT64 values[2] = {1, 1};

    if (FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12Device1, (void **)&device1))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fences[0]))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fences[1])))
    {
        check(FALSE, "multiple fences: ID3D12Device1 and two fences");
        goto done;
    }
    ID3D12Fence_Signal(fences[0], 1);
    ID3D12Fence_Signal(fences[1], 1);
    check(ID3D12Device1_SetEventOnMultipleFenceCompletion(device1, fences, values, 2,
            D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, NULL) == E_NOTIMPL,
            "SetEventOnMultipleFenceCompletion(ANY, NULL) -> E_NOTIMPL (V7)");
    check(ID3D12Device1_SetEventOnMultipleFenceCompletion(device1, fences, values, 2,
            D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, NULL) == S_OK,
            "SetEventOnMultipleFenceCompletion(ALL, NULL) on reached values -> S_OK");

done:
    if (fences[1])
        ID3D12Fence_Release(fences[1]);
    if (fences[0])
        ID3D12Fence_Release(fences[0]);
    if (device1)
        ID3D12Device1_Release(device1);
}

/* V7 and V8 on one device in the inline queue mode. */
static void inline_suite(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *base)
{
    ID3D12CommandQueue *qa = NULL, *qb = NULL, *qc = NULL, *extra[32], *tmp = NULL;
    VkQueue internal = NULL, va, vb, vc, freed;
    BC250_VKD3D_SHELL_SERVICES services;
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc;
    D3D12_COMMAND_QUEUE_DESC d3d12_desc;
    LARGE_INTEGER t0, t1, freq;
    ID3D12Device *device = NULL;
    ID3D12Fence *fence = NULL;
    unsigned int n, i, zero;
    ULONG refs;
    HRESULT hr;

    InitializeCriticalSection(&shell.lock);
    shell.thread = GetCurrentThreadId();

    memset(&services, 0, sizeof(services));
    services.Size = sizeof(services);
    services.Shell = &shell;
    services.BindQueue = shell_bind_queue;
    services.UnbindQueue = shell_unbind_queue;

    info = *base;
    info.Size = sizeof(info);
    info.AbiVersion = ABI_1_1;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE;
    info.Services = &services;

    census_expect_none("before CreateDevice");

    /* V7: configurations that would start a thread are refused. */
    SetEnvironmentVariableA("VKD3D_SHADER_DEBUG_RING_SIZE_LOG2", "12");
    device = (ID3D12Device *)(void *)1;
    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    SetEnvironmentVariableA("VKD3D_SHADER_DEBUG_RING_SIZE_LOG2", NULL);
    check(hr == E_INVALIDARG && !device && !shell.binds && !shell.failed_binds,
            "INLINE with VKD3D_SHADER_DEBUG_RING_SIZE_LOG2 set -> E_INVALIDARG, nothing bound");

    /* V7: a BindQueue failure of the internal queue fails CreateDevice with its HRESULT. */
    shell.fail_next = TEST_BIND_FAILURE;
    device = (ID3D12Device *)(void *)1;
    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    checkf(hr == TEST_BIND_FAILURE && !device && shell.failed_binds == 1 && !shell.bound_count && !shell.unbinds,
            "INLINE CreateDevice whose BindQueue fails -> BindQueue's %08lx, no device, nothing bound (hr %08lx)",
            (unsigned long)TEST_BIND_FAILURE, (unsigned long)hr);
    census_expect_none("after the failed CreateDevice");

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    QueryPerformanceCounter(&t1);
    printf("CreateDevice (INLINE): hr %08lx in %.1f ms\n", (unsigned long)hr,
            1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    check(SUCCEEDED(hr) && device, "CreateDevice in the INLINE queue mode (ABI 1.1)");
    print_icd_modules();
    if (FAILED(hr) || !device)
        goto done;
    internal = shell_queue_of(NULL);
    check(shell.bound_count == 1 && internal, "CreateDevice bound one VkQueue, the internal queue (cookie NULL)");
    census_expect_none("after CreateDevice");

    /* V7 refusals. */
    memset(&d3d12_desc, 0, sizeof(d3d12_desc));
    d3d12_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(ID3D12Device_CreateCommandQueue(device, &d3d12_desc, &IID_ID3D12CommandQueue, (void **)&tmp) == E_NOTIMPL
            && !tmp, "ID3D12Device::CreateCommandQueue -> E_NOTIMPL");
    check(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&fence)
            == E_INVALIDARG && !fence, "CreateFence(SHARED) -> E_INVALIDARG");
    memset(&qdesc, 0, sizeof(qdesc));
    qdesc.Size = sizeof(qdesc);
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_BUNDLE;
    check(funcs->CreateCommandQueue(device, &qdesc, COOKIE(0xbad), &IID_ID3D12CommandQueue, (void **)&tmp)
            == E_INVALIDARG && !tmp, "CreateCommandQueue(BUNDLE) -> E_INVALIDARG");
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Size = sizeof(qdesc) - 1;
    check(funcs->CreateCommandQueue(device, &qdesc, COOKIE(0xbad), &IID_ID3D12CommandQueue, (void **)&tmp)
            == E_INVALIDARG && !tmp, "CreateCommandQueue with a short desc Size -> E_INVALIDARG");

    /* Queues: each on a VkQueue of its own, bound before CreateCommandQueue returns. */
    check(SUCCEEDED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xa0), &qa))
            && SUCCEEDED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xb0), &qb))
            && SUCCEEDED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_COPY, COOKIE(0xc0), &qc)),
            "CreateCommandQueue: DIRECT A, DIRECT B, COPY C");
    if (!qa || !qb || !qc || qa == (void *)1 || qb == (void *)1 || qc == (void *)1)
    {
        qa = qa == (void *)1 ? NULL : qa;
        qb = qb == (void *)1 ? NULL : qb;
        qc = qc == (void *)1 ? NULL : qc;
        goto done;
    }
    va = shell_queue_of(COOKIE(0xa0));
    vb = shell_queue_of(COOKIE(0xb0));
    vc = shell_queue_of(COOKIE(0xc0));
    printf("queues: internal VkQueue %p, A %p, B %p, C %p\n", (void *)internal, (void *)va, (void *)vb, (void *)vc);
    check(va && vb && vc && va != vb && va != vc && vb != vc && va != internal && vb != internal && vc != internal,
            "A, B, C and the internal queue are bound to four distinct VkQueues");
    census_expect_none("after queue creation");

    /* V7: a BindQueue failure fails CreateCommandQueue with its HRESULT and binds nothing. */
    shell.fail_next = TEST_BIND_FAILURE;
    hr = create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xd0), &tmp);
    checkf(hr == TEST_BIND_FAILURE && !tmp && !shell_queue_of(COOKIE(0xd0)),
            "CreateCommandQueue whose BindQueue fails -> BindQueue's %08lx, no queue (hr %08lx)",
            (unsigned long)TEST_BIND_FAILURE, (unsigned long)hr);

    /* V7: never shared; DIRECT queues until the graphics family has none left. */
    hr = S_OK;
    for (n = 0; n < ARRAYSIZE(extra); ++n)
    {
        if (FAILED(hr = create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0x100 + n),
                &extra[n])))
            break;
    }
    printf("exhaustion: %u more DIRECT queues, then hr %08lx\n", n, (unsigned long)hr);
    checkf(hr == E_OUTOFMEMORY && n < ARRAYSIZE(extra) && extra[n] == NULL,
            "DIRECT queues until every VkQueue of the family is in use (%u more), then E_OUTOFMEMORY", n);
    if (n)
    {
        /* The only free VkQueue is the one the final Release unbinds: the next queue must get it. */
        freed = shell_queue_of(COOKIE(0x100));
        checkf(ID3D12CommandQueue_Release(extra[0]) == 0 && !shell_queue_of(COOKIE(0x100)),
                "final Release of a queue returns 0 and unbinds it");
        hr = create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0x1ff), &extra[0]);
        checkf(SUCCEEDED(hr) && shell_queue_of(COOKIE(0x1ff)) == freed,
                "a new queue gets the VkQueue that the final Release unbound (%p)", (void *)freed);
        if (FAILED(hr))
            extra[0] = NULL;
    }
    for (i = 0, zero = 0; i < n; ++i)
    {
        if (extra[i] && ID3D12CommandQueue_Release(extra[i]) == 0)
            ++zero;
    }
    checkf(zero == n && shell.bound_count == 4, "the %u extra queues return 0 from their final Release and are "
            "unbound", n);

    /* Work on the inline queues. */
    copy_round_trip(device, qc, D3D12_COMMAND_LIST_TYPE_COPY, WAIT_POLL, "COPY queue C: ");
    compute_dispatch(device, qa, WAIT_NULL_EVENT, "DIRECT queue A: ");
    census_expect_none("after ExecuteCommandLists");
    cross_queue_copy(device, qa, qb);
    census_expect_none("after fence waits");
    allocator_reuse(device, qa);
    multiple_fence_waits(device);

    check(ID3D12CommandQueue_Release(qc) == 0 && !shell_queue_of(COOKIE(0xc0)), "queue C: final Release 0, unbound");
    check(ID3D12CommandQueue_Release(qb) == 0 && !shell_queue_of(COOKIE(0xb0)), "queue B: final Release 0, unbound");
    check(ID3D12CommandQueue_Release(qa) == 0 && !shell_queue_of(COOKIE(0xa0)), "queue A: final Release 0, unbound");
    qa = qb = qc = NULL;
    census_expect_none("after the queues' final Release");

done:
    if (qc)
        ID3D12CommandQueue_Release(qc);
    if (qb)
        ID3D12CommandQueue_Release(qb);
    if (qa)
        ID3D12CommandQueue_Release(qa);
    if (device)
    {
        refs = ID3D12Device_Release(device);
        printf("device final Release: %lu\n", refs);
        check(refs == 0, "device final Release returns 0");
    }
    printf("shell: %u binds, %u unbinds, %u failed binds, %u still bound, %u on another thread, %u double binds, "
            "%u unknown unbinds\n", shell.binds, shell.unbinds, shell.failed_binds, shell.bound_count,
            shell.wrong_thread, shell.double_binds, shell.unknown_unbinds);
    check(shell.binds == shell.unbinds && !shell.bound_count && !shell.unknown_unbinds,
            "every BindQueue has its UnbindQueue, the internal queue's at the device's final Release");
    check(!shell.wrong_thread, "BindQueue and UnbindQueue ran on the thread of the engine call");
    check(!shell.double_binds, "no VkQueue or cookie was bound twice");
    census_expect_none("after the device's final Release");
    DeleteCriticalSection(&shell.lock);
}

/* The default run: a 1.0 shell (1.0-sized CreateInfo, AbiVersion 1.0) and the THREADED queue mode (V3). */
static int threaded_suite(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *base,
        LUID luid)
{
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    ID3D12Device *device = NULL, *second = NULL, *other = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc;
    ID3D12CommandQueue *queue = NULL;
    unsigned int engine, unknown;
    LARGE_INTEGER t0, t1, freq;
    LUID device_luid;
    ULONG refs;
    HRESULT hr;

    /* A 1.0 CreateInfo, with the Size a 1.0 shell passes (sizeof, tail padding included). QueueMode lies in
     * that padding on x64, where a 1.0 shell leaves whatever was there; Services lies past the Size. The engine
     * reads neither below AbiVersion 1.1. */
    info = *base;
    info.Size = BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_0;
    info.AbiVersion = ABI_1_0;
    info.QueueMode = 0x5e5e5e5eu;
    info.Services = (const BC250_VKD3D_SHELL_SERVICES *)(ULONG_PTR)0x5e5e5e5eu;
    printf("mode: 1.0 CreateInfo (%u bytes), THREADED\n", (unsigned int)info.Size);

    /* V2: an unknown LUID creates nothing. */
    info.AdapterLuid.LowPart = 0xffffffffu;
    info.AdapterLuid.HighPart = 0x7fffffff;
    device = (ID3D12Device *)(void *)1;
    check(funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
            "unknown LUID -> E_INVALIDARG, no device");

    info.AdapterLuid = luid;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    QueryPerformanceCounter(&t1);
    printf("CreateDevice: hr %08lx in %.1f ms\n", (unsigned long)hr,
            1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    check(SUCCEEDED(hr) && device, "CreateDevice on the adapter's LUID");
    print_icd_modules();
    if (FAILED(hr) || !device)
        return 1;

    /* Positive control of the census the inline suite relies on. */
    engine = census_count("after CreateDevice (THREADED)", &unknown);
    checkf(engine > 0 && !unknown, "census positive control: the THREADED device runs %u threads started in "
            "amdgpu_wddm_vkd3d.dll", engine);

    /* In C on Windows the SDK declares the struct return as a hidden out parameter. */
    ID3D12Device_GetAdapterLuid(device, &device_luid);
    check(!memcmp(&device_luid, &luid, sizeof(luid)), "device GetAdapterLuid equals the requested LUID");
    check(SUCCEEDED(funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&second)) && second && second != device,
            "a second CreateDevice returns a second device (independent)");
    if (second)
        check(ID3D12Device_Release(second) == 0, "second device final Release returns 0");

    print_caps(device);
    copy_round_trip(device, NULL, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_EVENT, "");
    compute_dispatch(device, NULL, WAIT_EVENT, "");

    /* CreateCommandQueue is for INLINE devices only. */
    memset(&qdesc, 0, sizeof(qdesc));
    qdesc.Size = sizeof(qdesc);
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue = (ID3D12CommandQueue *)(void *)1;
    check(funcs->CreateCommandQueue(device, &qdesc, NULL, &IID_ID3D12CommandQueue, (void **)&queue) == E_INVALIDARG
            && !queue, "CreateCommandQueue on a THREADED device -> E_INVALIDARG");

    refs = ID3D12Device_Release(device);
    printf("device final Release: %lu\n", refs);
    check(refs == 0, "device final Release returns 0");

    /* INLINE is 1.1: asking for it with AbiVersion 1.0 fails and creates nothing. */
    info = *base;
    info.Size = sizeof(info);
    info.AbiVersion = ABI_1_0;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE;
    info.Services = NULL;
    other = (ID3D12Device *)(void *)1;
    check(funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&other) == E_INVALIDARG && !other,
            "AbiVersion 1.0 with a 1.1-sized CreateInfo asking for INLINE -> E_INVALIDARG, no device");

    /* Other QueueMode values count only from AbiVersion 1.1: below it, the device is THREADED. */
    info.QueueMode = 7;
    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    other = NULL;
    queue = NULL;
    check(SUCCEEDED(funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&other)) && other
            && SUCCEEDED(ID3D12Device_CreateCommandQueue(other, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue)),
            "AbiVersion 1.0 with a 1.1-sized CreateInfo and QueueMode 7: a THREADED device");
    if (queue)
        ID3D12CommandQueue_Release(queue);
    if (other)
        check(ID3D12Device_Release(other) == 0, "that device's final Release returns 0");

    /* A 1.1 shell that asks for THREADED gets V3 unchanged. */
    info.AbiVersion = ABI_1_1;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_THREADED;
    other = NULL;
    queue = NULL;
    check(SUCCEEDED(funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&other)) && other
            && SUCCEEDED(ID3D12Device_CreateCommandQueue(other, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue)),
            "AbiVersion 1.1 with QueueMode THREADED and no Services: a THREADED device");
    if (queue)
        ID3D12CommandQueue_Release(queue);
    if (other)
        check(ID3D12Device_Release(other) == 0, "that device's final Release returns 0");
    return 0;
}

int main(int argc, char **argv)
{
    PFN_BC250_VKD3D_ENGINE_GET_FUNCS get_funcs;
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    BC250_VKD3D_SHELL_SERVICES services;
    BC250_VKD3D_ENGINE_FUNCS funcs;
    PFN_vkGetInstanceProcAddr gipa = NULL;
    const char *filter = NULL, *icd = NULL;
    ID3D12Device *device = NULL;
    UINT32 min_fl = 0xb000;
    HMODULE engine, driver;
    BOOL inline_mode = FALSE, tail_kept;
    char full[MAX_PATH];
    unsigned int k;
    LUID luid;
    int i;

    if (argc < 2)
    {
        printf("usage: amdgpu_wddm_vkd3d_engine_test <amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <path>]"
                " [--fl <hex>] [--inline]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc)
            icd = argv[++i];
        else if (!strcmp(argv[i], "--fl") && i + 1 < argc)
            min_fl = (UINT32)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--inline"))
            inline_mode = TRUE;
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
    check(census_init(engine),
            "thread census: NtQueryInformationThread and the module range of amdgpu_wddm_vkd3d.dll");

    /* Export and version rules. */
    get_funcs = (PFN_BC250_VKD3D_ENGINE_GET_FUNCS)(void *)GetProcAddress(engine, BC250_VKD3D_ENGINE_GET_FUNCS_NAME);
    check(get_funcs != NULL, "export " BC250_VKD3D_ENGINE_GET_FUNCS_NAME);
    if (!get_funcs)
        return 1;
    memset(&funcs, 0, sizeof(funcs));
    funcs.Size = sizeof(funcs);
    check(get_funcs(BC250_VKD3D_ENGINE_ABI_VERSION + 1, &funcs) == E_NOINTERFACE, "later minor -> E_NOINTERFACE");
    check(get_funcs(0x00020000u, &funcs) == E_NOINTERFACE, "other major -> E_NOINTERFACE");
    funcs.Size = BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0 - 1;
    check(get_funcs(ABI_1_0, &funcs) == E_INVALIDARG, "short Size -> E_INVALIDARG");

    /* A 1.0 shell's table ends before CreateCommandQueue; the engine fills that much and no more. */
    memset(&funcs, 0x5e, sizeof(funcs));
    funcs.Size = BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0;
    check(get_funcs(ABI_1_0, &funcs) == S_OK && funcs.CreateDevice
            && funcs.AbiVersion == BC250_VKD3D_ENGINE_ABI_VERSION, "GetFuncs(1.0) fills the table");
    for (k = BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0, tail_kept = TRUE; k < sizeof(funcs); ++k)
        tail_kept &= ((const BYTE *)&funcs)[k] == 0x5e;
    checkf(funcs.Size == BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0 && tail_kept,
            "GetFuncs(1.0) with the 1.0 Size (%u) writes nothing past it", (unsigned int)BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0);
    memset(&funcs, 0, sizeof(funcs));
    funcs.Size = sizeof(funcs);
    check(get_funcs(ABI_1_1, &funcs) == S_OK && funcs.CreateDevice && funcs.CreateCommandQueue
            && funcs.AbiVersion == ABI_1_1, "GetFuncs(1.1) fills CreateDevice and CreateCommandQueue");
    if (!funcs.CreateDevice || !funcs.CreateCommandQueue)
        return 1;

    memset(&info, 0, sizeof(info));
    info.Size = sizeof(info);
    info.AbiVersion = ABI_1_1;
    info.GetInstanceProcAddr = gipa;
    info.MinimumFeatureLevel = min_fl;
    info.AdapterLuid = luid;

    /* CreateInfo checks that create nothing (V7, Sizes). */
    {
        BC250_VKD3D_DEVICE_CREATE_INFO bad = info;

        bad.Size = BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_0 - 1;
        device = (ID3D12Device *)(void *)1;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
                "CreateInfo Size below 1.0 -> E_INVALIDARG");
        bad = info;
        bad.QueueMode = 7;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
                "QueueMode 7 -> E_INVALIDARG");
        bad.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE;
        bad.Services = NULL;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
                "INLINE without Services -> E_INVALIDARG");
        memset(&services, 0, sizeof(services));
        services.Size = sizeof(services) - 1;
        services.BindQueue = shell_bind_queue;
        services.UnbindQueue = shell_unbind_queue;
        bad.Services = &services;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
                "INLINE with a short Services Size -> E_INVALIDARG");
        services.Size = sizeof(services);
        services.UnbindQueue = NULL;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device,
                "INLINE without UnbindQueue -> E_INVALIDARG");
    }

    if (inline_mode)
    {
        printf("mode: %u-byte CreateInfo, AbiVersion 1.1, INLINE\n", (unsigned int)sizeof(info));
        inline_suite(&funcs, &info);
    }
    else if (threaded_suite(&funcs, &info, luid))
    {
        return 1;
    }

    printf(failures ? "FAILED (%u)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
