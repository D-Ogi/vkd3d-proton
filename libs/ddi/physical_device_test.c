/* SPDX-License-Identifier: MIT
 *
 * Host test of the LUID search of ddi_physical_device.h (BD-331): a fake vkGetInstanceProcAddr, a fake device list,
 * a failing allocator. No GPU, no Vulkan driver. Prints PASSED or FAILED and returns 0 or 1.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static int calloc_fails;

static void *test_calloc(size_t n, size_t size)
{
    return calloc_fails ? NULL : calloc(n, size);
}

#define BC250_LUID_SEARCH_CALLOC test_calloc
#include "ddi_physical_device.h"

static unsigned int failures;

#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define MIN_API VK_MAKE_API_VERSION(0, 1, 3, 0)

struct fake_device
{
    uint8_t luid[VK_LUID_SIZE];
    VkBool32 luid_valid;
    uint32_t api_version;
    const char *name;
};

static struct
{
    int no_enumerate, no_properties2;
    VkResult count_result, list_result;
    uint32_t count;
    struct fake_device devices[4];
    unsigned int list_calls;
} fake;

static VkResult VKAPI_PTR fake_enumerate(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices)
{
    uint32_t i;

    (void)instance;
    if (!devices)
    {
        *count = fake.count;
        return fake.count_result;
    }
    ++fake.list_calls;
    for (i = 0; i < *count && i < fake.count; ++i)
        devices[i] = (VkPhysicalDevice)&fake.devices[i];
    *count = i;
    return fake.list_result;
}

static void VKAPI_PTR fake_properties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2 *properties)
{
    const struct fake_device *d = (const struct fake_device *)device;
    VkPhysicalDeviceIDProperties *id = properties->pNext;

    properties->properties.apiVersion = d->api_version;
    snprintf(properties->properties.deviceName, sizeof(properties->properties.deviceName), "%s", d->name);
    id->deviceLUIDValid = d->luid_valid;
    memcpy(id->deviceLUID, d->luid, VK_LUID_SIZE);
}

static PFN_vkVoidFunction VKAPI_PTR fake_gipa(VkInstance instance, const char *name)
{
    (void)instance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return fake.no_enumerate ? NULL : (PFN_vkVoidFunction)fake_enumerate;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2"))
        return fake.no_properties2 ? NULL : (PFN_vkVoidFunction)fake_properties2;
    return NULL;
}

static const LUID want = { 0x1234, 0x5 };

static void reset(void)
{
    memset(&fake, 0, sizeof(fake));
    calloc_fails = 0;
}

static void add(uint32_t i, const LUID *luid, uint32_t api, const char *name)
{
    if (luid)
    {
        memcpy(fake.devices[i].luid, luid, VK_LUID_SIZE);
        fake.devices[i].luid_valid = VK_TRUE;
    }
    fake.devices[i].api_version = api;
    fake.devices[i].name = name;
    if (fake.count < i + 1)
        fake.count = i + 1;
}

static VkPhysicalDevice search(struct bc250_luid_search *s)
{
    /* A report from an earlier search must not survive into this one. */
    memset(s, 0x5a, sizeof(*s));
    return bc250_find_physical_device_by_luid((VkInstance)1, fake_gipa, &want, MIN_API, s);
}

int main(void)
{
    const LUID other = { 0x9999, 0x1 };
    struct bc250_luid_search s;
    VkPhysicalDevice d;

    /* A driver without vkEnumeratePhysicalDevices: the entry point step, never an empty list. */
    reset();
    fake.no_enumerate = 1;
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkGetInstanceProcAddr"));
    CHECK(s.result == VK_ERROR_INCOMPATIBLE_DRIVER);
    CHECK(s.count == 0);

    /* The same for vkGetPhysicalDeviceProperties2. */
    reset();
    fake.no_properties2 = 1;
    add(0, &want, MIN_API, "gpu");
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkGetInstanceProcAddr"));
    CHECK(s.result == VK_ERROR_INCOMPATIBLE_DRIVER);

    /* The count query fails: its result. */
    reset();
    fake.count_result = VK_ERROR_INITIALIZATION_FAILED;
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkEnumeratePhysicalDevices"));
    CHECK(s.result == VK_ERROR_INITIALIZATION_FAILED);

    /* An empty list: VK_SUCCESS and 0 devices. */
    reset();
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkEnumeratePhysicalDevices"));
    CHECK(s.result == VK_SUCCESS);
    CHECK(s.count == 0);

    /* The allocation fails: the calloc step, VK_ERROR_OUT_OF_HOST_MEMORY, never the count query's VK_SUCCESS. */
    reset();
    add(0, &want, MIN_API, "gpu");
    calloc_fails = 1;
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "calloc"));
    CHECK(s.result == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(s.count == 1);
    CHECK(fake.list_calls == 0);

    /* The list query fails. */
    reset();
    add(0, &want, MIN_API, "gpu");
    fake.list_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkEnumeratePhysicalDevices"));
    CHECK(s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

    /* A list without the LUID: VK_SUCCESS and the count; an invalid LUID never matches. */
    reset();
    add(0, &other, MIN_API, "other");
    add(1, NULL, MIN_API, "no luid");
    memcpy(fake.devices[1].luid, &want, VK_LUID_SIZE);
    d = search(&s);
    CHECK(!d);
    CHECK(!strcmp(s.step, "vkEnumeratePhysicalDevices"));
    CHECK(s.result == VK_SUCCESS);
    CHECK(s.count == 2);
    CHECK(s.below_api == 0 && s.extra == 0);

    /* The match. */
    reset();
    add(0, &other, MIN_API, "other");
    add(1, &want, MIN_API, "gpu");
    d = search(&s);
    CHECK(d == (VkPhysicalDevice)&fake.devices[1]);
    CHECK(s.result == VK_SUCCESS && s.count == 2 && s.below_api == 0 && s.extra == 0);

    /* A match below the minimum version is skipped and reported by name; a second match is counted, the first used. */
    reset();
    add(0, &want, VK_MAKE_API_VERSION(0, 1, 1, 0), "old");
    add(1, &want, MIN_API, "first");
    add(2, &want, MIN_API, "second");
    d = search(&s);
    CHECK(d == (VkPhysicalDevice)&fake.devices[1]);
    CHECK(s.below_api == 1);
    CHECK(!strcmp(s.below_api_name, "old"));
    CHECK(s.extra == 1);

    /* VK_INCOMPLETE of the list query still searches what it listed. */
    reset();
    add(0, &want, MIN_API, "gpu");
    fake.list_result = VK_INCOMPLETE;
    d = search(&s);
    CHECK(d == (VkPhysicalDevice)&fake.devices[0]);
    CHECK(s.result == VK_INCOMPLETE);

    printf(failures ? "FAILED (%u)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
