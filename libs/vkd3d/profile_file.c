/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: the machine profile file of vkd3d-proton (profile_file.h). This file does not log: the caller in
 * device_workarounds.c logs the result.
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#endif

#include "profile_file.h"
#include "vkd3d_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The flag identifiers, as the application_override table names them. */
static const struct vkd3d_debug_option vkd3d_profile_flag_names[] =
{
#define VKD3D_DECL_CONFIG(name, flag) { #flag, VKD3D_CONFIG_FLAG_STATIC(flag) },
#define VKD3D_DECL_CONFIG_PLAIN(flag) { #flag, VKD3D_CONFIG_FLAG_STATIC(flag) },
#include "config_flag_decl.h"
#undef VKD3D_DECL_CONFIG
#undef VKD3D_DECL_CONFIG_PLAIN
};

#define VKD3D_PROFILE_NAME_MAX 260

static void vkd3d_profile_trim(const char **s, size_t *n)
{
    while (*n && (**s == ' ' || **s == '\t'))
        ++*s, --*n;
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t'))
        --*n;
}

static bool vkd3d_profile_same(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && !memcmp(s, word, n);
}

static bool vkd3d_profile_lookup(const char *s, size_t n, const struct vkd3d_debug_option *options,
        unsigned int option_count, union vkd3d_config_flags *flag)
{
    unsigned int i;

    for (i = 0; i < option_count; i++)
    {
        if (vkd3d_profile_same(s, n, options[i].name))
        {
            *flag = options[i].flag;
            return true;
        }
    }
    for (i = 0; i < ARRAY_SIZE(vkd3d_profile_flag_names); i++)
    {
        if (vkd3d_profile_same(s, n, vkd3d_profile_flag_names[i].name))
        {
            *flag = vkd3d_profile_flag_names[i].flag;
            return true;
        }
    }
    return false;
}

static bool vkd3d_profile_fail(char *error, size_t error_size, unsigned int line, const char *reason,
        const char *s, size_t n)
{
    if (s)
        snprintf(error, error_size, "line %u: %s '%.*s'", line, reason, (int)(n > 64 ? 64 : n), s);
    else
        snprintf(error, error_size, "line %u: %s", line, reason);
    return false;
}

/* One pass over the text. The first pass (apply false) only checks it; the second applies the matching sections. */
static bool vkd3d_profile_pass(const char *text, size_t size, const char *app, const char *appid,
        const struct vkd3d_debug_option *options, unsigned int option_count, bool apply,
        union vkd3d_config_flags *flags, unsigned int *matched, vkd3d_profile_entry_cb cb, void *cb_data,
        char *error, size_t error_size)
{
    static const struct
    {
        const char *prefix;
        enum vkd3d_string_compare_mode mode;
    }
    modes[] =
    {
        { "starts_with:", VKD3D_STRING_COMPARE_STARTS_WITH },
        { "ends_with:", VKD3D_STRING_COMPARE_ENDS_WITH },
        { "appid:", VKD3D_STRING_COMPARE_APPID },
    };
    char section[VKD3D_PROFILE_NAME_MAX + 16], name[VKD3D_PROFILE_NAME_MAX], item[64];
    const char *p = text, *end = text + size;
    bool in_section = false, match = false;
    unsigned int line = 0;
    size_t i;

    if (size >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
        p += 3;

    while (p < end)
    {
        const char *eol = memchr(p, '\n', end - p);
        const char *s = p, *key, *value, *hash;
        size_t n = (eol ? eol : end) - p, key_n, value_n;
        bool add;

        p = eol ? eol + 1 : end;
        line++;
        if (n && s[n - 1] == '\r')
            n--;
        for (i = 0; i < n; i++)
        {
            unsigned char c = s[i];
            if ((c < 0x20 && c != '\t') || c >= 0x7f)
            {
                snprintf(error, error_size, "line %u: a byte 0x%02x that is not printable ASCII", line, c);
                return false;
            }
        }
        vkd3d_profile_trim(&s, &n);
        if (!n || s[0] == '#')
            continue;

        if (s[0] == '[')
        {
            enum vkd3d_string_compare_mode mode = VKD3D_STRING_COMPARE_EXACT;
            const char *body = s + 1;
            size_t body_n = n - 2;

            if (n < 2 || s[n - 1] != ']')
                return vkd3d_profile_fail(error, error_size, line, "a section header must end with ']':", s, n);
            for (i = 0; i < ARRAY_SIZE(modes); i++)
            {
                size_t length = strlen(modes[i].prefix);
                if (body_n >= length && !memcmp(body, modes[i].prefix, length))
                {
                    mode = modes[i].mode;
                    body += length;
                    body_n -= length;
                    break;
                }
            }
            if (!body_n)
                return vkd3d_profile_fail(error, error_size, line, "a section without a name:", s, n);
            if (body_n >= sizeof(name))
                return vkd3d_profile_fail(error, error_size, line, "a section name that is too long:", s, n);
            if (body[0] == ' ' || body[0] == '\t' || body[body_n - 1] == ' ' || body[body_n - 1] == '\t')
                return vkd3d_profile_fail(error, error_size, line,
                        "a section name that starts or ends with a space:", s, n);
            for (i = 0; i < body_n; i++)
            {
                if (strchr("[]\\/:*?\"<>|", body[i]))
                    return vkd3d_profile_fail(error, error_size, line,
                            "a section name must be a file name, without a path or an unknown prefix:", s, n);
                if (mode == VKD3D_STRING_COMPARE_APPID && (body[i] < '0' || body[i] > '9'))
                    return vkd3d_profile_fail(error, error_size, line, "an appid must be a number:", s, n);
            }
            memcpy(name, body, body_n);
            name[body_n] = '\0';
            memcpy(section, s, n);
            section[n] = '\0';
            if (mode == VKD3D_STRING_COMPARE_APPID)
                match = *appid && vkd3d_string_compare(VKD3D_STRING_COMPARE_EXACT, appid, name);
            else
                match = vkd3d_string_compare(mode, app, name);
            if (apply && match)
                ++*matched;
            in_section = true;
            continue;
        }

        if (!(key = memchr(s, '=', n)))
            return vkd3d_profile_fail(error, error_size, line,
                    "a line must be a comment, a [section] or 'add = ...' or 'remove = ...':", s, n);
        value = key + 1;
        value_n = s + n - value;
        key_n = key - s;
        key = s;
        vkd3d_profile_trim(&key, &key_n);
        if ((hash = memchr(value, '#', value_n)))
            value_n = hash - value;
        vkd3d_profile_trim(&value, &value_n);

        if (vkd3d_profile_same(key, key_n, "add"))
            add = true;
        else if (vkd3d_profile_same(key, key_n, "remove"))
            add = false;
        else
            return vkd3d_profile_fail(error, error_size, line, "an unknown key (only add and remove):", key, key_n);
        if (!in_section)
            return vkd3d_profile_fail(error, error_size, line,
                    "an option outside a section would apply to every application:", s, n);
        if (!value_n)
            return vkd3d_profile_fail(error, error_size, line, "no config name after the key:", s, n);

        while (value_n)
        {
            const char *comma = memchr(value, ',', value_n);
            const char *word = value;
            size_t word_n = comma ? (size_t)(comma - value) : value_n;
            union vkd3d_config_flags flag;

            value_n -= comma ? word_n + 1 : word_n;
            value += comma ? word_n + 1 : word_n;
            vkd3d_profile_trim(&word, &word_n);
            if (!word_n)
                return vkd3d_profile_fail(error, error_size, line, "an empty config name in the list:", s, n);
            for (i = 0; i < word_n; i++)
            {
                char c = word[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    return vkd3d_profile_fail(error, error_size, line, "a config name with a bad character:",
                            word, word_n);
            }
            if (word_n >= sizeof(item) || !vkd3d_profile_lookup(word, word_n, options, option_count, &flag))
                return vkd3d_profile_fail(error, error_size, line, "an unknown config name:", word, word_n);
            if (comma && !value_n)
                return vkd3d_profile_fail(error, error_size, line, "an empty config name in the list:", s, n);

            if (apply && match)
            {
                if (add)
                    vkd3d_config_flag_add(flags, flag);
                else
                    for (i = 0; i < ARRAY_SIZE(flags->words); i++)
                        flags->words[i] &= ~flag.words[i];
                if (cb)
                {
                    memcpy(item, word, word_n);
                    item[word_n] = '\0';
                    cb(cb_data, line, section, add, item);
                }
            }
        }
    }
    return true;
}

bool vkd3d_profile_parse(const char *text, size_t size, const char *app, const char *appid,
        const struct vkd3d_debug_option *options, unsigned int option_count, union vkd3d_config_flags *flags,
        unsigned int *matched, vkd3d_profile_entry_cb cb, void *cb_data, char *error, size_t error_size)
{
    if (error_size)
        error[0] = '\0';
    *matched = 0;
    if (memchr(text, '\0', size))
    {
        snprintf(error, error_size, "the file holds a NUL byte");
        return false;
    }
    if (!vkd3d_profile_pass(text, size, app, appid, options, option_count, false, flags, matched, NULL, NULL,
            error, error_size))
        return false;
    return vkd3d_profile_pass(text, size, app, appid, options, option_count, true, flags, matched, cb, cb_data,
            error, error_size);
}

/* ---- the profile directory (Windows) ------------------------------------------------------------------------- */

#ifdef _WIN32

#ifdef VKD3D_PROFILE_FILE_TEST
const wchar_t *vkd3d_profile_test_root;
bool vkd3d_profile_test_trust_all;
#endif

/* SYSTEM, Administrators and TrustedInstaller. */
static bool vkd3d_profile_trusted_sid(PSID sid)
{
    static const struct
    {
        BYTE revision, count;
        SID_IDENTIFIER_AUTHORITY authority;
        DWORD sub[6];
    }
    trusted_installer = { SID_REVISION, 6, { SECURITY_NT_AUTHORITY },
            { 80, 956008885, 3418522649u, 1831038044, 1853292631, 2271478464u } };

    return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
            EqualSid(sid, (PSID)&trusted_installer);
}

static void vkd3d_profile_sid_text(PSID sid, char *out, size_t size)
{
    char *text = NULL;

    if (ConvertSidToStringSidA(sid, &text))
    {
        snprintf(out, size, "%s", text);
        LocalFree(text);
    }
    else
        snprintf(out, size, "?");
}

bool vkd3d_profile_trusted_descriptor(void *security_descriptor, char *why, size_t why_size)
{
    /* The rights that change the content of a file or the entries of a directory, or the access rules. */
    const DWORD write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
            GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    PSECURITY_DESCRIPTOR sd = security_descriptor;
    BOOL defaulted = FALSE, present = FALSE;
    ACCESS_ALLOWED_ACE *allowed;
    PSID owner = NULL, who;
    ACE_HEADER *ace;
    PACL dacl = NULL;
    char sid[96];
    DWORD i;

    if (!GetSecurityDescriptorOwner(sd, &owner, &defaulted) || !owner)
    {
        snprintf(why, why_size, "it has no owner");
        return false;
    }
    if (!vkd3d_profile_trusted_sid(owner))
    {
        vkd3d_profile_sid_text(owner, sid, sizeof(sid));
        snprintf(why, why_size, "its owner %s is not SYSTEM, Administrators or TrustedInstaller", sid);
        return false;
    }
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl)
    {
        snprintf(why, why_size, "it has no access list, so every account may change it");
        return false;
    }
    for (i = 0; i < dacl->AceCount; i++)
    {
        if (!GetAce(dacl, i, (void **)&ace))
        {
            snprintf(why, why_size, "its access list cannot be read");
            return false;
        }
        if (ace->AceFlags & INHERIT_ONLY_ACE)
            continue;
        if (ace->AceType == ACCESS_DENIED_ACE_TYPE || ace->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE)
            continue;
        if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE && ace->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
        {
            if (ace->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || ace->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE)
            {
                snprintf(why, why_size, "its access list has an object access entry");
                return false;
            }
            continue;  /* audit and label entries give no access */
        }
        /* ACCESS_ALLOWED_CALLBACK_ACE starts as ACCESS_ALLOWED_ACE does. */
        allowed = (ACCESS_ALLOWED_ACE *)ace;
        who = (PSID)&allowed->SidStart;
        if (!(allowed->Mask & write) || vkd3d_profile_trusted_sid(who) ||
                IsWellKnownSid(who, WinCreatorOwnerRightsSid) || IsWellKnownSid(who, WinCreatorOwnerSid))
            continue;
        vkd3d_profile_sid_text(who, sid, sizeof(sid));
        snprintf(why, why_size, "it gives write access to %s", sid);
        return false;
    }
    return true;
}

static bool vkd3d_profile_trusted_handle(HANDLE handle, char *why, size_t why_size)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD status;
    bool ok;

#ifdef VKD3D_PROFILE_FILE_TEST
    if (vkd3d_profile_test_trust_all)
        return true;
#endif
    status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            NULL, NULL, NULL, NULL, &sd);
    if (status != ERROR_SUCCESS)
    {
        snprintf(why, why_size, "its access rules cannot be read (error %lu)", (unsigned long)status);
        return false;
    }
    ok = vkd3d_profile_trusted_descriptor(sd, why, why_size);
    LocalFree(sd);
    return ok;
}

/* The profile root, "<system drive>\ProgramData\amdgpu-wddm\profiles". Microsoft does not support another place
 * for ProgramData, and the drive comes from the system, not from the environment of the process. */
static bool vkd3d_profile_root(wchar_t *out, size_t size)
{
    wchar_t windows[MAX_PATH];
    UINT length;
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    DWORD n;
#endif

#ifdef VKD3D_PROFILE_FILE_TEST
    if (vkd3d_profile_test_root)
        return swprintf(out, size, L"%ls", vkd3d_profile_test_root) > 0;
#endif
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    /* BD-167: only a lab build takes the root from the environment. */
    n = GetEnvironmentVariableW(L"AMDGPU_WDDM_PROFILE_DIR", out, (DWORD)size);
    if (n && n < size)
        return true;
#endif
    length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH || windows[1] != L':')
        return false;
    return swprintf(out, size, L"%lc:\\ProgramData\\amdgpu-wddm\\profiles", windows[0]) > 0;
}

/* Opens a path of the profile and checks it. 0: accepted, 1: absent, -1: refused (why set). */
static int vkd3d_profile_open(const wchar_t *path, bool directory, HANDLE *out, DWORD *file_size,
        char *why, size_t why_size)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle;
    DWORD error;

    handle = CreateFileW(path, directory ? READ_CONTROL | FILE_READ_ATTRIBUTES : GENERIC_READ | READ_CONTROL,
            directory ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return 1;
        snprintf(why, why_size, "it cannot be opened (error %lu)", (unsigned long)error);
        return -1;
    }
    if (!GetFileInformationByHandle(handle, &info))
        snprintf(why, why_size, "its attributes cannot be read");
    else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        snprintf(why, why_size, "it is a reparse point (a link or a junction)");
    else if (directory && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        snprintf(why, why_size, "it is not a directory");
    else if (!directory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        snprintf(why, why_size, "it is not a regular file");
    else if (!directory && (info.nFileSizeHigh || info.nFileSizeLow > VKD3D_PROFILE_FILE_MAX_SIZE))
        snprintf(why, why_size, "it is larger than %u bytes", (unsigned int)VKD3D_PROFILE_FILE_MAX_SIZE);
    else if (vkd3d_profile_trusted_handle(handle, why, why_size))
    {
        *out = handle;
        *file_size = info.nFileSizeLow;
        return 0;
    }
    CloseHandle(handle);
    return -1;
}

char *vkd3d_profile_file_read(char *name, size_t name_size, size_t *size, char *why, size_t why_size)
{
    wchar_t root[MAX_PATH], path[MAX_PATH];
    DWORD length = 0, done = 0;
    HANDLE handle;
    char *text;
    int status;

    if (why_size)
        why[0] = '\0';
    if (name_size)
        name[0] = '\0';
    if (!vkd3d_profile_root(root, MAX_PATH) ||
            swprintf(path, MAX_PATH, L"%ls\\vkd3d-proton.conf", root) <= 0)
        return NULL;

    if (!WideCharToMultiByte(CP_UTF8, 0, root, -1, name, (int)name_size, NULL, NULL))
        snprintf(name, name_size, "?");
    if ((status = vkd3d_profile_open(root, true, &handle, &length, why, why_size)))
        return NULL;
    CloseHandle(handle);

    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, name, (int)name_size, NULL, NULL))
        snprintf(name, name_size, "?");
    if ((status = vkd3d_profile_open(path, false, &handle, &length, why, why_size)))
        return NULL;

    if (!(text = malloc((size_t)length + 1)))
        snprintf(why, why_size, "out of memory");
    else if (length && (!ReadFile(handle, text, length, &done, NULL) || done != length))
    {
        snprintf(why, why_size, "it cannot be read");
        free(text);
        text = NULL;
    }
    CloseHandle(handle);
    if (!text)
        return NULL;
    text[length] = '\0';
    *size = length;
    return text;
}

#endif
