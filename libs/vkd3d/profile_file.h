/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: the machine profile file of vkd3d-proton, "<system drive>\ProgramData\amdgpu-wddm\profiles\
 * vkd3d-proton.conf". An administrator adds config flags for an executable there, without a new build:
 *
 *     # comment
 *     [HaloInfinite.exe]          the program name, compared as VKD3D_STRING_COMPARE_EXACT
 *     add = FORCE_RAW_VA_CBV, no_upload_hvv
 *     remove = small_vram_rebar
 *
 *     [starts_with:MyGame]        VKD3D_STRING_COMPARE_STARTS_WITH
 *     [ends_with:-Shipping.exe]   VKD3D_STRING_COMPARE_ENDS_WITH
 *     [appid:1245620]             VKD3D_STRING_COMPARE_APPID (SteamAppID)
 *
 * A name is a VKD3D_CONFIG option name (as in VKD3D_CONFIG) or a flag identifier of config_flag_decl.h (as in the
 * application_override table). The lines of each matching section apply in file order, after the built-in table.
 * One error (an unknown name, an option outside a section, a malformed line) refuses the whole file.
 * docs/design/app-profiles.md in amdgpu-wddm gives the full rules.
 */

#ifndef __VKD3D_PROFILE_FILE_H
#define __VKD3D_PROFILE_FILE_H

#include "vkd3d_debug.h"

#include <stdbool.h>
#include <stddef.h>

#define VKD3D_PROFILE_FILE_MAX_SIZE (1u << 20)
#define VKD3D_PROFILE_FILE_NAME "vkd3d-proton.conf"

/* Called for each applied name of a matching section, in file order. */
typedef void (*vkd3d_profile_entry_cb)(void *data, unsigned int line, const char *section, bool add,
        const char *name);

/* Parses the file text and applies the matching sections to *flags. app is the program name, appid the SteamAppID
 * (empty when absent), options the VKD3D_CONFIG option table. true: the file is valid, *flags has the matching
 * sections applied and *matched counts them. false: *flags is unchanged and error holds "line N: reason". */
bool vkd3d_profile_parse(const char *text, size_t size, const char *app, const char *appid,
        const struct vkd3d_debug_option *options, unsigned int option_count, union vkd3d_config_flags *flags,
        unsigned int *matched, vkd3d_profile_entry_cb cb, void *cb_data, char *error, size_t error_size);

#ifdef _WIN32
/* Reads the profile file. NULL with an empty why: no profile directory or file. NULL with why set: the file or its
 * directory is refused. name receives the path in UTF-8 for log lines. Free the text with free(). */
char *vkd3d_profile_file_read(char *name, size_t name_size, size_t *size, char *why, size_t why_size);

/* Whether a security descriptor lets only SYSTEM, Administrators and TrustedInstaller change the object. */
bool vkd3d_profile_trusted_descriptor(void *security_descriptor, char *why, size_t why_size);

#ifdef VKD3D_PROFILE_FILE_TEST
extern const wchar_t *vkd3d_profile_test_root;
extern bool vkd3d_profile_test_trust_all;
#endif
#endif

#endif  /* __VKD3D_PROFILE_FILE_H */
