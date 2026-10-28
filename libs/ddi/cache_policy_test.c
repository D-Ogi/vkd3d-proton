/* SPDX-License-Identifier: MIT
 *
 * Host test of the disk cache decisions of amdgpu_wddm_vkd3d.dll (ddi_cache_policy.h) and of libvkd3d's file calls on
 * a path outside every ANSI code page (vkd3d_file_utils.h). No GPU, no device. The files go to a directory under the
 * current directory, which the test removes. Prints PASSED or FAILED and returns 0 or 1.
 */

#include "ddi_cache_policy.h"
#include "vkd3d_file_utils.h"

#include <stdio.h>

static unsigned int failures;

#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void test_group_keys(void)
{
    static const wchar_t game[] = L"vkd3d-proton.witcher3.exe.cache";
    const size_t key = wcslen(game);

    CHECK(bc250_cache_group_key_length(game) == key);
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.witcher3.exe.cache.write") == key);
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.witcher3.exe.cache.merge") == key);
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.witcher3.exe.cache.driver") == key);
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.witcher3.exe.cache.pso-log.txt") == key);
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.witcher3.exe.cache.driver.4242.7.tmp") == key);
    /* A program whose name has ".cache" in it keeps its whole name in the key. */
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.a.cache.exe.cache.write") ==
            wcslen(L"vkd3d-proton.a.cache.exe.cache"));
    CHECK(bc250_cache_group_key_length(L"vkd3d-proton.a.cache.exe.cache.driver.1.2.tmp") ==
            wcslen(L"vkd3d-proton.a.cache.exe.cache"));
    /* Not the engine's. */
    CHECK(!bc250_cache_group_key_length(L"vkd3d-proton.cache"));
    CHECK(!bc250_cache_group_key_length(L"vkd3d-proton.game.exe.cache.old"));
    CHECK(!bc250_cache_group_key_length(L"vkd3d-proton.game.exe.tmp"));
    CHECK(!bc250_cache_group_key_length(L"dxvk.game.exe.cache"));
    CHECK(!bc250_cache_group_key_length(L"notes.txt"));
    CHECK(!bc250_cache_group_key_length(NULL));
}

static void test_trim(void)
{
    struct bc250_cache_group g[4];

    /* Within the budget: nothing goes. */
    memset(g, 0, sizeof(g));
    g[0].size = 300; g[0].last_write = 1;
    g[1].size = 300; g[1].last_write = 2;
    CHECK(bc250_cache_plan_trim(g, 2, 600) == 600);
    CHECK(!g[0].drop && !g[1].drop);

    /* The oldest goes first, the current one is kept while others can go. */
    memset(g, 0, sizeof(g));
    g[0].size = 500; g[0].last_write = 10;
    g[1].size = 500; g[1].last_write = 5;
    g[2].size = 500; g[2].last_write = 1; g[2].current = true;
    g[3].size = 500; g[3].last_write = 7;
    CHECK(bc250_cache_plan_trim(g, 4, 1000) == 1000);
    CHECK(g[1].drop && g[3].drop && !g[0].drop && !g[2].drop);

    /* The current one alone over the budget: it starts over. */
    memset(g, 0, sizeof(g));
    g[0].size = 2000; g[0].current = true;
    g[1].size = 10; g[1].last_write = 3;
    CHECK(bc250_cache_plan_trim(g, 2, 1000) == 0);
    CHECK(g[0].drop && g[1].drop);

    /* No groups. */
    CHECK(bc250_cache_plan_trim(g, 0, 1000) == 0);
}

static bool sid_rule(BYTE authority, DWORD count, DWORD first)
{
    SID_IDENTIFIER_AUTHORITY a = { { 0, 0, 0, 0, 0, authority } };
    PSID sid = NULL;
    bool result;

    if (!AllocateAndInitializeSid(&a, (BYTE)count, first, 1001, 1002, 1003, 0, 0, 0, 0, &sid))
        return false;
    result = bc250_sid_has_driver_cache((const SID *)sid);
    FreeSid(sid);
    return result;
}

static void test_accounts(void)
{
    CHECK(sid_rule(5, 4, SECURITY_NT_NON_UNIQUE));   /* S-1-5-21-...: a local or domain user */
    CHECK(sid_rule(12, 4, 1));                       /* S-1-12-1-...: a Microsoft Entra ID user */
    CHECK(!sid_rule(5, 1, SECURITY_LOCAL_SYSTEM_RID));   /* S-1-5-18 */
    CHECK(!sid_rule(5, 1, SECURITY_LOCAL_SERVICE_RID));  /* S-1-5-19 */
    CHECK(!sid_rule(5, 2, 90));                      /* S-1-5-90-...: Window Manager\DWM-n */
    CHECK(!sid_rule(5, 2, 96));                      /* S-1-5-96-...: Font Driver Host\UMFD-n */
    CHECK(!sid_rule(5, 2, SECURITY_SERVICE_ID_BASE_RID));   /* S-1-5-80-...: a service */
    CHECK(!bc250_sid_has_driver_cache(NULL));

    CHECK(bc250_option_is_true(L"1") && bc250_option_is_true(L"TRUE") && bc250_option_is_true(L"Yes") &&
            bc250_option_is_true(L"y"));
    CHECK(!bc250_option_is_true(L"0") && !bc250_option_is_true(L"false") && !bc250_option_is_true(L"") &&
            !bc250_option_is_true(L"2") && !bc250_option_is_true(NULL));
}

/* libvkd3d's cache file calls on a directory named with characters that no ANSI code page holds together. */
static void test_files(void)
{
    WCHAR base[MAX_PATH], dir[MAX_PATH];
    char udir[MAX_PATH * 3], a[MAX_PATH * 3], b[MAX_PATH * 3];
    struct vkd3d_memory_mapped_file mapped;
    FILE *file;

    if (!GetCurrentDirectoryW(ARRAYSIZE(base), base) ||
            swprintf(dir, ARRAYSIZE(dir), L"%ls\\cache-\x0141ucja-\x65e5\x672c-\x0416", base) < 0)
    {
        CHECK(!"no test directory");
        return;
    }
    RemoveDirectoryW(dir);
    CHECK(CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, dir, -1, udir, sizeof(udir), NULL, NULL) > 0);
    snprintf(a, sizeof(a), "%s\\vkd3d-proton.test.exe.cache.write", udir);
    snprintf(b, sizeof(b), "%s\\vkd3d-proton.test.exe.cache", udir);

    CHECK((file = vkd3d_file_open_exclusive_write(a)) != NULL);
    if (file)
    {
        CHECK(fwrite("archive", 1, 7, file) == 7);
        fclose(file);
    }
    /* Exclusive: a second writer is refused. */
    CHECK((file = vkd3d_file_open_exclusive_write(a)) == NULL);
    if (file)
        fclose(file);
    CHECK(vkd3d_file_rename_no_replace(a, b));
    CHECK(!vkd3d_file_rename_no_replace(b, b));
    memset(&mapped, 0, sizeof(mapped));
    CHECK(vkd3d_file_map_read_only(b, &mapped) && mapped.mapped_size == 7 && !memcmp(mapped.mapped, "archive", 7));
    /* The read archive goes while it is mapped (FILE_SHARE_DELETE), as a cache CLEAR needs. */
    CHECK(vkd3d_file_delete(b));
    vkd3d_file_unmap(&mapped);

    CHECK((file = vkd3d_file_open(a, "wb")) != NULL);
    if (file)
    {
        CHECK(fwrite("driver", 1, 6, file) == 6);
        fclose(file);
    }
    CHECK((file = vkd3d_file_open(b, "wb")) != NULL);
    if (file)
        fclose(file);
    CHECK(vkd3d_file_rename_overwrite(a, b));
    CHECK((file = vkd3d_file_open(b, "rb")) != NULL);
    if (file)
    {
        char text[8] = {0};
        CHECK(fread(text, 1, sizeof(text), file) == 6 && !memcmp(text, "driver", 6));
        fclose(file);
    }
    CHECK(vkd3d_file_delete(b));
    CHECK(!vkd3d_file_delete(b));
    CHECK(RemoveDirectoryW(dir));
}

int main(void)
{
    test_group_keys();
    test_trim();
    test_accounts();
    test_files();
    printf(failures ? "FAILED: %u checks\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
