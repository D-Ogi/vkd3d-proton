/*
 * Host test of the Windows engine detection and program name of libs/vkd3d-common/platform.c.
 *
 * The test executable carries a version resource with the product name "Unreal Engine" and the product version
 * 5.3.2 (vkd3d_engine_version_test.rc), and no CrashReportClient.exe sits near it. So vkd3d_get_engine_version must
 * find Unreal Engine 5.3.2 from the executable itself, and keep it: the later probes must not overwrite that answer.
 * The long-path case runs a copy of the test from a directory deeper than MAX_PATH, where both functions must still
 * read the executable; it is skipped when the host cannot start a process from there.
 *
 * Build (in a Visual Studio developer environment, either architecture):
 *   rc /fo test.res tests\vkd3d_engine_version_test.rc
 *   clang-cl /W3 -D_CRT_SECURE_NO_WARNINGS /Iinclude /Iinclude\private tests\vkd3d_engine_version_test.c
 *            libs\vkd3d-common\platform.c libs\vkd3d-common\debug.c test.res version.lib pathcch.lib
 *   (add --target=i686-pc-windows-msvc for the 32-bit image)
 * Usage: vkd3d_engine_version_test [--negative-control]
 * The negative control inverts every expectation; it must fail every case.
 */
#include "vkd3d_platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures, checks;
static bool invert;

#define CHECK(cond) \
    do { \
        checks++; \
        if (!(cond) != invert) \
        { \
            failures++; \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
        } \
    } while (0)

static bool engine_is_ue_5_3_2(void)
{
    uint32_t major = 0, minor = 0, patch = 0;
    enum vkd3d_application_engine_class engine = vkd3d_get_engine_version(&major, &minor, &patch);
    printf("engine class %d version %u.%u.%u\n", (int)engine, major, minor, patch);
    return engine == VKD3D_APPLICATION_ENGINE_CLASS_UNREAL_ENGINE && major == 5 && minor == 3 && patch == 2;
}

static bool program_name_is_test(void)
{
    char name[VKD3D_PATH_MAX];
    return vkd3d_get_program_name(name) && !_stricmp(name, "vkd3d_engine_version_test.exe");
}

static void test_engine_from_exe(void)
{
    CHECK(engine_is_ue_5_3_2());
}

static void test_program_name(void)
{
    CHECK(program_name_is_test());
}

static void test_long_path(void)
{
    static WCHAR self[32768], dir[32768], copy[32768], cmd[33000];
    DWORD n = GetModuleFileNameW(NULL, self, 32768);
    WCHAR *slash = n && n < 32768 ? wcsrchr(self, L'\\') : NULL;
    size_t base;
    unsigned depth = 0;
    BOOL copied;

    CHECK(slash != NULL);
    if (!slash)
        return;
    _snwprintf(dir, 32768, L"\\\\?\\%.*ls\\engine_test_long_path", (int)(slash - self), self);
    base = wcslen(dir);
    CreateDirectoryW(dir, NULL);
    while (wcslen(dir) < base + MAX_PATH)
    {
        wcscat(dir, L"\\directory_name_of_forty_characters_long");
        CreateDirectoryW(dir, NULL);
        depth++;
    }
    _snwprintf(copy, 32768, L"%ls\\vkd3d_engine_version_test.exe", dir);
    _snwprintf(cmd, 33000, L"\"%ls\" --long-path-child", copy);
    copied = CopyFileW(self, copy, FALSE);
    CHECK(copied);
    if (copied)
    {
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(copy, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            DWORD code = 1;
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CHECK(code == 0);
        }
        else
        {
            printf("SKIP long_path: CreateProcessW error %lu\n", GetLastError());
        }
        DeleteFileW(copy);
    }
    for (; depth > 0; depth--)
    {
        RemoveDirectoryW(dir);
        *wcsrchr(dir, L'\\') = 0;
    }
    RemoveDirectoryW(dir);
}

static const struct
{
    const char *name;
    void (*fn)(void);
}
tests[] =
{
    {"engine_from_exe", test_engine_from_exe},
    {"program_name", test_program_name},
    {"long_path", test_long_path},
};

int main(int argc, char **argv)
{
    int failed_tests = 0;
    unsigned i;

    if (argc > 1 && !strcmp(argv[1], "--long-path-child"))
    {
        WCHAR path[32768];
        DWORD n = GetModuleFileNameW(NULL, path, 32768);
        bool ok = n > MAX_PATH && engine_is_ue_5_3_2() && program_name_is_test();
        printf("child: path length %lu, %s\n", (unsigned long)n, ok ? "ok" : "wrong");
        return ok ? 0 : 1;
    }
    invert = argc > 1 && !strcmp(argv[1], "--negative-control");
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        int before = failures;
        bool ok;
        tests[i].fn();
        ok = failures == before;
        failed_tests += !ok;
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
    }
    printf("engine version: %u tests, %d checks, %d failed checks, %u-bit image%s\n",
            (unsigned)(sizeof(tests) / sizeof(tests[0])), checks, failures, (unsigned)(sizeof(void *) * 8),
            invert ? " (negative control)" : "");
    return failed_tests ? 1 : 0;
}
