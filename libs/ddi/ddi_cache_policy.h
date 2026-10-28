/* SPDX-License-Identifier: MIT
 *
 * amdgpu_wddm_vkd3d.dll: the decisions about the per-user disk cache that need no file and no device, so that a host
 * test (cache_policy_test.c) can check them. ddi_engine.c applies them to %LOCALAPPDATA%\amdgpu-wddm\vkd3d.
 */

#ifndef __BC250_DDI_CACHE_POLICY_H
#define __BC250_DDI_CACHE_POLICY_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The budget of the whole directory. The archives of the identifier route hold names and pipeline descriptions only,
 * a few MiB per game; the route with full SPIR-V and a driver cache file (up to 512 MiB, VKD3D_DRIVER_CACHE_MAX_SIZE)
 * per program needs more. 1 GiB is the bound of Mesa's own disk cache, which holds the binaries. */
#define BC250_CACHE_BUDGET_BYTES (1024ull << 20)
/* The most files the trim looks at in one process start. */
#define BC250_CACHE_MAX_FILES 4096u

/* Whether hosted RADV keeps a disk cache for an account with this SID: Mesa's rule, src/util/disk_cache_os.c
 * disk_cache_account_has_default_dir. A local or domain account (S-1-5-21-...) or a Microsoft Entra ID account
 * (S-1-12-1-...); not LocalSystem, the services, DWM or the font driver host. */
static inline bool bc250_sid_has_driver_cache(const SID *s)
{
    static const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    static const SID_IDENTIFIER_AUTHORITY entra_authority = { { 0, 0, 0, 0, 0, 12 } };

    if (!s || s->Revision != SID_REVISION || s->SubAuthorityCount < 1)
        return false;
    if (!memcmp(&s->IdentifierAuthority, &nt_authority, sizeof(nt_authority)))
        return s->SubAuthority[0] == SECURITY_NT_NON_UNIQUE;
    if (!memcmp(&s->IdentifierAuthority, &entra_authority, sizeof(entra_authority)))
        return s->SubAuthority[0] == 1;
    return false;
}

/* A true value of Mesa's boolean option parser (src/util/u_debug.c debug_parse_bool_option): "1", "y", "yes" and
 * "true" in any case. MESA_SHADER_CACHE_DISABLE with such a value turns RADV's disk cache off. */
static inline bool bc250_option_is_true(const wchar_t *value)
{
    return value && (!_wcsicmp(value, L"1") || !_wcsicmp(value, L"y") || !_wcsicmp(value, L"yes") ||
            !_wcsicmp(value, L"true"));
}

/* The engine's cache files are vkd3d-proton.<program>.cache followed by nothing, ".write", ".merge", ".driver",
 * ".driver.<pid>.<n>.tmp" (a save in flight, or left by a killed process) or ".pso-log.txt" (lab flavour). Returns the
 * length of the group key "vkd3d-proton.<program>.cache" in name, or 0 when name is no such file. */
static inline size_t bc250_cache_group_key_length(const wchar_t *name)
{
    static const wchar_t prefix[] = L"vkd3d-proton.";
    static const wchar_t *const suffixes[] = { L"", L".write", L".merge", L".driver", L".pso-log.txt" };
    static const wchar_t cache[] = L".cache";
    const size_t prefix_length = ARRAYSIZE(prefix) - 1, cache_length = ARRAYSIZE(cache) - 1;
    size_t length, i, key, suffix_length;
    const wchar_t *tail;

    if (!name || wcsncmp(name, prefix, prefix_length))
        return 0;
    length = wcslen(name);
    for (i = 0; i < ARRAYSIZE(suffixes); ++i)
    {
        suffix_length = wcslen(suffixes[i]);
        if (length < prefix_length + 1 + cache_length + suffix_length)
            continue;
        key = length - suffix_length;
        if (!wcscmp(name + key, suffixes[i]) && !wcsncmp(name + key - cache_length, cache, cache_length))
            return key;
    }
    /* ".cache.driver.<pid>.<n>.tmp": the last ".cache.driver." in a name that ends in ".tmp". */
    if (length > 4 && !wcscmp(name + length - 4, L".tmp"))
    {
        for (tail = name + length - 4; tail > name + prefix_length; --tail)
        {
            if (!wcsncmp(tail, L".cache.driver.", 14))
                return (size_t)(tail - name) + cache_length;
        }
    }
    return 0;
}

struct bc250_cache_group
{
    uint64_t size;          /* bytes of all files of the group */
    uint64_t last_write;    /* the newest write of any file of the group, FILETIME units */
    bool current;           /* the group of this process's program */
    bool drop;              /* out: delete the group */
};

/* Marks groups to drop until the total is within budget: the least recently written first, the group of this
 * process last (only when it alone is over the budget; the engine has not opened its files yet, so it starts
 * over). Returns the total that stays. */
static inline uint64_t bc250_cache_plan_trim(struct bc250_cache_group *groups, size_t count, uint64_t budget)
{
    uint64_t total = 0;
    size_t i, oldest;

    for (i = 0; i < count; ++i)
    {
        groups[i].drop = false;
        total += groups[i].size;
    }
    while (total > budget)
    {
        oldest = count;
        for (i = 0; i < count; ++i)
        {
            if (groups[i].drop || groups[i].current)
                continue;
            if (oldest == count || groups[i].last_write < groups[oldest].last_write)
                oldest = i;
        }
        if (oldest == count)
        {
            for (i = 0; i < count; ++i)
            {
                if (groups[i].current && !groups[i].drop)
                {
                    oldest = i;
                    break;
                }
            }
            if (oldest == count)
                break;
        }
        groups[oldest].drop = true;
        total -= groups[oldest].size;
    }
    return total;
}

#endif
