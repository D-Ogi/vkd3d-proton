/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: host test of the machine profile file (profile_file.c). Built and run by profile_file_test.py.
 *   1. what the parser reads and what it refuses, and that a refused file changes no flag;
 *   2. a section applies to its program only, in the compare modes of the application_override table;
 *   3. the order: the file applies after the built-in table, so it can remove a flag that the table added;
 *   4. the file of a directory, and the access rule: a directory that the test user owns is refused.
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include <windows.h>
#include <sddl.h>

#include "profile_file.h"

#include <stdio.h>
#include <string.h>

/* As vkd3d_config_options of device.c. */
static const struct vkd3d_debug_option options[] =
{
#define VKD3D_DECL_CONFIG(name, flag) { name, VKD3D_CONFIG_FLAG_STATIC(flag) },
#define VKD3D_DECL_CONFIG_PLAIN(flag)
#include "config_flag_decl.h"
#undef VKD3D_DECL_CONFIG
#undef VKD3D_DECL_CONFIG_PLAIN
};

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, what); } } while (0)

static bool has(union vkd3d_config_flags flags, union vkd3d_config_flags flag)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(flags.words); i++)
        if (flag.words[i] && (flags.words[i] & flag.words[i]) != flag.words[i])
            return false;
    return true;
}

static bool empty(union vkd3d_config_flags flags)
{
    return !vkd3d_config_flag_is_nonzero(flags);
}

static void log_entry(void *data, unsigned int line, const char *section, bool add, const char *name)
{
    unsigned int *count = data;
    ++*count;
    printf("  line %u: %s %s %s\n", line, section, add ? "add" : "remove", name);
}

static bool parse(const char *text, const char *app, const char *appid, union vkd3d_config_flags *flags,
        unsigned int *matched, char *error)
{
    unsigned int entries = 0;
    return vkd3d_profile_parse(text, strlen(text), app, appid, options, ARRAY_SIZE(options), flags, matched,
            log_entry, &entries, error, 256);
}

static void test_parser(void)
{
    static const char good[] =
        "\xef\xbb\xbf# vkd3d-proton profiles\r\n"
        "\r\n"
        "[game.exe]\r\n"
        "add = FORCE_RAW_VA_CBV, no_upload_hvv   # comment\r\n"
        "  add=FORCE_MINIMUM_SUBGROUP_SIZE\r\n"
        "[other.exe]\r\n"
        "add = single_queue\r\n"
        "[ends_with:-Win64-Shipping.exe]\r\n"
        "add = force_static_cbv\r\n"
        "[starts_with:Halo]\r\n"
        "add = DXR\r\n"
        "[appid:1245620]\r\n"
        "add = nodxr\r\n";
    union vkd3d_config_flags flags = VKD3D_CONFIG_FLAGS_NONE;
    unsigned int matched;
    char error[256];

    CHECK(parse(good, "game.exe", "", &flags, &matched, error), error);
    CHECK(matched == 1, "one section matches game.exe");
    CHECK(has(flags, VKD3D_CONFIG_FLAG(FORCE_RAW_VA_CBV)), "a flag identifier");
    CHECK(has(flags, VKD3D_CONFIG_FLAG(NO_UPLOAD_HVV)), "an option name");
    CHECK(has(flags, VKD3D_CONFIG_FLAG(FORCE_MINIMUM_SUBGROUP_SIZE)), "a flag without an option name");
    CHECK(!vkd3d_config_flag_overlaps(flags, VKD3D_CONFIG_FLAG(SINGLE_QUEUE)), "not the flag of another program");

    flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAGS_NONE;
    CHECK(parse(good, "Game.exe", "", &flags, &matched, error) && !matched && empty(flags),
            "the name compares the case, as VKD3D_STRING_COMPARE_EXACT does");
    CHECK(parse(good, "third.exe", "", &flags, &matched, error) && !matched && empty(flags), "no section");
    CHECK(parse(good, "Foo-Win64-Shipping.exe", "", &flags, &matched, error) && matched == 1 &&
            has(flags, VKD3D_CONFIG_FLAG(FORCE_STATIC_CBV)), "ends_with");
    flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAGS_NONE;
    CHECK(parse(good, "HaloInfinite.exe", "", &flags, &matched, error) && matched == 1 &&
            has(flags, VKD3D_CONFIG_FLAG(DXR)), "starts_with");
    flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAGS_NONE;
    CHECK(parse(good, "eldenring.exe", "1245620", &flags, &matched, error) && matched == 1 &&
            has(flags, VKD3D_CONFIG_FLAG(NO_DXR)), "appid");
    flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAGS_NONE;
    CHECK(parse(good, "1245620", "", &flags, &matched, error) && !matched && empty(flags),
            "an appid section does not compare the program name");
}

static void test_order(void)
{
    /* As vkd3d_instance_apply_application_workarounds: the built-in table first, then the file. */
    static const char text[] =
        "[Wukong-Win64-Shipping.exe]\n"
        "remove = small_vram_rebar\n"
        "add = single_queue\n"
        "[ends_with:-Shipping.exe]\n"
        "add = SMALL_VRAM_REBAR\n"
        "remove = single_queue\n"
        "[Wukong-Win64-Shipping.exe]\n"
        "remove = SMALL_VRAM_REBAR\n";
    union vkd3d_config_flags flags = VKD3D_CONFIG_FLAG_INIT(.SMALL_VRAM_REBAR = 1, .NO_STAGGERED_SUBMIT = 1);
    unsigned int matched;
    char error[256];

    CHECK(parse(text, "Wukong-Win64-Shipping.exe", "", &flags, &matched, error), error);
    CHECK(matched == 3, "three sections match");
    CHECK(!vkd3d_config_flag_overlaps(flags, VKD3D_CONFIG_FLAG(SMALL_VRAM_REBAR)),
            "the file removes a flag of the built-in table, and the last line wins");
    CHECK(!vkd3d_config_flag_overlaps(flags, VKD3D_CONFIG_FLAG(SINGLE_QUEUE)), "lines apply in file order");
    CHECK(has(flags, VKD3D_CONFIG_FLAG(NO_STAGGERED_SUBMIT)), "a flag that the file does not name stays");
}

static void test_refused(void)
{
    static const char *const bad[] =
    {
        "add = single_queue\n",                          /* outside a section: every application */
        "[game.exe\nadd = single_queue\n",               /* unclosed header */
        "[]\nadd = single_queue\n",                      /* empty name */
        "[ends_with:]\nadd = single_queue\n",            /* empty name after a prefix */
        "[C:\\Games\\game.exe]\nadd = single_queue\n",   /* a path */
        "[contains:game]\nadd = single_queue\n",         /* an unknown prefix */
        "[ game.exe]\nadd = single_queue\n",             /* a space at the start */
        "[appid:12a]\nadd = single_queue\n",             /* not a number */
        "[game.exe]\nadd single_queue\n",                /* no '=' */
        "[game.exe]\nset = single_queue\n",              /* an unknown key */
        "[game.exe]\nadd =\n",                           /* no name */
        "[game.exe]\nadd = single_queue,,dxr\n",         /* an empty name in the list */
        "[game.exe]\nadd = single_queue,\n",             /* a list that ends with a comma */
        "[game.exe]\nadd = single-queue\n",              /* a bad character */
        "[game.exe]\nadd = not_an_option\n",             /* an unknown name */
        "[game.exe]\nadd = single_queue dxr\n",          /* two names without a comma */
        "[game.exe]\n;comment\n",                        /* not a comment of this format */
        "[g\xc3\xa4me.exe]\nadd = single_queue\n",       /* not ASCII */
    };
    union vkd3d_config_flags flags;
    unsigned int matched, i;
    char error[256];

    for (i = 0; i < ARRAY_SIZE(bad); i++)
    {
        flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAG_INIT(.NO_STAGGERED_SUBMIT = 1);
        CHECK(!parse(bad[i], "game.exe", "", &flags, &matched, error) && *error, bad[i]);
        CHECK(!matched && !vkd3d_config_flag_overlaps(flags, VKD3D_CONFIG_FLAG(SINGLE_QUEUE)) &&
                has(flags, VKD3D_CONFIG_FLAG(NO_STAGGERED_SUBMIT)), "a refused file changes no flag");
        printf("  refused: %s\n", error);
    }

    /* The refusal names the line, and a valid section before the error gives nothing. */
    flags = (union vkd3d_config_flags)VKD3D_CONFIG_FLAGS_NONE;
    CHECK(!parse("[game.exe]\nadd = single_queue\nbroken line\n", "game.exe", "", &flags, &matched, error) &&
            !strncmp(error, "line 3:", 7) && empty(flags), error);
    {
        static const char nul[] = "[game.exe]\nadd = single_queue\n\0\n";
        unsigned int entries = 0;
        CHECK(!vkd3d_profile_parse(nul, sizeof(nul) - 1, "game.exe", "", options, ARRAY_SIZE(options), &flags,
                &matched, log_entry, &entries, error, sizeof(error)) && empty(flags) && !entries, "a NUL byte");
    }
}

static int trusted(const char *sddl)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    char why[256];
    bool ok;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL))
        return -1;
    ok = vkd3d_profile_trusted_descriptor(sd, why, sizeof(why));
    LocalFree(sd);
    return ok;
}

static void test_files(const wchar_t *root)
{
    char name[MAX_PATH * 3], why[256];
    wchar_t path[MAX_PATH], absent[MAX_PATH];
    size_t size = 0;
    char *text;
    FILE *f;

    CreateDirectoryW(root, NULL);
    swprintf(path, MAX_PATH, L"%ls\\vkd3d-proton.conf", root);
    CHECK((f = _wfopen(path, L"wb")) != NULL, "write the test file");
    if (!f)
        return;
    fputs("[game.exe]\r\nadd = single_queue\r\n", f);
    fclose(f);

    vkd3d_profile_test_root = root;
    vkd3d_profile_test_trust_all = true;
    text = vkd3d_profile_file_read(name, sizeof(name), &size, why, sizeof(why));
    CHECK(text && size == strlen("[game.exe]\r\nadd = single_queue\r\n") && !*why, "the file of the directory");
    CHECK(strstr(name, "vkd3d-proton.conf") != NULL, "the name of the file");
    free(text);

    vkd3d_profile_test_trust_all = false;
    text = vkd3d_profile_file_read(name, sizeof(name), &size, why, sizeof(why));
    CHECK(!text && strstr(why, "owner"), "a directory of the test user is refused");
    printf("  refused: %s: %s\n", name, why);

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    /* Only the lab flavour takes the directory from the environment. */
    vkd3d_profile_test_root = NULL;
    vkd3d_profile_test_trust_all = true;
    SetEnvironmentVariableW(L"AMDGPU_WDDM_PROFILE_DIR", root);
    text = vkd3d_profile_file_read(name, sizeof(name), &size, why, sizeof(why));
    SetEnvironmentVariableW(L"AMDGPU_WDDM_PROFILE_DIR", NULL);
    CHECK(text && !*why, "the lab flavour reads the directory of the environment");
    free(text);
#endif

    swprintf(absent, MAX_PATH, L"%ls\\absent", root);
    vkd3d_profile_test_root = absent;
    vkd3d_profile_test_trust_all = true;
    text = vkd3d_profile_file_read(name, sizeof(name), &size, why, sizeof(why));
    CHECK(!text && !*why, "no directory: no file and no reason");
    vkd3d_profile_test_root = root;
    DeleteFileW(path);
    text = vkd3d_profile_file_read(name, sizeof(name), &size, why, sizeof(why));
    CHECK(!text && !*why, "no file: no reason");
    vkd3d_profile_test_root = NULL;
    vkd3d_profile_test_trust_all = false;

    CHECK(trusted("O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)"
            "(A;OICI;0x1200a9;;;S-1-15-2-2)") == 1, "the rule of the installer");
    CHECK(trusted("O:BAG:SYD:(A;OICIIO;FA;;;CO)(A;;FA;;;OW)(D;;FA;;;BU)(A;;FA;;;BA)") == 1,
            "templates and deny entries");
    CHECK(trusted("O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SYD:(A;;FA;;;SY)") == 1,
            "TrustedInstaller as owner");
    CHECK(trusted("O:BUG:SYD:(A;;FA;;;BA)") == 0, "a user owner");
    CHECK(trusted("O:BAG:SYD:(A;;FA;;;BA)(A;;FRFW;;;BU)") == 0, "users may write");
    CHECK(trusted("O:BAG:SYD:NO_ACCESS_CONTROL") == 0, "no access list");
    CHECK(trusted("O:BAG:SYD:(A;CI;0x4;;;BU)") == 0, "users may add folders");
}

int main(int argc, char **argv)
{
    wchar_t root[MAX_PATH];

    if (argc != 2)
    {
        printf("usage: profile_file_test <empty test directory>\n");
        return 2;
    }
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, root, MAX_PATH);
    test_parser();
    test_order();
    test_refused();
    test_files(root);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
