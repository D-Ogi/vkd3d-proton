/* SPDX-License-Identifier: MIT */
#ifndef VKD3D_SHADER_DIAGNOSTICS_H
#define VKD3D_SHADER_DIAGNOSTICS_H

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
#include <stddef.h>
#include <stdint.h>

/* BD-180: the shell's PSO failure record hashes all input bytes with FNV-1a64. The existing
 * vkd3d_shader_hash() uses FNV-1 (multiply then XOR), so it cannot join that record. This helper
 * is only for failure diagnostics, never for shader cache keys or successful compilation. */
static inline uint64_t vkd3d_shader_diagnostic_fnv1a64(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t i;

    for (i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
    return hash;
}
#endif

#endif
