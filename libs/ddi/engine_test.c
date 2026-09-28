/* SPDX-License-Identifier: MIT
 *
 * amdgpu_wddm_vkd3d_engine_test: checks amdgpu_wddm_vkd3d.dll (bc250_vkd3d_engine.h) on its own, the way the
 * D3D12 shell will use it, without the Microsoft runtime.
 *
 * Both runs:
 *   - the export and its version rules (E_NOINTERFACE, E_INVALIDARG); 1.0- and 1.1-sized function tables, whose
 *     tails the engine must leave alone; GetFuncs(1.1) leaves the 1.2 entries NULL, GetFuncs(1.2) fills them, and
 *     the suites use that table;
 *   - CreateInfo checks that create nothing: sizes, queue modes, missing services, InstanceMode 7 and PRIVATE
 *     with AbiVersion 1.1 (V12, also through QueryAdapterCaps); QueryAdapterCaps with no query array and
 *     MapHeap(NULL) fail with E_INVALIDARG.
 * Both suites, after their own devices (V12), in their queue mode, through an emulation of the shell's entry point
 * (one GetInstanceProcAddr for every device, a thread-local host selector, a dispatch per VkInstance's host):
 *   - SHARED, the default: the first device creates a VkInstance (no device of the suite, refused ones included,
 *     kept the shared one); two live devices, the first with an r3 shell's 1.1-sized CreateInfo, share it as
 *     upstream, and the emulation sees the second call the first host's entry point;
 *   - PRIVATE: two live devices created on two threads have VkInstances of their own, every entry point call of
 *     each goes to its own host, and each completes a copy round trip; QueryAdapterCaps while both live answers
 *     as the first device's CheckFeatureSupport (every byte) on a VkInstance of its own, destroyed before it
 *     returns, without calling either device's host or Services; the final Releases destroy each VkInstance
 *     through its own host;
 *   - with ENABLE_VULKAN_RENDERDOC_CAPTURE=1, PRIVATE CreateDevice and QueryAdapterCaps fail with E_INVALIDARG and
 *     leave no VkInstance; in INLINE, a PRIVATE CreateDevice that the admission refuses leaves none either.
 * Both suites, on their main device (1.2, V10, V11):
 *   - QueryAdapterCaps against ID3D12Device::CheckFeatureSupport of that device, made from the same CreateInfo:
 *     HRESULT and every byte for every feature QueryAdapterCaps answers (all DXGI formats for FORMAT_SUPPORT and
 *     FORMAT_INFO, eight formats at 1 to 16 samples); the key answers are printed;
 *   - imported memory: GetVulkanHandles; UPLOAD, DEFAULT (buffers), DEFAULT (textures) and READBACK heaps over
 *     memory the test allocates on the engine's VkDevice (CreateHeapFromMemory); a placed UPLOAD buffer at 64 KiB
 *     filled through MapHeap at the address its Map returns; a placed buffer and a placed texture round trip,
 *     word-exact through MapHeap of the READBACK heap; MapHeap on a heap of ID3D12Device::CreateHeap; the safe
 *     order before vkFreeMemory (fence wait, final Releases returning 0, vkFreeMemory).
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
 *   - teardown: the device's final Release returns 0 (V4);
 *   - admission (V7): a THREADED device, with its copy round trip, on a graphics family that the Vulkan wrapper
 *     below caps at one VkQueue.
 * --inline, ABI 1.1 and the INLINE queue mode (V7, V8), with a logging BindQueue/UnbindQueue and a Vulkan
 * wrapper in front of the entry point that counts vkResetCommandPool calls and command buffers, logs the
 * semaphores of each vkQueueSubmit2, can hold a submission and can cap the queueCount of graphics families:
 *   - thread census (Toolhelp32 snapshot, start address from NtQueryInformationThread): no thread of the
 *     process may start inside amdgpu_wddm_vkd3d.dll, before CreateDevice, after it, after queue creation, after
 *     ExecuteCommandLists and after fence waits;
 *   - admission: with the graphics family capped at 1 and 2 VkQueues, CreateDevice fails with
 *     DXGI_ERROR_UNSUPPORTED and binds nothing, and QueryAdapterCaps fails with the same HRESULT; at 3 QueryAdapterCaps
 *     succeeds, the device has two DIRECT queues at once, and a third finds none free;
 *   - QueryAdapterCaps creates no VkDevice, calls no Services and starts no thread;
 *   - binding: the internal queue at CreateDevice (cookie NULL), every queue on a thread that makes engine
 *     calls, a BindQueue failure returned as is with nothing bound, every BindQueue balanced by UnbindQueue
 *     before the VkQueue is bound again;
 *   - queues: DIRECT A, DIRECT B and COPY C on distinct VkQueues; two DIRECT queues created on two threads at
 *     once, held together inside BindQueue, then a copy on each; two threads creating and releasing 50 queues
 *     each; DIRECT queues until E_OUTOFMEMORY;
 *   - refusals: ID3D12Device::CreateCommandQueue, shared fences, a queue Wait before its Signal,
 *     SetEventOnMultipleFenceCompletion(ANY, NULL), the shader debug ring;
 *   - the copy round trip on C (event set by the GetCompletedValue poll that observes it), the compute dispatch
 *     on A (SetEventOnCompletion(v, NULL)), a cross-queue copy (A, held until B has submitted, Signals F, B
 *     Waits F; a submission on B's VkQueue waits for the timeline that A's submission signals), all word-exact;
 *   - 100 command-allocator cycles with polling in which every allocator Reset calls vkResetCommandPool, three
 *     serial cycles in which it does so as the only engine call after the GPU finished, and the allocators
 *     keeping at most a quarter of the command buffers that the same loop without allocator Reset (the negative
 *     control) keeps;
 *   - teardown: queues and device return 0 from their final Release.
 * --hang, ABI 1.1 and the INLINE queue mode: the wrapper holds one submission on a semaphore that only the test
 *   signals; SetEventOnCompletion(v, NULL) gives up after the wait budget with DXGI_ERROR_DEVICE_HUNG, the device
 *   is removed, a second wait fails at once, and everything still comes apart once the semaphore is signalled.
 *   It waits about 10 s, and vkd3d-proton logs the timeout as an error.
 *
 * Usage: amdgpu_wddm_vkd3d_engine_test.exe <path to amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <path>]
 *        [--fl <hex>] [--inline | --hang]
 *   adapter substring  picks the DXGI adapter whose description contains it (default: first hardware adapter)
 *   --icd              loads that Vulkan driver DLL directly (entry vk_icdGetInstanceProcAddr), as the shell
 *                      loads hosted RADV, instead of the Vulkan loader (vulkan-1.dll)
 *   --fl               MinimumFeatureLevel for CreateDevice, default b000 (11_0)
 *   --inline           runs the INLINE suite instead of the THREADED one
 *   --hang             runs the GPU hang suite instead of the THREADED one
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
#define ABI_1_2 0x00010002u

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

/* Vulkan entry-point wrapper of the inline suites. The engine gets its entry points through
 * CreateInfo.GetInstanceProcAddr (V1), so the test puts this wrapper there, in front of the driver's (or the
 * loader's) entry point, to see what the engine asks the GPU to do rather than only what comes out: it counts
 * vkResetCommandPool calls and command buffers allocated minus freed, and logs the semaphores that every
 * vkQueueSubmit2 waits for and signals. On request it holds one submission: it adds a wait for a semaphore that
 * only the test signals (hold_arm), and it caps the queueCount that graphics families report (graphics_queue_cap).
 * Every other name goes to the driver unchanged. The real entry points are those of the last device created, so
 * one engine device at a time. */
#define VKW_LOG_SIZE 256u
#define VKW_MAX_SEMAPHORES 8u

struct vkw_submit
{
    VkQueue queue;
    unsigned int wait_count, signal_count;
    VkSemaphore waits[VKW_MAX_SEMAPHORES], signals[VKW_MAX_SEMAPHORES];
    UINT64 wait_values[VKW_MAX_SEMAPHORES], signal_values[VKW_MAX_SEMAPHORES];
};

static struct
{
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDevice create_device;
    PFN_vkResetCommandPool reset_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkFreeCommandBuffers free_command_buffers;
    PFN_vkQueueSubmit2 queue_submit2;
    VkDevice device;                    /* the last device created through the wrapper */
    volatile LONG pool_resets;          /* vkResetCommandPool calls */
    volatile LONG command_buffers;      /* allocated minus freed (vkDestroyCommandPool frees without a count) */
    CRITICAL_SECTION lock;
    unsigned int submits;               /* vkQueueSubmit2 calls; log[n % VKW_LOG_SIZE] is call n */
    struct vkw_submit log[VKW_LOG_SIZE];
    VkQueue hold_queue;                 /* the next submission on it also waits for (hold_semaphore, 1) */
    VkSemaphore hold_semaphore;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_family_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queue_family_properties2;
    volatile LONG graphics_queue_cap;   /* nonzero: graphics families report at most this queueCount */
    volatile LONG device_creates;       /* successful vkCreateDevice calls */
} vkw;

static VKAPI_ATTR VkResult VKAPI_CALL vkw_ResetCommandPool(VkDevice device, VkCommandPool pool,
        VkCommandPoolResetFlags flags)
{
    InterlockedIncrement(&vkw.pool_resets);
    return vkw.reset_command_pool(device, pool, flags);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkw_AllocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo *info, VkCommandBuffer *buffers)
{
    VkResult vr = vkw.allocate_command_buffers(device, info, buffers);

    if (vr == VK_SUCCESS)
        InterlockedAdd(&vkw.command_buffers, (LONG)info->commandBufferCount);
    return vr;
}

static VKAPI_ATTR void VKAPI_CALL vkw_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
        const VkCommandBuffer *buffers)
{
    LONG freed = 0;
    uint32_t i;

    for (i = 0; i < count; ++i)
        freed += buffers[i] != NULL;
    vkw.free_command_buffers(device, pool, count, buffers);
    InterlockedAdd(&vkw.command_buffers, -freed);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkw_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
        VkFence fence)
{
    VkSemaphoreSubmitInfo *waits = NULL;
    VkSubmitInfo2 *copy = NULL;
    struct vkw_submit *entry;
    VkResult vr;
    uint32_t i, j;

    EnterCriticalSection(&vkw.lock);
    entry = &vkw.log[vkw.submits++ % VKW_LOG_SIZE];
    memset(entry, 0, sizeof(*entry));
    entry->queue = queue;
    for (i = 0; i < count; ++i)
    {
        for (j = 0; j < submits[i].waitSemaphoreInfoCount && entry->wait_count < VKW_MAX_SEMAPHORES; ++j)
        {
            entry->waits[entry->wait_count] = submits[i].pWaitSemaphoreInfos[j].semaphore;
            entry->wait_values[entry->wait_count++] = submits[i].pWaitSemaphoreInfos[j].value;
        }
        for (j = 0; j < submits[i].signalSemaphoreInfoCount && entry->signal_count < VKW_MAX_SEMAPHORES; ++j)
        {
            entry->signals[entry->signal_count] = submits[i].pSignalSemaphoreInfos[j].semaphore;
            entry->signal_values[entry->signal_count++] = submits[i].pSignalSemaphoreInfos[j].value;
        }
    }
    if (count && vkw.hold_queue && queue == vkw.hold_queue
            && (copy = malloc(count * sizeof(*copy)))
            && (waits = malloc((submits[0].waitSemaphoreInfoCount + 1) * sizeof(*waits))))
    {
        memcpy(copy, submits, count * sizeof(*copy));
        if (submits[0].waitSemaphoreInfoCount)
            memcpy(waits, submits[0].pWaitSemaphoreInfos, submits[0].waitSemaphoreInfoCount * sizeof(*waits));
        memset(&waits[submits[0].waitSemaphoreInfoCount], 0, sizeof(*waits));
        waits[submits[0].waitSemaphoreInfoCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[submits[0].waitSemaphoreInfoCount].semaphore = vkw.hold_semaphore;
        waits[submits[0].waitSemaphoreInfoCount].value = 1;
        waits[submits[0].waitSemaphoreInfoCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        copy[0].waitSemaphoreInfoCount = submits[0].waitSemaphoreInfoCount + 1;
        copy[0].pWaitSemaphoreInfos = waits;
        submits = copy;
        vkw.hold_queue = NULL;
        printf("hold: the submission on VkQueue %p also waits for semaphore %p, value 1\n", (void *)queue,
                (void *)vkw.hold_semaphore);
    }
    LeaveCriticalSection(&vkw.lock);

    vr = vkw.queue_submit2(queue, count, submits, fence);
    free(waits);
    free(copy);
    return vr;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkw_CreateDevice(VkPhysicalDevice physical_device,
        const VkDeviceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkResult vr = vkw.create_device(physical_device, create_info, allocator, device);

    if (vr == VK_SUCCESS)
    {
        vkw.device = *device;
        InterlockedIncrement(&vkw.device_creates);
    }
    return vr;
}

/* The limited physical-device answer of the admission tests (V7): with graphics_queue_cap set, every queue family
 * with VK_QUEUE_GRAPHICS_BIT reports at most that many queues. */
static void vkw_cap_family(VkQueueFamilyProperties *properties)
{
    LONG cap = vkw.graphics_queue_cap;

    if (cap && (properties->queueFlags & VK_QUEUE_GRAPHICS_BIT) && properties->queueCount > (uint32_t)cap)
        properties->queueCount = (uint32_t)cap;
}

static VKAPI_ATTR void VKAPI_CALL vkw_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physical_device,
        uint32_t *count, VkQueueFamilyProperties *properties)
{
    uint32_t i;

    vkw.queue_family_properties(physical_device, count, properties);
    for (i = 0; properties && i < *count; ++i)
        vkw_cap_family(&properties[i]);
}

static VKAPI_ATTR void VKAPI_CALL vkw_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physical_device,
        uint32_t *count, VkQueueFamilyProperties2 *properties)
{
    uint32_t i;

    vkw.queue_family_properties2(physical_device, count, properties);
    for (i = 0; properties && i < *count; ++i)
        vkw_cap_family(&properties[i].queueFamilyProperties);
}

static PFN_vkVoidFunction vkw_wrap(const char *name, PFN_vkVoidFunction real)
{
    if (!real)
        return NULL;
    if (!strcmp(name, "vkResetCommandPool"))
    {
        vkw.reset_command_pool = (PFN_vkResetCommandPool)real;
        return (PFN_vkVoidFunction)vkw_ResetCommandPool;
    }
    if (!strcmp(name, "vkAllocateCommandBuffers"))
    {
        vkw.allocate_command_buffers = (PFN_vkAllocateCommandBuffers)real;
        return (PFN_vkVoidFunction)vkw_AllocateCommandBuffers;
    }
    if (!strcmp(name, "vkFreeCommandBuffers"))
    {
        vkw.free_command_buffers = (PFN_vkFreeCommandBuffers)real;
        return (PFN_vkVoidFunction)vkw_FreeCommandBuffers;
    }
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR"))
    {
        vkw.queue_submit2 = (PFN_vkQueueSubmit2)real;
        return (PFN_vkVoidFunction)vkw_QueueSubmit2;
    }
    return real;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetDeviceProcAddr(VkDevice device, const char *name)
{
    return vkw_wrap(name, vkw.gdpa(device, name));
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction real = vkw.gipa(instance, name);

    if (!real)
        return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
    {
        vkw.gdpa = (PFN_vkGetDeviceProcAddr)real;
        return (PFN_vkVoidFunction)vkw_GetDeviceProcAddr;
    }
    if (!strcmp(name, "vkCreateDevice"))
    {
        vkw.create_device = (PFN_vkCreateDevice)real;
        return (PFN_vkVoidFunction)vkw_CreateDevice;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties"))
    {
        vkw.queue_family_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)real;
        return (PFN_vkVoidFunction)vkw_GetPhysicalDeviceQueueFamilyProperties;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2")
            || !strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"))
    {
        vkw.queue_family_properties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)real;
        return (PFN_vkVoidFunction)vkw_GetPhysicalDeviceQueueFamilyProperties2;
    }
    return vkw_wrap(name, real);
}

static unsigned int vkw_mark(void)
{
    unsigned int mark;

    EnterCriticalSection(&vkw.lock);
    mark = vkw.submits;
    LeaveCriticalSection(&vkw.lock);
    return mark;
}

/* The highest value that the submissions on queue from start to end (vkw_mark values) signal on semaphore; 0 if
 * none does. */
static UINT64 vkw_signalled(unsigned int start, unsigned int end, VkQueue queue, VkSemaphore semaphore)
{
    const struct vkw_submit *entry;
    UINT64 value = 0;
    unsigned int n, i;

    EnterCriticalSection(&vkw.lock);
    for (n = max(start, vkw.submits > VKW_LOG_SIZE ? vkw.submits - VKW_LOG_SIZE : 0); n < end; ++n)
    {
        entry = &vkw.log[n % VKW_LOG_SIZE];
        for (i = 0; entry->queue == queue && i < entry->signal_count; ++i)
        {
            if (entry->signals[i] == semaphore && entry->signal_values[i] > value)
                value = entry->signal_values[i];
        }
    }
    LeaveCriticalSection(&vkw.lock);
    return value;
}

/* Whether a submission on waiter since wait_mark waits for a semaphore that the submissions on signaller from
 * signal_mark to wait_mark signal, for at least the highest value they signal on it. */
static BOOL vkw_waits_for(unsigned int wait_mark, VkQueue waiter, unsigned int signal_mark, VkQueue signaller,
        VkSemaphore *semaphore, UINT64 *wait_value, UINT64 *signal_value)
{
    static struct vkw_submit waits[VKW_LOG_SIZE];
    const struct vkw_submit *entry;
    unsigned int n, i, count = 0;
    UINT64 signalled;

    EnterCriticalSection(&vkw.lock);
    for (n = max(wait_mark, vkw.submits > VKW_LOG_SIZE ? vkw.submits - VKW_LOG_SIZE : 0); n < vkw.submits; ++n)
    {
        entry = &vkw.log[n % VKW_LOG_SIZE];
        if (entry->queue == waiter)
            waits[count++] = *entry;
    }
    LeaveCriticalSection(&vkw.lock);

    for (n = 0; n < count; ++n)
    {
        for (i = 0; i < waits[n].wait_count; ++i)
        {
            if ((signalled = vkw_signalled(signal_mark, wait_mark, signaller, waits[n].waits[i]))
                    && waits[n].wait_values[i] >= signalled)
            {
                *semaphore = waits[n].waits[i];
                *wait_value = waits[n].wait_values[i];
                *signal_value = signalled;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* The semaphore that holds a submission (vkw.hold_queue), made and signalled through the device's own entry
 * points, past the wrapper. It goes from 0 to 1 once: one hold per run. */
static struct
{
    PFN_vkSignalSemaphore signal;
    PFN_vkDestroySemaphore destroy;
    PFN_vkDeviceWaitIdle wait_idle;
    VkDevice device;
    VkSemaphore semaphore;
    volatile LONG released;
    HANDLE stop;                        /* ends the --hang watchdog */
} hold;

static BOOL hold_create(void)
{
    PFN_vkCreateSemaphore create;
    VkSemaphoreTypeCreateInfo type_info;
    VkSemaphoreCreateInfo info;

    memset(&hold, 0, sizeof(hold));
    if (!vkw.gdpa || !vkw.device)
        return FALSE;
    hold.device = vkw.device;
    create = (PFN_vkCreateSemaphore)vkw.gdpa(vkw.device, "vkCreateSemaphore");
    hold.signal = (PFN_vkSignalSemaphore)vkw.gdpa(vkw.device, "vkSignalSemaphore");
    hold.destroy = (PFN_vkDestroySemaphore)vkw.gdpa(vkw.device, "vkDestroySemaphore");
    hold.wait_idle = (PFN_vkDeviceWaitIdle)vkw.gdpa(vkw.device, "vkDeviceWaitIdle");
    if (!create || !hold.signal || !hold.destroy || !hold.wait_idle)
        return FALSE;
    memset(&type_info, 0, sizeof(type_info));
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type_info;
    if (create(vkw.device, &info, NULL, &hold.semaphore) != VK_SUCCESS)
        hold.semaphore = VK_NULL_HANDLE;
    return hold.semaphore != VK_NULL_HANDLE;
}

static void hold_arm(VkQueue queue)
{
    EnterCriticalSection(&vkw.lock);
    vkw.hold_semaphore = hold.semaphore;
    vkw.hold_queue = queue;
    LeaveCriticalSection(&vkw.lock);
}

/* Whether a submission took the hold since hold_arm(). */
static BOOL hold_disarm(void)
{
    BOOL taken;

    EnterCriticalSection(&vkw.lock);
    taken = !vkw.hold_queue;
    vkw.hold_queue = NULL;
    LeaveCriticalSection(&vkw.lock);
    return taken;
}

static void hold_release(const char *who)
{
    VkSemaphoreSignalInfo info;
    VkResult vr;

    if (!hold.semaphore || InterlockedCompareExchange(&hold.released, 1, 0))
        return;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = hold.semaphore;
    info.value = 1;
    vr = hold.signal(hold.device, &info);
    printf("hold: %s signals semaphore %p to 1, vr %d\n", who, (void *)hold.semaphore, (int)vr);
}

/* Releases the hold if nobody did, lets the device go idle and destroys the semaphore. No engine call may run
 * meanwhile (vkDeviceWaitIdle). */
static void hold_destroy(void)
{
    if (!hold.semaphore)
        return;
    hold_release("teardown");
    hold.wait_idle(hold.device);
    hold.destroy(hold.device, hold.semaphore, NULL);
    hold.semaphore = VK_NULL_HANDLE;
}

/* The shell's entry point as the native D3D12 shell has it (V12): one static GetInstanceProcAddr for every engine
 * device, and a thread-local selector naming the device ("host") whose engine call runs on the thread. Hosted RADV
 * binds the selected host's runtime to a VkInstance when vkCreateInstance creates it, and the entry points it
 * returns for that VkInstance are that host's dispatch. This emulation, in front of the driver's entry point,
 * records which host created each VkInstance and hands each host its own wrappers of the instance-level functions
 * that the engine resolves (vkEnumeratePhysicalDevices, vkCreateDevice, vkGetDeviceProcAddr, vkDestroyInstance).
 * It counts the GetInstanceProcAddr calls on each host's VkInstances and the calls into each host's dispatch, and
 * among them those made while another host was selected: they would run on another device's runtime. Hosts are
 * 1 to 3; 0 is no host selected. */
#define HOSTW_HOSTS 4u
#define HOSTW_INSTANCES 16u

static __declspec(thread) unsigned int tls_host;

struct hostw_host
{
    volatile LONG instances;        /* VkInstances created while it was selected */
    volatile LONG destroyed;        /* its VkInstances destroyed */
    volatile LONG gipa_calls;       /* GetInstanceProcAddr calls on its VkInstances */
    volatile LONG dispatch_calls;   /* calls into its dispatch */
    volatile LONG device_creates;   /* vkCreateDevice calls into its dispatch */
    volatile LONG foreign_gipa;     /* gipa_calls made while another host was selected */
    volatile LONG foreign_dispatch; /* dispatch_calls made while another host was selected */
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkDestroyInstance destroy_instance;
};

static struct
{
    PFN_vkGetInstanceProcAddr next;
    PFN_vkCreateInstance create_instance;
    CRITICAL_SECTION lock;
    struct
    {
        VkInstance instance;
        unsigned int host;
    } bound[HOSTW_INSTANCES];
    unsigned int bound_count;
    volatile LONG unselected;       /* entry point calls with no host selected */
    volatile LONG unbound;          /* GetInstanceProcAddr calls on a VkInstance that no host created */
    struct hostw_host host[HOSTW_HOSTS];
} hostw;

static unsigned int hostw_owner(VkInstance instance)
{
    unsigned int i, host = 0;

    EnterCriticalSection(&hostw.lock);
    for (i = 0; i < hostw.bound_count; ++i)
    {
        if (hostw.bound[i].instance == instance)
            host = hostw.bound[i].host;
    }
    LeaveCriticalSection(&hostw.lock);
    return host;
}

static void hostw_enter(unsigned int host)
{
    InterlockedIncrement(&hostw.host[host].dispatch_calls);
    if (tls_host != host)
        InterlockedIncrement(&hostw.host[host].foreign_dispatch);
}

static void hostw_unbind(VkInstance instance)
{
    unsigned int i;

    EnterCriticalSection(&hostw.lock);
    for (i = 0; i < hostw.bound_count; ++i)
    {
        if (hostw.bound[i].instance == instance)
        {
            InterlockedIncrement(&hostw.host[hostw.bound[i].host].destroyed);
            hostw.bound[i] = hostw.bound[--hostw.bound_count];
            break;
        }
    }
    LeaveCriticalSection(&hostw.lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL hostw_CreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    unsigned int host = tls_host;
    VkResult vr;

    if (!host)
        InterlockedIncrement(&hostw.unselected);
    if ((vr = hostw.create_instance(info, allocator, instance)) != VK_SUCCESS)
        return vr;
    InterlockedIncrement(&hostw.host[host].instances);
    EnterCriticalSection(&hostw.lock);
    if (hostw.bound_count < HOSTW_INSTANCES)
    {
        hostw.bound[hostw.bound_count].instance = *instance;
        hostw.bound[hostw.bound_count++].host = host;
    }
    LeaveCriticalSection(&hostw.lock);
    return vr;
}

#define HOSTW_DISPATCH(n) \
static VKAPI_ATTR VkResult VKAPI_CALL hostw_EnumeratePhysicalDevices##n(VkInstance instance, uint32_t *count, \
        VkPhysicalDevice *devices) \
{ \
    hostw_enter(n); \
    return hostw.host[n].enumerate_physical_devices(instance, count, devices); \
} \
static VKAPI_ATTR VkResult VKAPI_CALL hostw_CreateDevice##n(VkPhysicalDevice physical_device, \
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device) \
{ \
    hostw_enter(n); \
    InterlockedIncrement(&hostw.host[n].device_creates); \
    return hostw.host[n].create_device(physical_device, info, allocator, device); \
} \
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hostw_GetDeviceProcAddr##n(VkDevice device, const char *name) \
{ \
    hostw_enter(n); \
    return hostw.host[n].get_device_proc_addr(device, name); \
} \
static VKAPI_ATTR void VKAPI_CALL hostw_DestroyInstance##n(VkInstance instance, \
        const VkAllocationCallbacks *allocator) \
{ \
    hostw_enter(n); \
    hostw_unbind(instance); \
    hostw.host[n].destroy_instance(instance, allocator); \
}

HOSTW_DISPATCH(1)
HOSTW_DISPATCH(2)
HOSTW_DISPATCH(3)

static const struct
{
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkDestroyInstance destroy_instance;
}
hostw_dispatch[HOSTW_HOSTS] =
{
    {NULL},
    {hostw_EnumeratePhysicalDevices1, hostw_CreateDevice1, hostw_GetDeviceProcAddr1, hostw_DestroyInstance1},
    {hostw_EnumeratePhysicalDevices2, hostw_CreateDevice2, hostw_GetDeviceProcAddr2, hostw_DestroyInstance2},
    {hostw_EnumeratePhysicalDevices3, hostw_CreateDevice3, hostw_GetDeviceProcAddr3, hostw_DestroyInstance3},
};

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hostw_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    unsigned int host = tls_host, owner = 0;
    struct hostw_host *h;
    PFN_vkVoidFunction real;

    if (!host)
        InterlockedIncrement(&hostw.unselected);
    if (instance)
    {
        if (!(owner = hostw_owner(instance)))
        {
            InterlockedIncrement(&hostw.unbound);
        }
        else
        {
            InterlockedIncrement(&hostw.host[owner].gipa_calls);
            if (owner != host)
                InterlockedIncrement(&hostw.host[owner].foreign_gipa);
        }
    }
    if (!(real = hostw.next(instance, name)))
        return NULL;
    if (!instance)
    {
        if (strcmp(name, "vkCreateInstance"))
            return real;
        InterlockedExchangePointer((void **)&hostw.create_instance, (void *)real);
        return (PFN_vkVoidFunction)hostw_CreateInstance;
    }
    if (!owner)
        return real;

    /* The owner's own dispatch: its wrappers, over the entry points of its VkInstance. */
    h = &hostw.host[owner];
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
    {
        InterlockedExchangePointer((void **)&h->enumerate_physical_devices, (void *)real);
        return (PFN_vkVoidFunction)hostw_dispatch[owner].enumerate_physical_devices;
    }
    if (!strcmp(name, "vkCreateDevice"))
    {
        InterlockedExchangePointer((void **)&h->create_device, (void *)real);
        return (PFN_vkVoidFunction)hostw_dispatch[owner].create_device;
    }
    if (!strcmp(name, "vkGetDeviceProcAddr"))
    {
        InterlockedExchangePointer((void **)&h->get_device_proc_addr, (void *)real);
        return (PFN_vkVoidFunction)hostw_dispatch[owner].get_device_proc_addr;
    }
    if (!strcmp(name, "vkDestroyInstance"))
    {
        InterlockedExchangePointer((void **)&h->destroy_instance, (void *)real);
        return (PFN_vkVoidFunction)hostw_dispatch[owner].destroy_instance;
    }
    return real;
}

/* Clears the counters; the bindings stay. */
static void hostw_reset(void)
{
    unsigned int i;

    hostw.unselected = hostw.unbound = 0;
    for (i = 0; i < HOSTW_HOSTS; ++i)
    {
        hostw.host[i].instances = hostw.host[i].destroyed = hostw.host[i].gipa_calls = 0;
        hostw.host[i].dispatch_calls = hostw.host[i].device_creates = 0;
        hostw.host[i].foreign_gipa = hostw.host[i].foreign_dispatch = 0;
    }
}

/* Calls on the wrong host, or with none selected, since the last reset. */
static LONG hostw_foreign(void)
{
    LONG n = hostw.unselected + hostw.unbound;
    unsigned int i;

    for (i = 0; i < HOSTW_HOSTS; ++i)
        n += hostw.host[i].foreign_gipa + hostw.host[i].foreign_dispatch;
    return n;
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

/* Executes list on queue, signals a new fence to 1 and waits for it (mode as for copy_round_trip()). */
static BOOL execute_and_wait(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list,
        enum wait_mode mode, const char *tag)
{
    ID3D12Fence *fence = NULL;
    HANDLE event;
    BOOL done;

    if (FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
    {
        checkf(FALSE, "%screate a fence", tag);
        return FALSE;
    }
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    checkf(SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, 1)), "%squeue Signal(fence, 1)", tag);
    if (mode == WAIT_EVENT)
    {
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        done = event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, 1, event))
                && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 && ID3D12Fence_GetCompletedValue(fence) >= 1;
        checkf(done, "%sfence reaches 1 within 5 s", tag);
        if (event)
            CloseHandle(event);
    }
    else
    {
        done = wait_fence_inline(fence, 1, mode, tag);
    }
    ID3D12Fence_Release(fence);
    return done;
}

/* V10: heaps over memory that the test allocates on the engine's VkDevice, as the shell will. */
#define IMPORT_HEAP_SIZE (2u << 20)
#define IMPORT_OFFSET 0x10000u              /* 64 KiB: where the second resource of a heap goes */
#define IMPORT_WORDS 16384u                 /* 64 KiB: one buffer, or a 128 x 128 R8G8B8A8_UINT texture */
#define IMPORT_EDGE 128u

struct import_ctx
{
    const BC250_VKD3D_ENGINE_FUNCS *funcs;
    ID3D12Device *device;
    VkDevice vk_device;
    VkPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
};

struct import_heap
{
    ID3D12Heap *heap;
    VkDeviceMemory memory;
};

/* A heap of IMPORT_HEAP_SIZE bytes over a VkDeviceMemory of its own, allocated with
 * VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT. The engine takes only a memory type it would pick for such a heap itself
 * (V10), so the test offers the types with the wanted properties first, then the others, and keeps the first
 * that CreateHeapFromMemory takes; a refused allocation is freed at once. */
static BOOL import_heap_create(const struct import_ctx *ctx, D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS flags,
        VkMemoryPropertyFlags wanted, struct import_heap *out, const char *what)
{
    BC250_VKD3D_IMPORTED_MEMORY imported;
    VkMemoryAllocateFlagsInfo flags_info;
    VkMemoryAllocateInfo alloc_info;
    VkMemoryPropertyFlags properties;
    D3D12_HEAP_DESC desc;
    unsigned int pass, i;

    memset(out, 0, sizeof(*out));
    memset(&flags_info, 0, sizeof(flags_info));
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    memset(&desc, 0, sizeof(desc));
    desc.SizeInBytes = IMPORT_HEAP_SIZE;
    desc.Properties.Type = type;
    desc.Flags = flags;

    for (pass = 0; pass < 2; ++pass)
    {
        for (i = 0; i < ctx->memory_properties.memoryTypeCount; ++i)
        {
            properties = ctx->memory_properties.memoryTypes[i].propertyFlags;
            if (((properties & wanted) == wanted) == !!pass)
                continue;
            memset(&alloc_info, 0, sizeof(alloc_info));
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.pNext = &flags_info;
            alloc_info.allocationSize = IMPORT_HEAP_SIZE;
            alloc_info.memoryTypeIndex = i;
            if (ctx->allocate_memory(ctx->vk_device, &alloc_info, NULL, &out->memory) != VK_SUCCESS)
                continue;

            memset(&imported, 0, sizeof(imported));
            imported.Size = sizeof(imported);
            imported.Memory = out->memory;
            imported.AllocationSize = IMPORT_HEAP_SIZE;
            imported.MemoryTypeIndex = i;
            imported.Flags = BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS;
            if (SUCCEEDED(ctx->funcs->CreateHeapFromMemory(ctx->device, &imported, &desc, &IID_ID3D12Heap,
                    (void **)&out->heap)) && out->heap)
            {
                printf("import: %s heap %p over VkDeviceMemory %p, memory type %u (property flags %#x)\n", what,
                        (void *)out->heap, (void *)out->memory, i, (unsigned int)properties);
                return TRUE;
            }
            out->heap = NULL;
            ctx->free_memory(ctx->vk_device, out->memory, NULL);
            out->memory = VK_NULL_HANDLE;
        }
    }
    printf("import: no memory type makes a %s heap\n", what);
    return FALSE;
}

static ID3D12Resource *place_resource(ID3D12Device *device, const struct import_heap *heap, UINT64 offset,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
    ID3D12Resource *resource = NULL;

    if (!heap->heap || FAILED(ID3D12Device_CreatePlacedResource(device, heap->heap, offset, desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource)))
        return NULL;
    return resource;
}

static void set_footprint(D3D12_TEXTURE_COPY_LOCATION *location, ID3D12Resource *buffer)
{
    memset(location, 0, sizeof(*location));
    location->pResource = buffer;
    location->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location->PlacedFootprint.Offset = 0;
    location->PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    location->PlacedFootprint.Footprint.Width = IMPORT_EDGE;
    location->PlacedFootprint.Footprint.Height = IMPORT_EDGE;
    location->PlacedFootprint.Footprint.Depth = 1;
    location->PlacedFootprint.Footprint.RowPitch = IMPORT_EDGE * 4;
}

/* V10 on one device: GetVulkanHandles; four heaps over memory the test allocated on the engine's VkDevice
 * (UPLOAD, DEFAULT for buffers, DEFAULT for textures, READBACK); a placed UPLOAD buffer filled through MapHeap at
 * the address its Map returns; one command list copying it to a placed DEFAULT buffer and, by footprint, to a
 * placed texture, and both back to placed READBACK buffers; a word-exact compare through MapHeap; MapHeap on a heap
 * from ID3D12Device::CreateHeap; then the safe order: fence wait, final Releases, vkFreeMemory. given_queue and
 * mode as for copy_round_trip(); the list type is DIRECT. */
static void imported_memory(const BC250_VKD3D_ENGINE_FUNCS *funcs, ID3D12Device *device,
        PFN_vkGetInstanceProcAddr gipa, ID3D12CommandQueue *given_queue, enum wait_mode mode, const char *tag)
{
    const UINT64 size = IMPORT_WORDS * sizeof(UINT32);
    ID3D12Resource *src = NULL, *gpu = NULL, *texture = NULL, *dst_buffer = NULL, *dst_texture = NULL;
    struct import_heap upload, buffers, textures, readback;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    D3D12_TEXTURE_COPY_LOCATION copy_dst, copy_src;
    D3D12_RESOURCE_DESC buffer_desc, texture_desc;
    D3D12_RESOURCE_ALLOCATION_INFO allocation;
    VkQueueFamilyProperties families[32];
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    D3D12_RESOURCE_BARRIER barriers[2];
    ID3D12CommandQueue *queue = NULL;
    VkPhysicalDevice vk_physical_device;
    PFN_vkGetDeviceProcAddr gdpa;
    ID3D12Heap *engine_heap = NULL;
    D3D12_HEAP_DESC heap_desc;
    struct import_ctx ctx;
    void *heap_address, *map_address;
    unsigned int i, bad_buffer = 0, bad_texture = 0, nonzero = 0;
    const UINT32 *words;
    UINT32 family = ~0u;
    uint32_t family_count;
    VkInstance vk_instance;
    D3D12_RANGE range;
    BOOL done = FALSE;
    HRESULT hr;

    memset(&ctx, 0, sizeof(ctx));
    memset(&upload, 0, sizeof(upload));
    memset(&buffers, 0, sizeof(buffers));
    memset(&textures, 0, sizeof(textures));
    memset(&readback, 0, sizeof(readback));

    hr = funcs->GetVulkanHandles(device, &vk_instance, &vk_physical_device, &ctx.vk_device, &family);
    printf("%sGetVulkanHandles: hr %08lx, VkInstance %p, VkPhysicalDevice %p, VkDevice %p, queue family %u\n", tag,
            (unsigned long)hr, (void *)vk_instance, (void *)vk_physical_device, (void *)ctx.vk_device, family);
    checkf(hr == S_OK && vk_instance && vk_physical_device && ctx.vk_device,
            "%sGetVulkanHandles returns the engine's VkInstance, VkPhysicalDevice and VkDevice", tag);
    checkf(funcs->GetVulkanHandles(device, &vk_instance, &vk_physical_device, NULL, &family) == E_INVALIDARG,
            "%sGetVulkanHandles with a NULL out pointer -> E_INVALIDARG", tag);
    if (hr != S_OK)
        return;
    get_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(vk_instance,
            "vkGetPhysicalDeviceQueueFamilyProperties");
    get_memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(vk_instance,
            "vkGetPhysicalDeviceMemoryProperties");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(vk_instance, "vkGetDeviceProcAddr");
    if (!get_families || !get_memory_properties || !gdpa)
    {
        checkf(FALSE, "%sVulkan entry points of the engine's instance", tag);
        return;
    }
    family_count = ARRAYSIZE(families);
    get_families(vk_physical_device, &family_count, families);
    checkf(family < family_count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT),
            "%sthe queue family of the DIRECT queues (%u) has VK_QUEUE_GRAPHICS_BIT", tag, family);

    ctx.funcs = funcs;
    ctx.device = device;
    get_memory_properties(vk_physical_device, &ctx.memory_properties);
    ctx.allocate_memory = (PFN_vkAllocateMemory)gdpa(ctx.vk_device, "vkAllocateMemory");
    ctx.free_memory = (PFN_vkFreeMemory)gdpa(ctx.vk_device, "vkFreeMemory");
    if (!ctx.allocate_memory || !ctx.free_memory)
    {
        checkf(FALSE, "%svkAllocateMemory and vkFreeMemory of the engine's VkDevice", tag);
        return;
    }

    checkf(import_heap_create(&ctx, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &upload, "UPLOAD")
            && import_heap_create(&ctx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buffers, "DEFAULT buffer")
            && import_heap_create(&ctx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &textures, "DEFAULT texture")
            && import_heap_create(&ctx, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &readback, "READBACK"),
            "%sCreateHeapFromMemory: UPLOAD, DEFAULT (buffers), DEFAULT (textures) and READBACK heaps of %u MiB over "
            "memory the test allocated", tag, IMPORT_HEAP_SIZE >> 20);
    if (!upload.heap || !buffers.heap || !textures.heap || !readback.heap)
        goto release;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = IMPORT_EDGE;
    texture_desc.Height = IMPORT_EDGE;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    /* In C on Windows the SDK declares the struct return as a hidden out parameter. */
    ID3D12Device_GetResourceAllocationInfo(device, &allocation, 0, 1, &texture_desc);
    printf("%stexture: %llu bytes, alignment %llu, placed at offset %llu\n", tag,
            (unsigned long long)allocation.SizeInBytes, (unsigned long long)allocation.Alignment,
            (unsigned long long)allocation.Alignment);

    /* Second resources at nonzero offsets: the UPLOAD buffer and the texture. */
    src = place_resource(device, &upload, IMPORT_OFFSET, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    gpu = place_resource(device, &buffers, 0, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST);
    if (allocation.Alignment && allocation.Alignment + allocation.SizeInBytes <= IMPORT_HEAP_SIZE)
        texture = place_resource(device, &textures, allocation.Alignment, &texture_desc,
                D3D12_RESOURCE_STATE_COPY_DEST);
    dst_buffer = place_resource(device, &readback, 0, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST);
    dst_texture = place_resource(device, &readback, IMPORT_OFFSET, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST);
    checkf(src && gpu && texture && dst_buffer && dst_texture, "%sCreatePlacedResource on the imported heaps: "
            "UPLOAD buffer at 64 KiB, DEFAULT buffer, texture at its alignment, two READBACK buffers", tag);
    if (!src || !gpu || !texture || !dst_buffer || !dst_texture)
        goto release;
    checkf(ID3D12Resource_GetGPUVirtualAddress(src) && ID3D12Resource_GetGPUVirtualAddress(gpu)
            && ID3D12Resource_GetGPUVirtualAddress(dst_buffer),
            "%splaced buffers on imported heaps have GPU virtual addresses", tag);

    /* MapHeap: offset 0 of the heap; the buffer placed at 64 KiB maps at that address plus 64 KiB. */
    heap_address = map_address = NULL;
    hr = funcs->MapHeap(upload.heap, &heap_address);
    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(src, 0, &range, &map_address)))
        map_address = NULL;
    printf("%sMapHeap(UPLOAD) %p, Map of the buffer at 64 KiB %p\n", tag, heap_address, map_address);
    checkf(hr == S_OK && heap_address && map_address == (BYTE *)heap_address + IMPORT_OFFSET,
            "%sMapHeap(UPLOAD): the heap's address plus 64 KiB is the address Map returns for the buffer placed "
            "there", tag);
    if (hr != S_OK || !heap_address)
        goto release;
    for (i = 0; i < IMPORT_WORDS; ++i)
        ((UINT32 *)((BYTE *)heap_address + IMPORT_OFFSET))[i] = 0x9e3779b9u * (i + 7);
    if (map_address)
        ID3D12Resource_Unmap(src, 0, NULL);
    checkf(funcs->UnmapHeap(upload.heap) == S_OK, "%sUnmapHeap(UPLOAD)", tag);

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
    checkf(queue && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list)),
            "%screate a queue, allocator and command list", tag);
    if (!queue || !allocator || !list)
        goto release;

    /* UPLOAD -> DEFAULT buffer and texture -> READBACK, all placed on imported heaps. */
    ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu, 0, src, 0, size);
    memset(&copy_dst, 0, sizeof(copy_dst));
    copy_dst.pResource = texture;
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dst.SubresourceIndex = 0;
    set_footprint(&copy_src, src);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &copy_dst, 0, 0, 0, &copy_src, NULL);
    memset(barriers, 0, sizeof(barriers));
    for (i = 0; i < ARRAYSIZE(barriers); ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = i ? texture : gpu;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, ARRAYSIZE(barriers), barriers);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, dst_buffer, 0, gpu, 0, size);
    set_footprint(&copy_dst, dst_texture);
    memset(&copy_src, 0, sizeof(copy_src));
    copy_src.pResource = texture;
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &copy_dst, 0, 0, 0, &copy_src, NULL);
    checkf(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "%srecord buffer and texture copies", tag);

    /* Safe order, step 1 (V10): the fence of the queue that used the heaps. */
    if (!(done = execute_and_wait(device, queue, list, mode, tag)))
        goto release;

    heap_address = NULL;
    hr = funcs->MapHeap(readback.heap, &heap_address);
    checkf(hr == S_OK && heap_address, "%sMapHeap(READBACK)", tag);
    if (hr == S_OK && heap_address)
    {
        words = heap_address;
        for (i = 0; i < IMPORT_WORDS; ++i)
        {
            nonzero += !!words[i];
            bad_buffer += words[i] != 0x9e3779b9u * (i + 7);
            bad_texture += words[IMPORT_WORDS + i] != 0x9e3779b9u * (i + 7);
        }
        checkf(funcs->UnmapHeap(readback.heap) == S_OK, "%sUnmapHeap(READBACK)", tag);
        printf("%sbuffer: %u of %u words differ, texture: %u differ, %u nonzero\n", tag, bad_buffer,
                IMPORT_WORDS, bad_texture, nonzero);
        checkf(!bad_buffer && nonzero, "%splaced buffer round trip UPLOAD -> DEFAULT -> READBACK: word-exact", tag);
        checkf(!bad_texture, "%splaced texture round trip UPLOAD -> texture (footprint) -> READBACK: word-exact",
                tag);
    }

    /* MapHeap works on heaps of ID3D12Device::CreateHeap too. */
    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.SizeInBytes = IMPORT_OFFSET;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    heap_address = NULL;
    checkf(SUCCEEDED(ID3D12Device_CreateHeap(device, &heap_desc, &IID_ID3D12Heap, (void **)&engine_heap))
            && funcs->MapHeap(engine_heap, &heap_address) == S_OK && heap_address
            && funcs->UnmapHeap(engine_heap) == S_OK && !ID3D12Heap_Release(engine_heap),
            "%sMapHeap and UnmapHeap on an UPLOAD heap of ID3D12Device::CreateHeap (%p)", tag, heap_address);

release:
    /* Safe order, steps 2 and 3 (V10). Without the fence wait, nothing is released: the GPU may use it. */
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocator)
        ID3D12CommandAllocator_Release(allocator);
    if (queue)
        ID3D12CommandQueue_Release(queue);
    if (list && !done)
    {
        checkf(FALSE, "%sthe imported memory stays allocated: its work did not complete", tag);
        return;
    }
    {
        ID3D12Resource *resources[] = {src, gpu, texture, dst_buffer, dst_texture};
        struct import_heap *heaps[] = {&upload, &buffers, &textures, &readback};
        unsigned int zero = 0, count = 0;

        for (i = 0; i < ARRAYSIZE(resources); ++i)
        {
            count += !!resources[i];
            if (resources[i] && !ID3D12Resource_Release(resources[i]))
                ++zero;
        }
        for (i = 0; i < ARRAYSIZE(heaps); ++i)
        {
            count += !!heaps[i]->heap;
            if (heaps[i]->heap && !ID3D12Heap_Release(heaps[i]->heap))
                ++zero;
        }
        for (i = 0; i < ARRAYSIZE(heaps); ++i)
        {
            if (heaps[i]->memory)
                ctx.free_memory(ctx.vk_device, heaps[i]->memory, NULL);
        }
        if (done)
            checkf(zero == count, "%ssafe order: after the fence wait, the %u placed resources and heaps return 0 "
                    "from their final Release, then vkFreeMemory", tag, count);
    }
}

/* V11: QueryAdapterCaps against CheckFeatureSupport of a device made from the same CreateInfo. Every feature that
 * QueryAdapterCaps answers, with the inputs of each set: the SDK's up to OPTIONS21 and 56, 57, 61, and three that
 * vkd3d-proton's IDL has beyond the SDK (54, 64, 65, with its layouts). */
#define CAPS_MAX 512u
#define CAPS_SLOT_WORDS 40u                 /* 320 bytes, above the largest (SHADER_CACHE_ABI_SUPPORT, 288) */
#define TEST_FEATURE_TIGHT_ALIGNMENT ((D3D12_FEATURE)54)
#define TEST_FEATURE_BARRIER_LAYOUT ((D3D12_FEATURE)64)
#define TEST_FEATURE_OPTIONS22 ((D3D12_FEATURE)65)

struct test_barrier_layout
{
    UINT32 CommandListType;
    UINT32 Layout;
    BOOL Supported;
};

static struct caps_table
{
    unsigned int count;
    struct
    {
        D3D12_FEATURE feature;
        UINT32 size;
        const char *name;
    } cases[CAPS_MAX];
    UINT64 data[CAPS_MAX][CAPS_SLOT_WORDS];
} caps_query, caps_device;

static unsigned int caps_add(struct caps_table *t, D3D12_FEATURE feature, const char *name, const void *input,
        UINT32 size)
{
    unsigned int i = t->count;

    if (i >= CAPS_MAX || size > sizeof(t->data[0]))
        return ~0u;
    ++t->count;
    t->cases[i].feature = feature;
    t->cases[i].size = size;
    t->cases[i].name = name;
    memset(t->data[i], 0, sizeof(t->data[i]));
    if (input)
        memcpy(t->data[i], input, size);
    return i;
}

#define CAPS_OUT(t, feature, type) caps_add(t, feature, #feature, NULL, sizeof(type))

/* The SDK's DXGI formats: 0 to DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_P208 to DXGI_FORMAT_V408 and
 * DXGI_FORMAT_A4B4G4R4_UNORM; the values in between are no formats. */
static UINT32 next_format(UINT32 format)
{
    if (format == DXGI_FORMAT_B4G4R4A4_UNORM)
        return DXGI_FORMAT_P208;
    if (format == DXGI_FORMAT_V408)
        return DXGI_FORMAT_A4B4G4R4_UNORM;
    return format + 1;
}

struct caps_index
{
    unsigned int options, levels, model, root_signature, options5;
};

static void caps_build(struct caps_table *t, struct caps_index *index)
{
    static const D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2,
    };
    static const DXGI_FORMAT msaa_formats[] =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_BC1_UNORM,
    };
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa;
    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY priority;
    D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT session;
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels;
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture1;
    D3D12_FEATURE_DATA_ARCHITECTURE architecture;
    D3D12_FEATURE_DATA_SERIALIZATION serialization;
    D3D12_FEATURE_DATA_FORMAT_INFO format_info;
    struct test_barrier_layout barrier_layout;
    unsigned int i, j;
    UINT32 format;

    t->count = 0;
    index->options = CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS, D3D12_FEATURE_DATA_D3D12_OPTIONS);
    memset(&architecture, 0, sizeof(architecture));
    caps_add(t, D3D12_FEATURE_ARCHITECTURE, "D3D12_FEATURE_ARCHITECTURE", &architecture, sizeof(architecture));
    memset(&feature_levels, 0, sizeof(feature_levels));
    feature_levels.NumFeatureLevels = ARRAYSIZE(levels);
    feature_levels.pFeatureLevelsRequested = levels;
    index->levels = caps_add(t, D3D12_FEATURE_FEATURE_LEVELS, "D3D12_FEATURE_FEATURE_LEVELS", &feature_levels,
            sizeof(feature_levels));
    for (format = 0; format <= DXGI_FORMAT_A4B4G4R4_UNORM; format = next_format(format))
    {
        memset(&format_support, 0, sizeof(format_support));
        format_support.Format = (DXGI_FORMAT)format;
        caps_add(t, D3D12_FEATURE_FORMAT_SUPPORT, "D3D12_FEATURE_FORMAT_SUPPORT", &format_support,
                sizeof(format_support));
    }
    for (i = 0; i < ARRAYSIZE(msaa_formats); ++i)
    {
        for (j = 1; j <= 16; j *= 2)
        {
            memset(&msaa, 0, sizeof(msaa));
            msaa.Format = msaa_formats[i];
            msaa.SampleCount = j;
            caps_add(t, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, "D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS", &msaa,
                    sizeof(msaa));
        }
    }
    for (format = 0; format <= DXGI_FORMAT_A4B4G4R4_UNORM; format = next_format(format))
    {
        memset(&format_info, 0, sizeof(format_info));
        format_info.Format = (DXGI_FORMAT)format;
        caps_add(t, D3D12_FEATURE_FORMAT_INFO, "D3D12_FEATURE_FORMAT_INFO", &format_info, sizeof(format_info));
    }
    CAPS_OUT(t, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
    memset(&shader_model, 0, sizeof(shader_model));
    shader_model.HighestShaderModel = D3D_HIGHEST_SHADER_MODEL;
    index->model = caps_add(t, D3D12_FEATURE_SHADER_MODEL, "D3D12_FEATURE_SHADER_MODEL", &shader_model,
            sizeof(shader_model));
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS1, D3D12_FEATURE_DATA_D3D12_OPTIONS1);
    memset(&session, 0, sizeof(session));
    caps_add(t, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, "D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT",
            &session, sizeof(session));
    memset(&root_signature, 0, sizeof(root_signature));
    root_signature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_2;
    index->root_signature = caps_add(t, D3D12_FEATURE_ROOT_SIGNATURE, "D3D12_FEATURE_ROOT_SIGNATURE",
            &root_signature, sizeof(root_signature));
    memset(&architecture1, 0, sizeof(architecture1));
    caps_add(t, D3D12_FEATURE_ARCHITECTURE1, "D3D12_FEATURE_ARCHITECTURE1", &architecture1, sizeof(architecture1));
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS2, D3D12_FEATURE_DATA_D3D12_OPTIONS2);
    CAPS_OUT(t, D3D12_FEATURE_SHADER_CACHE, D3D12_FEATURE_DATA_SHADER_CACHE);
    memset(&priority, 0, sizeof(priority));
    priority.CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    priority.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    caps_add(t, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, "D3D12_FEATURE_COMMAND_QUEUE_PRIORITY", &priority,
            sizeof(priority));
    priority.CommandListType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    priority.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    caps_add(t, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, "D3D12_FEATURE_COMMAND_QUEUE_PRIORITY", &priority,
            sizeof(priority));
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS3, D3D12_FEATURE_DATA_D3D12_OPTIONS3);
    CAPS_OUT(t, D3D12_FEATURE_EXISTING_HEAPS, D3D12_FEATURE_DATA_EXISTING_HEAPS);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS4, D3D12_FEATURE_DATA_D3D12_OPTIONS4);
    memset(&serialization, 0, sizeof(serialization));
    caps_add(t, D3D12_FEATURE_SERIALIZATION, "D3D12_FEATURE_SERIALIZATION", &serialization, sizeof(serialization));
    CAPS_OUT(t, D3D12_FEATURE_CROSS_NODE, D3D12_FEATURE_DATA_CROSS_NODE);
    index->options5 = CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS5, D3D12_FEATURE_DATA_D3D12_OPTIONS5);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS6, D3D12_FEATURE_DATA_D3D12_OPTIONS6);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS7, D3D12_FEATURE_DATA_D3D12_OPTIONS7);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS8, D3D12_FEATURE_DATA_D3D12_OPTIONS8);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS9, D3D12_FEATURE_DATA_D3D12_OPTIONS9);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS10, D3D12_FEATURE_DATA_D3D12_OPTIONS10);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS11, D3D12_FEATURE_DATA_D3D12_OPTIONS11);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS12, D3D12_FEATURE_DATA_D3D12_OPTIONS12);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS13, D3D12_FEATURE_DATA_D3D12_OPTIONS13);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS14, D3D12_FEATURE_DATA_D3D12_OPTIONS14);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS15, D3D12_FEATURE_DATA_D3D12_OPTIONS15);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS16, D3D12_FEATURE_DATA_D3D12_OPTIONS16);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS17, D3D12_FEATURE_DATA_D3D12_OPTIONS17);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS18, D3D12_FEATURE_DATA_D3D12_OPTIONS18);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS19, D3D12_FEATURE_DATA_D3D12_OPTIONS19);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS20, D3D12_FEATURE_DATA_D3D12_OPTIONS20);
    CAPS_OUT(t, D3D12_FEATURE_D3D12_OPTIONS21, D3D12_FEATURE_DATA_D3D12_OPTIONS21);
    caps_add(t, TEST_FEATURE_TIGHT_ALIGNMENT, "D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT (54)", NULL, sizeof(UINT32));
    CAPS_OUT(t, D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE, D3D12_FEATURE_DATA_APPLICATION_SPECIFIC_DRIVER_STATE);
    CAPS_OUT(t, D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED, D3D12_FEATURE_DATA_BYTECODE_BYPASS_HASH_SUPPORTED);
    CAPS_OUT(t, D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT, D3D12_FEATURE_DATA_SHADERCACHE_ABI_SUPPORT);
    memset(&barrier_layout, 0, sizeof(barrier_layout));
    barrier_layout.CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    barrier_layout.Layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    caps_add(t, TEST_FEATURE_BARRIER_LAYOUT, "D3D12_FEATURE_BARRIER_LAYOUT (64)", &barrier_layout,
            sizeof(barrier_layout));
    barrier_layout.CommandListType = D3D12_COMMAND_LIST_TYPE_COPY;
    caps_add(t, TEST_FEATURE_BARRIER_LAYOUT, "D3D12_FEATURE_BARRIER_LAYOUT (64)", &barrier_layout,
            sizeof(barrier_layout));
    caps_add(t, TEST_FEATURE_OPTIONS22, "D3D12_FEATURE_D3D12_OPTIONS22 (65)", NULL, 2 * sizeof(BOOL)
            + 2 * sizeof(UINT));
}

/* Returns whether every answer was equal. */
static BOOL adapter_caps_equality(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *info,
        ID3D12Device *device, const char *tag)
{
    static BC250_VKD3D_FEATURE_QUERY queries[CAPS_MAX];
    static HRESULT device_hr[CAPS_MAX];
    const D3D12_FEATURE_DATA_D3D12_OPTIONS5 *options5;
    const D3D12_FEATURE_DATA_D3D12_OPTIONS *options;
    unsigned int i, j, differ, first, answered = 0, all_differ = 0;
    struct caps_index index;
    HRESULT hr;

    caps_build(&caps_query, &index);
    caps_device = caps_query;
    for (i = 0; i < caps_query.count; ++i)
    {
        memset(&queries[i], 0, sizeof(queries[i]));
        queries[i].Feature = (UINT32)caps_query.cases[i].feature;
        queries[i].DataSize = caps_query.cases[i].size;
        queries[i].pData = caps_query.data[i];
        queries[i].Result = E_PENDING;
    }
    hr = funcs->QueryAdapterCaps(info, caps_query.count, queries);
    checkf(hr == S_OK, "%sQueryAdapterCaps answers %u queries (hr %08lx)", tag, caps_query.count, (unsigned long)hr);
    if (hr != S_OK)
        return FALSE;
    for (i = 0; i < caps_device.count; ++i)
    {
        device_hr[i] = ID3D12Device_CheckFeatureSupport(device, caps_device.cases[i].feature, caps_device.data[i],
                caps_device.cases[i].size);
        answered += device_hr[i] == S_OK && queries[i].Result == S_OK;
    }

    /* One check per feature: HRESULT and every byte of the data, for each of its queries. */
    for (i = 0; i < caps_query.count; i = j)
    {
        differ = 0;
        first = ~0u;
        for (j = i; j < caps_query.count && !strcmp(caps_query.cases[j].name, caps_query.cases[i].name); ++j)
        {
            if (queries[j].Result != device_hr[j]
                    || memcmp(caps_query.data[j], caps_device.data[j], caps_query.cases[j].size))
            {
                if (first == ~0u)
                    first = j;
                ++differ;
            }
        }
        if (first != ~0u)
            printf("caps: %s query %u: QueryAdapterCaps hr %08lx, CheckFeatureSupport hr %08lx\n",
                    caps_query.cases[first].name, first - i, (unsigned long)queries[first].Result,
                    (unsigned long)device_hr[first]);
        all_differ += differ;
        checkf(!differ, "%s%s: QueryAdapterCaps equals CheckFeatureSupport, HRESULT and every byte (%u of %u "
                "queries)", tag, caps_query.cases[i].name, j - i - differ, j - i);
    }
    printf("%s%u queries, %u S_OK on both, %u different\n", tag, caps_query.count, answered, all_differ);

    options = (const D3D12_FEATURE_DATA_D3D12_OPTIONS *)caps_query.data[index.options];
    options5 = (const D3D12_FEATURE_DATA_D3D12_OPTIONS5 *)caps_query.data[index.options5];
    printf("%sQueryAdapterCaps: MaxSupportedFeatureLevel %x HighestShaderModel %x RootSignature %x "
            "ResourceBindingTier %u TiledResourcesTier %u ResourceHeapTier %u RaytracingTier %u\n", tag,
            (unsigned int)((const D3D12_FEATURE_DATA_FEATURE_LEVELS *)caps_query.data[index.levels])
                    ->MaxSupportedFeatureLevel,
            (unsigned int)((const D3D12_FEATURE_DATA_SHADER_MODEL *)caps_query.data[index.model])->HighestShaderModel,
            (unsigned int)((const D3D12_FEATURE_DATA_ROOT_SIGNATURE *)caps_query.data[index.root_signature])
                    ->HighestVersion,
            (unsigned int)options->ResourceBindingTier, (unsigned int)options->TiledResourcesTier,
            (unsigned int)options->ResourceHeapTier, (unsigned int)options5->RaytracingTier);
    return !all_differ;
}

/* The logging shell of the inline suite: records every BindQueue and UnbindQueue, checks that they come on a
 * thread of the test that makes engine calls (never on a thread of the engine) and that a VkQueue is never bound
 * twice. The test's threads mark themselves in tls_engine_caller. */
#define TEST_BIND_FAILURE ((HRESULT)0x80bc2501)
#define COOKIE(x) ((void *)(ULONG_PTR)(x))

static __declspec(thread) BOOL tls_engine_caller;

static struct test_shell
{
    CRITICAL_SECTION lock;
    HRESULT fail_next;              /* BindQueue returns it once instead of binding */
    struct
    {
        void *cookie;
        VkQueue queue;
    } bound[64];
    unsigned int bound_count;
    unsigned int binds, unbinds, failed_binds, wrong_thread, double_binds, unknown_unbinds;
    VkQueue last_offered;
    /* Concurrent creation: BindQueue waits (up to 2 s) until rendezvous callers are inside it; met counts the
     * calls that saw them all. */
    volatile LONG rendezvous, arrived, met;
    /* The shells of the instance isolation test serve one host each (tls_host, V12); 0 checks nothing. */
    unsigned int host, wrong_host;
} shell;

static HRESULT APIENTRY shell_bind_queue(void *context, void *cookie, VkQueue queue)
{
    struct test_shell *s = context;
    ULONGLONG start;
    HRESULT hr = S_OK;
    unsigned int i;

    if (s->rendezvous)
    {
        start = GetTickCount64();
        InterlockedIncrement(&s->arrived);
        while (s->arrived < s->rendezvous && GetTickCount64() - start < 2000)
            SwitchToThread();
        if (s->arrived >= s->rendezvous)
            InterlockedIncrement(&s->met);
    }

    EnterCriticalSection(&s->lock);
    s->last_offered = queue;
    if (!tls_engine_caller)
        ++s->wrong_thread;
    if (s->host && s->host != tls_host)
        ++s->wrong_host;
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
    if (!tls_engine_caller)
        ++s->wrong_thread;
    if (s->host && s->host != tls_host)
        ++s->wrong_host;
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

/* A test thread that makes engine calls: with count 0 it creates one DIRECT queue and keeps it, otherwise it
 * creates and releases count DIRECT queues, cookies from cookie up. */
struct queue_thread
{
    const BC250_VKD3D_ENGINE_FUNCS *funcs;
    ID3D12Device *device;
    ULONG_PTR cookie;
    unsigned int count;
    ID3D12CommandQueue *queue;
    unsigned int created, released;
    HRESULT hr;
};

static DWORD WINAPI queue_thread_main(void *arg)
{
    struct queue_thread *t = arg;
    ID3D12CommandQueue *queue;
    unsigned int i;

    tls_engine_caller = TRUE;
    if (!t->count)
    {
        if (FAILED(t->hr = create_inline_queue(t->funcs, t->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                COOKIE(t->cookie), &t->queue)))
            t->queue = NULL;
        return 0;
    }
    for (i = 0; i < t->count; ++i)
    {
        if (FAILED(t->hr = create_inline_queue(t->funcs, t->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                COOKIE(t->cookie + i), &queue)))
            break;
        ++t->created;
        if (!ID3D12CommandQueue_Release(queue))
            ++t->released;
    }
    return 0;
}

/* Runs two queue threads to their end. A thread that is still running after 30 s may yet touch the test's
 * state, so the test stops there. */
static void run_queue_threads(struct queue_thread threads[2])
{
    HANDLE handles[2];
    DWORD count = 0, wait;
    unsigned int i;

    for (i = 0; i < 2; ++i)
    {
        if ((handles[count] = CreateThread(NULL, 0, queue_thread_main, &threads[i], 0, NULL)))
            ++count;
        else
            threads[i].hr = HRESULT_FROM_WIN32(GetLastError());
    }
    wait = count ? WaitForMultipleObjects(count, handles, TRUE, 30000) : WAIT_OBJECT_0;
    for (i = 0; i < count; ++i)
        CloseHandle(handles[i]);
    if (wait != WAIT_OBJECT_0)
    {
        check(FALSE, "queue threads end within 30 s");
        printf("FAILED (%u)\n", failures);
        ExitProcess(1);
    }
}

/* V7 lets the shell create queues on several threads at once. BindQueue holds two creations until both are
 * inside it, which is after the engine picked their VkQueues and before it adds them to its list of inline
 * queues; with the list grown and filled in two critical sections, both would find room for one more and the
 * second would write past it. A third queue then grows the list, and a copy on each of the two, waited for with
 * SetEventOnCompletion(v, NULL), completes only if retirement finds the queue in the list. Then two threads
 * create and release 50 queues each. */
static void concurrent_queue_creation(const BC250_VKD3D_ENGINE_FUNCS *funcs, ID3D12Device *device)
{
    struct queue_thread threads[2];
    ID3D12CommandQueue *qf = NULL;
    unsigned int binds, released, i;
    VkQueue vd, ve, vf;
    BOOL heap_ok;
    HRESULT hr;

    memset(threads, 0, sizeof(threads));
    for (i = 0; i < 2; ++i)
    {
        threads[i].funcs = funcs;
        threads[i].device = device;
    }
    threads[0].cookie = 0xd1;
    threads[1].cookie = 0xe1;
    shell.arrived = shell.met = 0;
    shell.rendezvous = 2;
    run_queue_threads(threads);
    shell.rendezvous = 0;
    heap_ok = HeapValidate(GetProcessHeap(), 0, NULL);
    vd = shell_queue_of(COOKIE(0xd1));
    ve = shell_queue_of(COOKIE(0xe1));
    printf("concurrent: D hr %08lx VkQueue %p, E hr %08lx VkQueue %p, %ld of 2 BindQueue calls met, process heap "
            "%s\n", (unsigned long)threads[0].hr, (void *)vd, (unsigned long)threads[1].hr, (void *)ve,
            (long)shell.met, heap_ok ? "valid" : "CORRUPT");
    check(threads[0].queue && threads[1].queue && vd && ve && vd != ve,
            "concurrent: two threads create DIRECT D and E at once, on two VkQueues");
    check(shell.met == 2, "concurrent: both creations were inside BindQueue at the same time");
    check(heap_ok, "concurrent: HeapValidate of the process heap after the overlapping creations");

    hr = create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xf1), &qf);
    if (FAILED(hr))
        qf = NULL;
    vf = shell_queue_of(COOKIE(0xf1));
    check(qf && vf && vf != vd && vf != ve, "concurrent: then DIRECT F on this thread, which grows the engine's "
            "queue list");
    if (threads[0].queue)
        copy_round_trip(device, threads[0].queue, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_NULL_EVENT,
                "concurrent queue D: ");
    if (threads[1].queue)
        copy_round_trip(device, threads[1].queue, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_NULL_EVENT,
                "concurrent queue E: ");
    released = 0;
    if (qf && !ID3D12CommandQueue_Release(qf))
        ++released;
    for (i = 0; i < 2; ++i)
    {
        if (threads[i].queue && !ID3D12CommandQueue_Release(threads[i].queue))
            ++released;
    }
    check(released == 3 && !shell_queue_of(COOKIE(0xd1)) && !shell_queue_of(COOKIE(0xe1))
            && !shell_queue_of(COOKIE(0xf1)), "concurrent: D, E and F return 0 from their final Release and are "
            "unbound");

    memset(threads, 0, sizeof(threads));
    for (i = 0; i < 2; ++i)
    {
        threads[i].funcs = funcs;
        threads[i].device = device;
        threads[i].count = 50;
        threads[i].cookie = 0x10000 * (i + 1);
    }
    binds = shell.binds;
    run_queue_threads(threads);
    heap_ok = HeapValidate(GetProcessHeap(), 0, NULL);
    printf("concurrent: the threads created %u and %u queues and released %u and %u of them to 0 (hr %08lx, "
            "%08lx), %u binds, process heap %s\n", threads[0].created, threads[1].created, threads[0].released,
            threads[1].released, (unsigned long)threads[0].hr, (unsigned long)threads[1].hr, shell.binds - binds,
            heap_ok ? "valid" : "CORRUPT");
    check(threads[0].released == 50 && threads[1].released == 50 && shell.binds - binds == 100 && heap_ok
            && shell.bound_count == 4, "concurrent: two threads create and release 50 DIRECT queues each; all bound "
            "and unbound again, the process heap valid");
}

/* A writes UPLOAD -> DEFAULT (16 MiB) and Signals (F, 1); B Waits (F, 1) and copies DEFAULT -> READBACK. The
 * Vulkan wrapper holds A's submission until B has submitted, so F cannot be reached before B's Wait (which would
 * need no GPU wait) and B cannot run in order with A by luck. Then a submission on B's VkQueue (vb) must wait for
 * the timeline that A's submission on va signals, at least up to A's value; G must stay at 0 while A is held;
 * and READBACK must equal UPLOAD, where a B that did not wait would copy the DEFAULT buffer before A wrote it. */
static void cross_queue_copy(ID3D12Device *device, ID3D12CommandQueue *qa, ID3D12CommandQueue *qb, VkQueue va,
        VkQueue vb)
{
    enum { WORDS = 4 * 1024 * 1024 };
    const UINT64 size = (UINT64)WORDS * sizeof(UINT32);
    ID3D12Resource *upload = NULL, *gpu = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator_a = NULL, *allocator_b = NULL;
    ID3D12GraphicsCommandList *list_a = NULL, *list_b = NULL;
    UINT64 wait_value = 0, signal_value = 0;
    ID3D12Fence *f = NULL, *g = NULL, *h = NULL;
    unsigned int mark_a, mark_b;
    VkSemaphore timeline = NULL;
    BOOL waits, held, ok;
    unsigned int i, bad = 0;
    D3D12_RANGE range;
    UINT64 g_held;
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

    if (!hold_create())
    {
        check(FALSE, "cross-queue: a timeline semaphore through the device's own entry points");
        goto done;
    }
    mark_a = vkw_mark();
    hold_arm(va);
    ID3D12CommandQueue_ExecuteCommandLists(qa, 1, (ID3D12CommandList **)&list_a);
    held = hold_disarm();
    check(SUCCEEDED(ID3D12CommandQueue_Signal(qa, f, 1)) && held, "queue A: ExecuteCommandLists, held by the "
            "wrapper, Signal(F, 1)");
    mark_b = vkw_mark();
    check(ID3D12CommandQueue_Wait(qb, f, 1) == S_OK, "queue B: Wait(F, 1) after A's Signal -> S_OK");
    ID3D12CommandQueue_ExecuteCommandLists(qb, 1, (ID3D12CommandList **)&list_b);
    check(SUCCEEDED(ID3D12CommandQueue_Signal(qb, g, 1)), "queue B: ExecuteCommandLists, Signal(G, 1)");
    waits = vkw_waits_for(mark_b, vb, mark_a, va, &timeline, &wait_value, &signal_value);
    printf("cross-queue: B's submissions wait for semaphore %p value %llu, which A's submission signals up to "
            "%llu\n", (void *)timeline, (unsigned long long)wait_value, (unsigned long long)signal_value);
    check(waits, "cross-queue: a vkQueueSubmit2 on B's VkQueue waits for the timeline that A's submission signals, "
            "at least up to A's value");
    Sleep(50);
    g_held = ID3D12Fence_GetCompletedValue(g);
    printf("cross-queue: 50 ms after B's Signal, with A held, G is at %llu\n", (unsigned long long)g_held);
    check(!g_held, "cross-queue: G stays at 0 while A is held");
    hold_release("cross-queue");
    ok = wait_fence_inline(g, 1, WAIT_NULL_EVENT, "cross-queue");
    hold_destroy();
    if (!ok)
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

/* Command-allocator reuse on one queue, the way a frame loop does it: allocators[n & 1] records cycle n, and a
 * cycle that reuses an allocator first polls GetCompletedValue until that allocator's previous submission (cycle
 * n - 2) is done. With reset, the cycle then Resets the allocator, and every Reset must reach vkResetCommandPool
 * (the Vulkan wrapper counts the calls). Without, the negative control, the cycle Resets only the list, on the
 * allocator as it is: nothing is recycled, and each allocator keeps a command buffer per cycle. What an
 * allocator keeps is measured in command buffers (allocated minus freed, counted by the wrapper), since their
 * memory is the Vulkan driver's and need not show in the process's private bytes (the NVIDIA driver's does not);
 * both are sampled after cycle ALLOC_WARMUP and after the last one. The fence counts the cycles from base + 1. */
enum { ALLOC_CYCLES = 100, ALLOC_WARMUP = 10, ALLOC_WORDS = 1024 };

struct allocator_loop
{
    unsigned int cycles, polls;
    unsigned int resets, missed_resets;     /* allocator Resets; those that did not call vkResetCommandPool */
    LONG pool_resets;                       /* vkResetCommandPool calls during the loop */
    LONG command_buffers;                   /* command buffers kept, end of cycle ALLOC_WARMUP to the end */
    long long growth;                       /* private bytes, end of cycle ALLOC_WARMUP to the end */
};

static void record_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *dst, ID3D12Resource *src)
{
    ID3D12GraphicsCommandList_CopyBufferRegion(list, dst, 0, src, 0, ALLOC_WORDS * sizeof(UINT32));
}

static BOOL allocator_loop(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *upload,
        ID3D12Resource *readback, ID3D12Fence *fence, UINT64 base, BOOL reset, struct allocator_loop *out)
{
    ID3D12CommandAllocator *allocators[2] = {NULL, NULL};
    ID3D12GraphicsCommandList *list = NULL;
    LONG pool_resets_start, command_buffers_start = 0, count;
    SIZE_T before = 0, after;
    unsigned int n;
    ULONGLONG start;
    BOOL ok;

    memset(out, 0, sizeof(*out));
    ok = SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocators[0]))
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocators[1]))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12GraphicsCommandList_Close(list));
    pool_resets_start = vkw.pool_resets;

    for (n = 1; n <= ALLOC_CYCLES && ok; ++n)
    {
        ID3D12CommandAllocator *allocator = allocators[n & 1];

        /* Cycle n - 2 used this allocator. */
        start = GetTickCount64();
        while (n > 2 && ID3D12Fence_GetCompletedValue(fence) < base + n - 2)
        {
            ++out->polls;
            if (GetTickCount64() - start > 5000)
            {
                ok = FALSE;
                break;
            }
            SwitchToThread();
        }
        if (ok && reset)
        {
            count = vkw.pool_resets;
            ok = SUCCEEDED(ID3D12CommandAllocator_Reset(allocator));
            ++out->resets;
            if (vkw.pool_resets == count)
                ++out->missed_resets;
        }
        if (!ok || FAILED(ID3D12GraphicsCommandList_Reset(list, allocator, NULL)))
        {
            ok = FALSE;
            break;
        }
        record_copy(list, readback, upload);
        if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        {
            ok = FALSE;
            break;
        }
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        if (FAILED(ID3D12CommandQueue_Signal(queue, fence, base + n)))
        {
            ok = FALSE;
            break;
        }
        out->cycles = n;
        if (n == ALLOC_WARMUP)
        {
            before = private_bytes();
            command_buffers_start = vkw.command_buffers;
        }
    }
    ok = ok && wait_fence_inline(fence, base + ALLOC_CYCLES, WAIT_NULL_EVENT,
            reset ? "allocator reuse" : "allocator reuse (no Reset)");
    after = private_bytes();
    out->growth = (long long)after - (long long)before;
    out->command_buffers = vkw.command_buffers - command_buffers_start;
    out->pool_resets = vkw.pool_resets - pool_resets_start;

    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocators[1])
        ID3D12CommandAllocator_Release(allocators[1]);
    if (allocators[0])
        ID3D12CommandAllocator_Release(allocators[0]);
    return ok && before;
}

/* The frame loop with allocator Reset, then three serial cycles in which allocator Reset is the first engine call
 * after the GPU finished: the test learns that from the READBACK memory, not from the engine, so only Reset's own
 * retirement (V8) can release the allocator's submission, and Reset must still reach vkResetCommandPool. Then
 * the negative control: the same loop without allocator Reset. The loop with Reset may keep a quarter of the
 * command buffers that the control keeps. */
static void allocator_reuse(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    enum { SERIAL = 3, CONTROL_MIN = (ALLOC_CYCLES - ALLOC_WARMUP) / 2 };
    const UINT64 size = ALLOC_WORDS * sizeof(UINT32);
    ID3D12Resource *upload = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    struct allocator_loop loop, control;
    unsigned int n, serial_resets = 0;
    volatile const UINT32 *mapped = NULL;
    BOOL ok, loop_ok, control_ok = FALSE;
    UINT32 *upload_words = NULL;
    ID3D12Fence *fence = NULL;
    D3D12_RANGE range;
    ULONGLONG start;
    UINT32 marker;
    UINT64 value;
    LONG count;

    upload = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    ok = upload && readback
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12GraphicsCommandList_Close(list))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    check(ok, "allocator reuse: 4 KiB UPLOAD and READBACK, an allocator, a list, a fence");
    if (!ok)
        goto done;
    memset(&range, 0, sizeof(range));
    if (FAILED(ID3D12Resource_Map(upload, 0, &range, (void **)&upload_words))
            || FAILED(ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped)))
    {
        check(FALSE, "allocator reuse: Map UPLOAD and READBACK");
        goto done;
    }
    memset(upload_words, 0, (size_t)size);

    loop_ok = allocator_loop(device, queue, upload, readback, fence, 0, TRUE, &loop);
    printf("allocator reuse: %u cycles, %u GetCompletedValue polls, %u allocator Resets, %ld vkResetCommandPool "
            "calls, %u Resets without one, command buffers kept %+ld and private bytes %+lld from cycle %u to %u\n",
            loop.cycles, loop.polls, loop.resets, (long)loop.pool_resets, loop.missed_resets,
            (long)loop.command_buffers, loop.growth, (unsigned int)ALLOC_WARMUP, (unsigned int)ALLOC_CYCLES);
    checkf(loop_ok && loop.cycles == ALLOC_CYCLES, "allocator reuse: %u cycles over two allocators, each Reset "
            "after polling its fence value", (unsigned int)ALLOC_CYCLES);
    checkf(loop_ok && loop.resets == ALLOC_CYCLES && !loop.missed_resets, "allocator reuse: each of the %u "
            "allocator Resets calls vkResetCommandPool (%u do not)", loop.resets, loop.missed_resets);
    census_expect_none("after 100 allocator cycles");
    if (!loop_ok)
        goto done;

    /* Reset n + 1 follows serial cycle n's submission, with no engine call in between. */
    value = ALLOC_CYCLES;
    for (n = 1; n <= SERIAL + 1 && ok; ++n)
    {
        count = vkw.pool_resets;
        if (FAILED(ID3D12CommandAllocator_Reset(allocator)))
        {
            ok = FALSE;
            break;
        }
        if (n > 1 && vkw.pool_resets > count)
            ++serial_resets;
        if (n > SERIAL)
            break;
        if (FAILED(ID3D12GraphicsCommandList_Reset(list, allocator, NULL)))
        {
            ok = FALSE;
            break;
        }
        marker = 0xc0de0000u + n;
        upload_words[0] = marker;
        record_copy(list, readback, upload);
        ok = SUCCEEDED(ID3D12GraphicsCommandList_Close(list));
        if (ok)
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        ok = ok && SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, ++value));
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
    printf("allocator reuse: %u of %u allocator Resets that were the first engine call after the GPU finished "
            "called vkResetCommandPool\n", serial_resets, (unsigned int)SERIAL);
    checkf(ok && serial_resets == SERIAL, "allocator reuse: %u serial cycles; each allocator Reset that is the first "
            "engine call after the GPU finished calls vkResetCommandPool", (unsigned int)SERIAL);
    ok = ok && wait_fence_inline(fence, value, WAIT_NULL_EVENT, "allocator reuse (serial)");
    if (!ok)
        goto done;

    control_ok = allocator_loop(device, queue, upload, readback, fence, value, FALSE, &control);
    printf("allocator reuse (no Reset): %u cycles, %u GetCompletedValue polls, %ld vkResetCommandPool calls, "
            "command buffers kept %+ld and private bytes %+lld from cycle %u to %u\n", control.cycles, control.polls,
            (long)control.pool_resets, (long)control.command_buffers, control.growth, (unsigned int)ALLOC_WARMUP,
            (unsigned int)ALLOC_CYCLES);
    checkf(control_ok && control.cycles == ALLOC_CYCLES && !control.pool_resets, "negative control: the same %u "
            "cycles without allocator Reset call vkResetCommandPool 0 times", (unsigned int)ALLOC_CYCLES);
    checkf(control_ok && control.command_buffers >= CONTROL_MIN, "negative control: without allocator Reset, the "
            "allocators keep %ld more command buffers from cycle %u to %u (at least %u)",
            (long)control.command_buffers, (unsigned int)ALLOC_WARMUP, (unsigned int)ALLOC_CYCLES,
            (unsigned int)CONTROL_MIN);
    checkf(control_ok && loop.command_buffers <= control.command_buffers / 4, "allocator reuse: with allocator "
            "Reset, the allocators keep %+ld command buffers from cycle %u to %u, at most a quarter of the %ld "
            "without", (long)loop.command_buffers, (unsigned int)ALLOC_WARMUP, (unsigned int)ALLOC_CYCLES,
            (long)control.command_buffers);

done:
    if (mapped)
        ID3D12Resource_Unmap(readback, 0, NULL);
    if (upload_words)
        ID3D12Resource_Unmap(upload, 0, NULL);
    if (fence)
        ID3D12Fence_Release(fence);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocator)
        ID3D12CommandAllocator_Release(allocator);
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

/* V7 admission. INLINE needs BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES usable VkQueues in the graphics family: the
 * internal queue and two DIRECT queues. The Vulkan wrapper caps the queueCount that graphics families report.
 * Below the minimum, CreateDevice fails with DXGI_ERROR_UNSUPPORTED and binds nothing; at it, the device has two
 * DIRECT queues at once, and a third finds every VkQueue in use. Runs with no other device bound. */
static void inline_admission(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *info)
{
    ID3D12CommandQueue *q1 = NULL, *q2 = NULL, *q3 = NULL;
    ID3D12Device *device;
    unsigned int binds, cap, released = 0;
    HRESULT hr, hr3, query_hr;

    for (cap = 1; cap < BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES; ++cap)
    {
        binds = shell.binds + shell.failed_binds;
        vkw.graphics_queue_cap = (LONG)cap;
        device = (ID3D12Device *)(void *)1;
        hr = funcs->CreateDevice(info, &IID_ID3D12Device, (void **)&device);
        query_hr = funcs->QueryAdapterCaps(info, 0, NULL);
        vkw.graphics_queue_cap = 0;
        checkf(hr == DXGI_ERROR_UNSUPPORTED && !device && shell.binds + shell.failed_binds == binds,
                "admission: INLINE with the graphics family capped at %u VkQueue(s) -> DXGI_ERROR_UNSUPPORTED, "
                "nothing bound (hr %08lx)", cap, (unsigned long)hr);
        checkf(query_hr == hr, "admission: QueryAdapterCaps at that cap fails as CreateDevice does (V11, hr %08lx)",
                (unsigned long)query_hr);
        if (SUCCEEDED(hr) && device && device != (void *)1)
            ID3D12Device_Release(device);
    }

    vkw.graphics_queue_cap = BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES;
    device = NULL;
    hr = funcs->CreateDevice(info, &IID_ID3D12Device, (void **)&device);
    query_hr = funcs->QueryAdapterCaps(info, 0, NULL);
    vkw.graphics_queue_cap = 0;
    checkf(query_hr == S_OK, "admission: QueryAdapterCaps with the graphics family capped at %u VkQueues -> S_OK "
            "(hr %08lx)", (unsigned int)BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES, (unsigned long)query_hr);
    checkf(SUCCEEDED(hr) && device, "admission: INLINE with the graphics family capped at %u VkQueues -> a device "
            "(hr %08lx)", (unsigned int)BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES, (unsigned long)hr);
    if (FAILED(hr) || !device)
        return;
    if (FAILED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xa1), &q1)))
        q1 = NULL;
    if (FAILED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xa2), &q2)))
        q2 = NULL;
    if (FAILED(hr3 = create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xa3), &q3)))
        q3 = NULL;
    check(q1 && q2 && shell_queue_of(COOKIE(0xa1)) && shell_queue_of(COOKIE(0xa2))
            && shell_queue_of(COOKIE(0xa1)) != shell_queue_of(COOKIE(0xa2)),
            "admission: that device has two DIRECT queues at once, on two VkQueues");
    checkf(hr3 == E_OUTOFMEMORY && !q3, "admission: a third DIRECT queue finds every VkQueue in use -> "
            "E_OUTOFMEMORY (hr %08lx)", (unsigned long)hr3);
    if (q3 && !ID3D12CommandQueue_Release(q3))
        ++released;
    if (q2 && !ID3D12CommandQueue_Release(q2))
        ++released;
    if (q1 && !ID3D12CommandQueue_Release(q1))
        ++released;
    check(released == (q1 != NULL) + (q2 != NULL) + (q3 != NULL) && !ID3D12Device_Release(device)
            && !shell.bound_count, "admission: its queues and the device return 0 from their final Release, "
            "nothing stays bound");
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
    tls_engine_caller = TRUE;

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
    inline_admission(funcs, &info);

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

    /* V11 in INLINE: the answers of the device just made, with no VkDevice, Services call or thread. */
    n = shell.binds + shell.failed_binds + shell.unbinds;
    refs = (ULONG)vkw.device_creates;
    adapter_caps_equality(funcs, &info, device, "INLINE: ");
    check(shell.binds + shell.failed_binds + shell.unbinds == n && (ULONG)vkw.device_creates == refs,
            "QueryAdapterCaps (INLINE) creates no VkDevice and calls no Services");
    census_expect_none("after QueryAdapterCaps");

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
    concurrent_queue_creation(funcs, device);

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
    cross_queue_copy(device, qa, qb, va, vb);
    census_expect_none("after fence waits");
    allocator_reuse(device, qa);
    multiple_fence_waits(device);
    imported_memory(funcs, device, info.GetInstanceProcAddr, qa, WAIT_NULL_EVENT, "INLINE imported memory: ");

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
    check(!shell.wrong_thread, "BindQueue and UnbindQueue ran on the test's threads that make engine calls");
    check(!shell.double_binds, "no VkQueue or cookie was bound twice");
    census_expect_none("after the device's final Release");
    DeleteCriticalSection(&shell.lock);
}

static DWORD WINAPI hang_watchdog(void *arg)
{
    (void)arg;
    if (WaitForSingleObject(hold.stop, 30000) == WAIT_TIMEOUT)
        hold_release("the 30 s watchdog");
    return 0;
}

/* --hang, ABI 1.1 and the INLINE queue mode: a GPU wait that does not end. The Vulkan wrapper holds queue A's
 * ExecuteCommandLists submission on a semaphore that only the test signals, so the Signal(F, 1) that completes
 * with it does not reach 1 on its own. SetEventOnCompletion(1, NULL) must give up after the wait budget (V7, BC250_VKD3D_INLINE_WAIT_BUDGET_MS)
 * with DXGI_ERROR_DEVICE_HUNG and leave the device removed with that reason; a second wait must fail at once. A
 * watchdog thread signals the semaphore after 30 s, so a wait without a bound ends too, late, and fails the timing
 * check. Then the test signals the semaphore, and the queue, the fence and the device must still come apart
 * cleanly. */
static void hang_suite(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *base)
{
    const double budget = BC250_VKD3D_INLINE_WAIT_BUDGET_MS;
    BC250_VKD3D_SHELL_SERVICES services;
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    ID3D12Resource *upload = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    LARGE_INTEGER t0, t1, freq;
    ID3D12CommandQueue *qa = NULL;
    double first_ms, second_ms;
    ID3D12Device *device = NULL;
    ID3D12Fence *fence = NULL;
    HANDLE watchdog = NULL;
    HRESULT hr, removed;
    VkQueue va = NULL;
    ULONGLONG start;
    UINT64 value;
    ULONG refs;
    BOOL held;

    InitializeCriticalSection(&shell.lock);
    tls_engine_caller = TRUE;
    QueryPerformanceFrequency(&freq);

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

    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    check(SUCCEEDED(hr) && device, "hang: CreateDevice in the INLINE queue mode (ABI 1.1)");
    print_icd_modules();
    if (FAILED(hr) || !device)
    {
        device = NULL;
        goto done;
    }

    if (FAILED(create_inline_queue(funcs, device, D3D12_COMMAND_LIST_TYPE_DIRECT, COOKIE(0xa0), &qa)))
        qa = NULL;
    va = shell_queue_of(COOKIE(0xa0));
    upload = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 4096, D3D12_RESOURCE_STATE_COPY_DEST);
    check(qa && va && hold_create() && upload && readback
            && SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&allocator))
            && SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            && SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                    (void **)&fence)), "hang: queue A, a list with a 4 KiB copy, fence F and a timeline semaphore "
            "through the device's own entry points");
    if (!qa || !va || !hold.semaphore || !list || !fence)
        goto done;
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, upload, 0, 4096);
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
    {
        check(FALSE, "hang: record the copy");
        goto done;
    }
    if (!(hold.stop = CreateEventW(NULL, TRUE, FALSE, NULL))
            || !(watchdog = CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL)))
    {
        check(FALSE, "hang: watchdog thread");
        goto done;
    }

    /* A queue Signal submits nothing of its own: it completes with the queue's last submission. */
    hold_arm(va);
    ID3D12CommandQueue_ExecuteCommandLists(qa, 1, (ID3D12CommandList **)&list);
    held = hold_disarm();
    hr = ID3D12CommandQueue_Signal(qa, fence, 1);
    check(SUCCEEDED(hr) && held, "hang: queue A ExecuteCommandLists (a 4 KiB copy), which the wrapper holds, and "
            "Signal(F, 1)");

    QueryPerformanceCounter(&t0);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, NULL);
    QueryPerformanceCounter(&t1);
    first_ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    removed = ID3D12Device_GetDeviceRemovedReason(device);
    printf("hang: SetEventOnCompletion(1, NULL) hr %08lx after %.0f ms, GetDeviceRemovedReason %08lx\n",
            (unsigned long)hr, first_ms, (unsigned long)removed);
    checkf(hr == DXGI_ERROR_DEVICE_HUNG && first_ms >= budget - 500.0 && first_ms < 2.0 * budget,
            "hang: SetEventOnCompletion(1, NULL) on a GPU wait that does not end -> DXGI_ERROR_DEVICE_HUNG after "
            "the %.0f ms budget (%.0f ms)", budget, first_ms);
    checkf(removed == DXGI_ERROR_DEVICE_HUNG, "hang: GetDeviceRemovedReason -> DXGI_ERROR_DEVICE_HUNG (%08lx)",
            (unsigned long)removed);

    QueryPerformanceCounter(&t0);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, NULL);
    QueryPerformanceCounter(&t1);
    second_ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    printf("hang: second SetEventOnCompletion(1, NULL) hr %08lx after %.2f ms\n", (unsigned long)hr, second_ms);
    checkf(hr == DXGI_ERROR_DEVICE_HUNG && second_ms < 100.0, "hang: a second SetEventOnCompletion(1, NULL) fails "
            "at once with DXGI_ERROR_DEVICE_HUNG (%.2f ms)", second_ms);
    check(ID3D12Fence_GetCompletedValue(fence) == 0, "hang: F is still at 0 while the semaphore holds A");

    hold_release("the test");
    SetEvent(hold.stop);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);
    watchdog = NULL;
    check(hold.wait_idle(hold.device) == VK_SUCCESS, "hang: vkDeviceWaitIdle once the semaphore is signalled");
    start = GetTickCount64();
    while ((value = ID3D12Fence_GetCompletedValue(fence)) < 1 && GetTickCount64() - start < 5000)
        Sleep(1);
    checkf(value == 1, "hang: F then reaches 1 through GetCompletedValue; retirement goes on on a removed device "
            "(%llu)", (unsigned long long)value);

done:
    if (watchdog)
    {
        hold_release("the test");
        SetEvent(hold.stop);
        WaitForSingleObject(watchdog, INFINITE);
        CloseHandle(watchdog);
    }
    if (hold.stop)
        CloseHandle(hold.stop);
    hold_destroy();
    if (fence)
        ID3D12Fence_Release(fence);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (allocator)
        ID3D12CommandAllocator_Release(allocator);
    if (readback)
        ID3D12Resource_Release(readback);
    if (upload)
        ID3D12Resource_Release(upload);
    if (qa)
        check(ID3D12CommandQueue_Release(qa) == 0 && !shell_queue_of(COOKIE(0xa0)),
                "hang: queue A final Release 0, unbound");
    if (device)
    {
        refs = ID3D12Device_Release(device);
        printf("device final Release: %lu\n", refs);
        check(refs == 0, "hang: device final Release returns 0");
    }
    printf("shell: %u binds, %u unbinds, %u failed binds, %u still bound, %u on another thread, %u double binds, "
            "%u unknown unbinds\n", shell.binds, shell.unbinds, shell.failed_binds, shell.bound_count,
            shell.wrong_thread, shell.double_binds, shell.unknown_unbinds);
    check(shell.binds == shell.unbinds && !shell.bound_count && !shell.unknown_unbinds && !shell.wrong_thread
            && !shell.double_binds, "hang: every BindQueue has its UnbindQueue, on the thread of the engine call, "
            "no VkQueue bound twice");
    census_expect_none("after the hang suite");
    DeleteCriticalSection(&shell.lock);
}

/* V7 admission leaves THREADED alone: with the Vulkan wrapper capping the graphics family at one VkQueue, a
 * THREADED device is created and copies on a direct queue. */
static void threaded_admission(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *base)
{
    BC250_VKD3D_DEVICE_CREATE_INFO info = *base;
    ID3D12Device *device = NULL;
    HRESULT hr;

    info.Size = sizeof(info);
    info.AbiVersion = ABI_1_1;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_THREADED;
    info.Services = NULL;
    info.GetInstanceProcAddr = vkw_GetInstanceProcAddr;
    vkw.graphics_queue_cap = 1;
    hr = funcs->CreateDevice(&info, &IID_ID3D12Device, (void **)&device);
    vkw.graphics_queue_cap = 0;
    checkf(SUCCEEDED(hr) && device, "admission: THREADED with the graphics family capped at 1 VkQueue -> a device "
            "(hr %08lx)", (unsigned long)hr);
    if (FAILED(hr) || !device)
        return;
    copy_round_trip(device, NULL, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_EVENT, "one-VkQueue THREADED device: ");
    check(ID3D12Device_Release(device) == 0, "admission: that device's final Release returns 0");
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
    adapter_caps_equality(funcs, &info, device, "THREADED: ");
    copy_round_trip(device, NULL, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_EVENT, "");
    compute_dispatch(device, NULL, WAIT_EVENT, "");
    imported_memory(funcs, device, info.GetInstanceProcAddr, NULL, WAIT_EVENT, "THREADED imported memory: ");

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
    info.Size = BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_1;
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

    threaded_admission(funcs, base);
    return 0;
}

/* One engine device of the instance isolation test: its engine calls run with its host selected (tls_host), as the
 * shell's do, and in INLINE it has Services of its own (host_shell[host]). */
static struct test_shell host_shell[HOSTW_HOSTS];

struct host_device
{
    const BC250_VKD3D_ENGINE_FUNCS *funcs;
    BC250_VKD3D_DEVICE_CREATE_INFO info;
    BC250_VKD3D_SHELL_SERVICES services;
    unsigned int host;
    ID3D12Device *device;
    HRESULT hr;
};

static void host_device_init(struct host_device *d, const BC250_VKD3D_ENGINE_FUNCS *funcs,
        const BC250_VKD3D_DEVICE_CREATE_INFO *base, BOOL inline_mode, unsigned int host, UINT32 instance_mode)
{
    memset(d, 0, sizeof(*d));
    d->funcs = funcs;
    d->host = host;
    d->info = *base;
    d->info.Size = sizeof(d->info);
    d->info.AbiVersion = ABI_1_2;
    d->info.GetInstanceProcAddr = hostw_GetInstanceProcAddr;
    d->info.QueueMode = inline_mode ? BC250_VKD3D_QUEUE_MODE_INLINE : BC250_VKD3D_QUEUE_MODE_THREADED;
    d->info.Services = NULL;
    d->info.InstanceMode = instance_mode;
    if (inline_mode)
    {
        d->services.Size = sizeof(d->services);
        d->services.Shell = &host_shell[host];
        d->services.BindQueue = shell_bind_queue;
        d->services.UnbindQueue = shell_unbind_queue;
        d->info.Services = &d->services;
    }
}

static DWORD WINAPI host_device_create(void *arg)
{
    struct host_device *d = arg;

    tls_engine_caller = TRUE;
    tls_host = d->host;
    if (FAILED(d->hr = d->funcs->CreateDevice(&d->info, &IID_ID3D12Device, (void **)&d->device)))
        d->device = NULL;
    tls_host = 0;
    return 0;
}

static VkInstance host_device_instance(const struct host_device *d)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device;
    VkDevice vk_device;
    UINT32 family;

    tls_host = d->host;
    if (d->device && FAILED(d->funcs->GetVulkanHandles(d->device, &instance, &physical_device, &vk_device, &family)))
        instance = VK_NULL_HANDLE;
    tls_host = 0;
    return instance;
}

/* A copy round trip on the device: on a DIRECT queue of the engine's own in THREADED, on a DIRECT queue from
 * CreateCommandQueue in INLINE. */
static void host_device_copy(struct host_device *d, BOOL inline_mode, const char *tag)
{
    ID3D12CommandQueue *queue = NULL;

    tls_host = d->host;
    if (!inline_mode)
    {
        copy_round_trip(d->device, NULL, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_EVENT, tag);
    }
    else if (SUCCEEDED(create_inline_queue(d->funcs, d->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            COOKIE(0x1000 + d->host), &queue)) && queue)
    {
        copy_round_trip(d->device, queue, D3D12_COMMAND_LIST_TYPE_DIRECT, WAIT_NULL_EVENT, tag);
        checkf(ID3D12CommandQueue_Release(queue) == 0, "%sthe DIRECT queue's final Release returns 0", tag);
    }
    else
    {
        checkf(FALSE, "%sCreateCommandQueue(DIRECT)", tag);
    }
    tls_host = 0;
}

static ULONG host_device_release(struct host_device *d)
{
    ULONG refs = 0;

    tls_host = d->host;
    if (d->device)
        refs = ID3D12Device_Release(d->device);
    d->device = NULL;
    tls_host = 0;
    return refs;
}

/* V12 in the suite's queue mode, after the suite's own devices are gone. The entry point is the host emulation
 * (hostw) over the test's driver.
 *   SHARED, the default: device A with the 1.1-sized CreateInfo of an r3 shell (PRIVATE past its Size, not read)
 *   and device B with InstanceMode SHARED share A's VkInstance, as in upstream vkd3d-proton. B's CreateDevice
 *   then calls host 1's entry point with host 2 selected, which the emulation must see: its positive control.
 *   PRIVATE: devices A and B, created on two threads with hosts 1 and 2 selected, have VkInstances of their own,
 *   and every entry point call of each goes to its own host; each completes a copy round trip. While both live,
 *   QueryAdapterCaps with host 3 selected answers as A's CheckFeatureSupport, HRESULT and every byte, on a new
 *   VkInstance of host 3 that it destroys before it returns, without a VkDevice, a Services call or any call to
 *   host 1 or 2. The final Releases destroy each VkInstance through its own host.
 *   With ENABLE_VULKAN_RENDERDOC_CAPTURE=1, PRIVATE CreateDevice and QueryAdapterCaps fail with E_INVALIDARG and
 *   leave no VkInstance (V2). In INLINE, a PRIVATE CreateDevice that the admission refuses destroys its VkInstance.
 */
static void instance_isolation(const BC250_VKD3D_ENGINE_FUNCS *funcs, const BC250_VKD3D_DEVICE_CREATE_INFO *base,
        BOOL inline_mode)
{
    const char *mode = inline_mode ? "INLINE" : "THREADED";
    LONG gipa1, dispatch1, gipa2, dispatch2;
    struct host_device a, b, q;
    HANDLE threads[2];
    VkInstance ia, ib;
    HRESULT hr, query_hr;
    unsigned int i, services;
    ULONG ra, rb;
    char tag[64];
    DWORD wait;

    InitializeCriticalSection(&hostw.lock);
    hostw.next = vkw.gipa;
    for (i = 1; i < HOSTW_HOSTS; ++i)
    {
        memset(&host_shell[i], 0, sizeof(host_shell[i]));
        InitializeCriticalSection(&host_shell[i].lock);
        host_shell[i].host = i;
    }
    tls_engine_caller = TRUE;
    printf("isolation: %s, hosts 1 to 3 over the test's entry point\n", mode);

    /* SHARED. */
    host_device_init(&a, funcs, base, inline_mode, 1, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
    a.info.Size = BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_1;
    host_device_init(&b, funcs, base, inline_mode, 2, BC250_VKD3D_INSTANCE_MODE_SHARED);
    hostw_reset();
    host_device_create(&a);
    host_device_create(&b);
    ia = host_device_instance(&a);
    ib = host_device_instance(&b);
    printf("isolation: SHARED: A hr %08lx VkInstance %p, B hr %08lx VkInstance %p; VkInstances created: host 1 %ld, "
            "host 2 %ld; host 1 calls with host 2 selected: %ld GetInstanceProcAddr, %ld dispatch\n",
            (unsigned long)a.hr, (void *)ia, (unsigned long)b.hr, (void *)ib, hostw.host[1].instances,
            hostw.host[2].instances, hostw.host[1].foreign_gipa, hostw.host[1].foreign_dispatch);
    checkf(a.device && hostw.host[1].instances == 1, "isolation %s SHARED: A creates a VkInstance, so none of the "
            "suite's devices, refused ones included, keeps the shared one alive", mode);
    checkf(a.device && b.device && ia && ia == ib && hostw.host[1].instances == 1 && !hostw.host[2].instances,
            "isolation %s SHARED (A: 1.1-sized CreateInfo, PRIVATE past its Size; B: InstanceMode SHARED): B takes "
            "A's VkInstance and creates none, as upstream vkd3d-proton", mode);
    checkf(hostw_owner(ia) == 1 && hostw.host[1].foreign_gipa > 0 && hostw.host[1].foreign_dispatch > 0,
            "isolation %s SHARED: the host emulation sees B's CreateDevice call host 1's entry point (its positive "
            "control)", mode);
    rb = host_device_release(&b);
    ra = host_device_release(&a);
    checkf(!ra && !rb && hostw.host[1].destroyed == 1 && !hostw.bound_count, "isolation %s SHARED: final Releases "
            "return 0, and the last one destroys the VkInstance, once", mode);

    /* PRIVATE, two devices created on two threads. */
    host_device_init(&a, funcs, base, inline_mode, 1, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
    host_device_init(&b, funcs, base, inline_mode, 2, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
    for (i = 1; i < HOSTW_HOSTS; ++i)
        host_shell[i].binds = host_shell[i].unbinds = host_shell[i].failed_binds = 0;
    hostw_reset();
    threads[0] = CreateThread(NULL, 0, host_device_create, &a, 0, NULL);
    threads[1] = CreateThread(NULL, 0, host_device_create, &b, 0, NULL);
    wait = threads[0] && threads[1] ? WaitForMultipleObjects(2, threads, TRUE, 60000) : WAIT_FAILED;
    for (i = 0; i < 2; ++i)
    {
        if (threads[i])
            CloseHandle(threads[i]);
    }
    if (wait != WAIT_OBJECT_0)
    {
        check(FALSE, "isolation: two CreateDevice threads end within 60 s");
        printf("FAILED (%u)\n", failures);
        ExitProcess(1);
    }
    ia = host_device_instance(&a);
    ib = host_device_instance(&b);
    printf("isolation: PRIVATE: A hr %08lx VkInstance %p of host %u, B hr %08lx VkInstance %p of host %u\n",
            (unsigned long)a.hr, (void *)ia, hostw_owner(ia), (unsigned long)b.hr, (void *)ib, hostw_owner(ib));
    checkf(a.device && b.device && ia && ib && ia != ib && hostw_owner(ia) == 1 && hostw_owner(ib) == 2
            && hostw.host[1].instances == 1 && hostw.host[2].instances == 1, "isolation %s PRIVATE: A and B, created "
            "on two threads with hosts 1 and 2 selected, have VkInstances of their own, each created by its host",
            mode);
    printf("isolation: PRIVATE: host 1 %ld GetInstanceProcAddr and %ld dispatch calls, host 2 %ld and %ld; %ld calls "
            "on another host or none\n", hostw.host[1].gipa_calls, hostw.host[1].dispatch_calls,
            hostw.host[2].gipa_calls, hostw.host[2].dispatch_calls, hostw_foreign());
    checkf(!hostw_foreign() && hostw.host[1].gipa_calls && hostw.host[1].device_creates == 1
            && hostw.host[2].gipa_calls && hostw.host[2].device_creates == 1, "isolation %s PRIVATE: every entry "
            "point call of each device goes to its own host, with one vkCreateDevice each", mode);
    if (inline_mode)
        check(host_shell[1].binds == 1 && host_shell[2].binds == 1 && !host_shell[1].wrong_host
                && !host_shell[2].wrong_host, "isolation INLINE PRIVATE: each device binds its internal queue "
                "through its own Services, with its host selected");

    if (a.device && b.device)
    {
        snprintf(tag, sizeof(tag), "isolation %s PRIVATE A: ", mode);
        host_device_copy(&a, inline_mode, tag);
        snprintf(tag, sizeof(tag), "isolation %s PRIVATE B: ", mode);
        host_device_copy(&b, inline_mode, tag);
        checkf(!hostw_foreign(), "isolation %s PRIVATE: the copies call no entry point of another host", mode);

        /* QueryAdapterCaps while A and B live. */
        host_device_init(&q, funcs, base, inline_mode, 3, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
        gipa1 = hostw.host[1].gipa_calls;
        dispatch1 = hostw.host[1].dispatch_calls;
        gipa2 = hostw.host[2].gipa_calls;
        dispatch2 = hostw.host[2].dispatch_calls;
        snprintf(tag, sizeof(tag), "isolation %s PRIVATE: ", mode);
        tls_host = 3;
        adapter_caps_equality(funcs, &q.info, a.device, tag);
        tls_host = 0;
        printf("isolation: QueryAdapterCaps: host 3 created %ld and destroyed %ld VkInstances, %ld vkCreateDevice\n",
                hostw.host[3].instances, hostw.host[3].destroyed, hostw.host[3].device_creates);
        checkf(hostw.host[3].instances == 1 && hostw.host[3].destroyed == 1 && !hostw.host[3].device_creates
                && hostw.bound_count == 2, "isolation %s: QueryAdapterCaps (PRIVATE, host 3) while A and B live makes "
                "a VkInstance of host 3 and destroys it before it returns, with no VkDevice", mode);
        services = host_shell[3].binds + host_shell[3].unbinds + host_shell[3].failed_binds;
        checkf(hostw.host[1].gipa_calls == gipa1 && hostw.host[1].dispatch_calls == dispatch1
                && hostw.host[2].gipa_calls == gipa2 && hostw.host[2].dispatch_calls == dispatch2
                && !hostw_foreign() && !services, "isolation %s: that QueryAdapterCaps calls neither host 1 nor host 2 "
                "and no Services", mode);
    }
    rb = host_device_release(&b);
    ra = host_device_release(&a);
    checkf(!ra && !rb && hostw.host[1].destroyed == 1 && hostw.host[2].destroyed == 1 && !hostw.bound_count
            && !hostw_foreign(), "isolation %s PRIVATE: final Releases return 0 and destroy each device's VkInstance "
            "through its own host", mode);
    if (inline_mode)
    {
        for (i = 1, services = 0; i < HOSTW_HOSTS; ++i)
            services += host_shell[i].binds != host_shell[i].unbinds || host_shell[i].bound_count
                    || host_shell[i].wrong_thread || host_shell[i].wrong_host || host_shell[i].double_binds
                    || host_shell[i].unknown_unbinds;
        check(!services, "isolation INLINE: every BindQueue of each host's Services has its UnbindQueue, with that "
                "host selected, on the thread of the engine call");
    }

    /* RenderDoc's singleton devices (V2). */
    host_device_init(&a, funcs, base, inline_mode, 1, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
    host_shell[1].binds = 0;
    hostw_reset();
    SetEnvironmentVariableA("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1");
    tls_host = 1;
    a.device = (ID3D12Device *)(void *)1;
    hr = funcs->CreateDevice(&a.info, &IID_ID3D12Device, (void **)&a.device);
    query_hr = funcs->QueryAdapterCaps(&a.info, 0, NULL);
    tls_host = 0;
    SetEnvironmentVariableA("ENABLE_VULKAN_RENDERDOC_CAPTURE", NULL);
    checkf(hr == E_INVALIDARG && !a.device && query_hr == E_INVALIDARG
            && hostw.host[1].instances == hostw.host[1].destroyed && !hostw.bound_count
            && !host_shell[1].binds, "isolation %s: with ENABLE_VULKAN_RENDERDOC_CAPTURE=1 "
            "PRIVATE CreateDevice and QueryAdapterCaps -> E_INVALIDARG (%08lx, %08lx), no device, no VkInstance left",
            mode, (unsigned long)hr, (unsigned long)query_hr);
    if (hr == S_OK && a.device && a.device != (void *)1)
        host_device_release(&a);

    /* The admission (V7) refuses an INLINE device after its VkInstance exists; the refusal must not keep it. The
     * inline suite's Vulkan wrapper, put behind the host emulation, caps the graphics family at one VkQueue. */
    if (inline_mode)
    {
        host_device_init(&a, funcs, base, TRUE, 1, BC250_VKD3D_INSTANCE_MODE_PRIVATE);
        host_shell[1].binds = 0;
        hostw_reset();
        hostw.next = vkw_GetInstanceProcAddr;
        vkw.graphics_queue_cap = 1;
        tls_host = 1;
        a.device = (ID3D12Device *)(void *)1;
        hr = funcs->CreateDevice(&a.info, &IID_ID3D12Device, (void **)&a.device);
        tls_host = 0;
        vkw.graphics_queue_cap = 0;
        hostw.next = vkw.gipa;
        checkf(hr == DXGI_ERROR_UNSUPPORTED && !a.device && hostw.host[1].instances == 1
                && hostw.host[1].destroyed == 1 && !hostw.bound_count && !host_shell[1].binds, "isolation INLINE "
                "PRIVATE: CreateDevice that the admission refuses (graphics family capped at 1 VkQueue) -> "
                "DXGI_ERROR_UNSUPPORTED (%08lx), and its VkInstance is destroyed", (unsigned long)hr);
        if (hr == S_OK && a.device && a.device != (void *)1)
            host_device_release(&a);
    }

    for (i = 1; i < HOSTW_HOSTS; ++i)
        DeleteCriticalSection(&host_shell[i].lock);
    DeleteCriticalSection(&hostw.lock);
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
    BOOL inline_mode = FALSE, hang_mode = FALSE, tail_kept;
    char full[MAX_PATH];
    unsigned int k;
    LUID luid;
    int i;

    if (argc < 2)
    {
        printf("usage: amdgpu_wddm_vkd3d_engine_test <amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <path>]"
                " [--fl <hex>] [--inline | --hang]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&vkw.lock);
    for (i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc)
            icd = argv[++i];
        else if (!strcmp(argv[i], "--fl") && i + 1 < argc)
            min_fl = (UINT32)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--inline"))
            inline_mode = TRUE;
        else if (!strcmp(argv[i], "--hang"))
            hang_mode = TRUE;
        else if (argv[i][0] == '-')
        {
            printf("FAIL  unknown or incomplete option %s\n", argv[i]);
            return 2;
        }
        else
            filter = argv[i];
    }
    if (inline_mode && hang_mode)
    {
        printf("FAIL  --inline and --hang are separate runs\n");
        return 2;
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
    /* A 1.1 shell's table ends before GetVulkanHandles; a shell that requires 1.1 gets no 1.2 entry even in a
     * 1.2-sized table. */
    memset(&funcs, 0x5e, sizeof(funcs));
    funcs.Size = BC250_VKD3D_ENGINE_FUNCS_SIZE_1_1;
    check(get_funcs(ABI_1_1, &funcs) == S_OK && funcs.CreateDevice && funcs.CreateCommandQueue,
            "GetFuncs(1.1) with the 1.1 Size");
    for (k = BC250_VKD3D_ENGINE_FUNCS_SIZE_1_1, tail_kept = TRUE; k < sizeof(funcs); ++k)
        tail_kept &= ((const BYTE *)&funcs)[k] == 0x5e;
    checkf(funcs.Size == BC250_VKD3D_ENGINE_FUNCS_SIZE_1_1 && tail_kept,
            "GetFuncs(1.1) with the 1.1 Size (%u) writes nothing past it",
            (unsigned int)BC250_VKD3D_ENGINE_FUNCS_SIZE_1_1);
    memset(&funcs, 0x5e, sizeof(funcs));
    funcs.Size = sizeof(funcs);
    check(get_funcs(ABI_1_1, &funcs) == S_OK && funcs.CreateDevice && funcs.CreateCommandQueue
            && !funcs.GetVulkanHandles && !funcs.CreateHeapFromMemory && !funcs.MapHeap && !funcs.UnmapHeap
            && !funcs.QueryAdapterCaps && funcs.AbiVersion == BC250_VKD3D_ENGINE_ABI_VERSION,
            "GetFuncs(1.1) with a 1.2-sized table fills CreateDevice and CreateCommandQueue, and the 1.2 entries NULL");
    memset(&funcs, 0, sizeof(funcs));
    funcs.Size = sizeof(funcs);
    check(get_funcs(ABI_1_2, &funcs) == S_OK && funcs.AbiVersion == ABI_1_2 && funcs.CreateDevice
            && funcs.CreateCommandQueue && funcs.GetVulkanHandles && funcs.CreateHeapFromMemory && funcs.MapHeap
            && funcs.UnmapHeap && funcs.QueryAdapterCaps, "GetFuncs(1.2) fills every entry");
    if (!funcs.CreateDevice || !funcs.CreateCommandQueue || !funcs.GetVulkanHandles || !funcs.CreateHeapFromMemory
            || !funcs.MapHeap || !funcs.UnmapHeap || !funcs.QueryAdapterCaps)
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
        check(funcs.QueryAdapterCaps(&info, 1, NULL) == E_INVALIDARG,
                "QueryAdapterCaps(1 query, NULL) -> E_INVALIDARG");
        /* V12: InstanceMode counts from AbiVersion 1.2; PRIVATE below it fails. */
        bad = info;
        bad.AbiVersion = ABI_1_2;
        bad.InstanceMode = 7;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device
                && funcs.QueryAdapterCaps(&bad, 0, NULL) == E_INVALIDARG,
                "InstanceMode 7 -> E_INVALIDARG from CreateDevice and QueryAdapterCaps");
        bad.AbiVersion = ABI_1_1;
        bad.InstanceMode = BC250_VKD3D_INSTANCE_MODE_PRIVATE;
        check(funcs.CreateDevice(&bad, &IID_ID3D12Device, (void **)&device) == E_INVALIDARG && !device
                && funcs.QueryAdapterCaps(&bad, 0, NULL) == E_INVALIDARG,
                "AbiVersion 1.1 asking for PRIVATE -> E_INVALIDARG from CreateDevice and QueryAdapterCaps");
        check(funcs.MapHeap(NULL, (void **)&device) == E_INVALIDARG && !device, "MapHeap(NULL) -> E_INVALIDARG");
    }

    /* The Vulkan wrapper's driver; the THREADED suite puts the wrapper in front only where it says so. */
    vkw.gipa = gipa;
    if (inline_mode || hang_mode)
    {
        /* The Vulkan wrapper sits in front of the entry point. */
        info.GetInstanceProcAddr = vkw_GetInstanceProcAddr;
        printf("mode: %u-byte CreateInfo, AbiVersion 1.1, INLINE%s\n", (unsigned int)sizeof(info),
                hang_mode ? ", GPU hang" : "");
        if (hang_mode)
        {
            hang_suite(&funcs, &info);
        }
        else
        {
            inline_suite(&funcs, &info);
            instance_isolation(&funcs, &info, TRUE);
        }
    }
    else if (threaded_suite(&funcs, &info, luid))
    {
        return 1;
    }
    else
    {
        instance_isolation(&funcs, &info, FALSE);
    }

    printf(failures ? "FAILED (%u)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
