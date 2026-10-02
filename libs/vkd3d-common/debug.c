/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_COUNT
#include "vkd3d_debug.h"
#include "vkd3d_threads.h"

#include "vkd3d_platform.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define VKD3D_DEBUG_BUFFER_COUNT 8

static const char *debug_level_names[] =
{
    /* VKD3D_DBG_LEVEL_UNKNOWN */ NULL,
    /* VKD3D_DBG_LEVEL_NONE    */ "none",
    /* VKD3D_DBG_LEVEL_ERR     */ "err",
    /* VKD3D_DBG_LEVEL_INFO    */ "info",
    /* VKD3D_DBG_LEVEL_FIXME   */ "fixme",
    /* VKD3D_DBG_LEVEL_WARN    */ "warn",
    /* VKD3D_DBG_LEVEL_TRACE   */ "trace",
};

static const char *env_for_channel[] =
{
    /* VKD3D_DBG_CHANNEL_API    */ "VKD3D_DEBUG",
    /* VKD3D_DBG_CHANNEL_SHADER */ "VKD3D_SHADER_DEBUG",
};

static unsigned int vkd3d_dbg_level[VKD3D_DBG_CHANNEL_COUNT];
static spinlock_t vkd3d_dbg_initialized;
static pthread_once_t vkd3d_dbg_once = PTHREAD_ONCE_INIT;
static FILE *vkd3d_log_file;
static bool vkd3d_disable_file;

void vkd3d_dbg_disable_debug_file(void)
{
    vkd3d_disable_file = true;
}

#ifdef _WIN32
typedef int (*PFN_wine_log)(const char *);
static PFN_wine_log wine_log_output;
#include <fcntl.h>
#include <io.h>
#endif

/* amdgpu-wddm: the engine runs inside the application, and the application's stdio is not ours. 3DMark's helper
 * processes pipe stderr without reading it and blocked in WriteFile once ~12 KB of driver lines had filled the
 * pipe (session 283). So without VKD3D_LOG_FILE the log goes where AMDGPU_WDDM_LOG says, the switch the D3D12
 * shell and the RADV ICD read too: "stderr" is upstream's stderr, "file:<path>" appends to a shared file, and
 * anything else, unset included, prints nothing on stdio. An explicit VKD3D_DEBUG, VKD3D_SHADER_DEBUG or
 * VKD3D_LOG_BUFFERED asks for stderr as upstream does. Without any of them only errors are kept, on the
 * debugger's output and bounded, and the level is err, so info and fixme lines cost nothing. */
static bool vkd3d_log_debugger_only;
static uint32_t vkd3d_debugger_budget = 1024;

static FILE *vkd3d_open_shared_log(const char *path)
{
#ifdef _WIN32
    /* Append only and shared: the shell and the ICD may write to the same file, and so may other processes. */
    HANDLE handle = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    FILE *file;
    int fd;

    if (handle == INVALID_HANDLE_VALUE)
        return NULL;
    if ((fd = _open_osfhandle((intptr_t)handle, _O_APPEND | _O_WRONLY)) < 0)
    {
        CloseHandle(handle);
        return NULL;
    }
    if (!(file = _fdopen(fd, "a")))
    {
        _close(fd);
        return NULL;
    }
    setvbuf(file, NULL, _IONBF, 0);
    return file;
#else
    return fopen(path, "a");
#endif
}

enum vkd3d_amdgpu_wddm_sink
{
    VKD3D_AMDGPU_WDDM_SINK_NONE,
    VKD3D_AMDGPU_WDDM_SINK_STDERR,
    VKD3D_AMDGPU_WDDM_SINK_FILE,
};

static enum vkd3d_amdgpu_wddm_sink vkd3d_amdgpu_wddm_log_sink(char *path, size_t path_size)
{
    char value[VKD3D_PATH_MAX];

    if (!vkd3d_get_env_var("AMDGPU_WDDM_LOG", value, sizeof(value)))
        return VKD3D_AMDGPU_WDDM_SINK_NONE;
    if (!strcmp(value, "stderr"))
        return VKD3D_AMDGPU_WDDM_SINK_STDERR;
    if (strncmp(value, "file:", 5) || !value[5] || strlen(value + 5) >= path_size)
        return VKD3D_AMDGPU_WDDM_SINK_NONE;
    strcpy(path, value + 5);
    return VKD3D_AMDGPU_WDDM_SINK_FILE;
}

/* With breadcrumbs trace and similar intensive logging operations,
 * reduce stdio/syscall overhead to an absolute minimum. */
struct vkd3d_string_stream
{
    char *buffer;
    size_t offset;
    size_t size;
};
static struct vkd3d_string_stream vkd3d_dbg_buffer;

static void vkd3d_dbg_init_once(void)
{
    bool explicit_request = false, log_file_requested;
    enum vkd3d_amdgpu_wddm_sink sink;
    char sink_path[VKD3D_PATH_MAX];
    char vkd3d_debug[VKD3D_PATH_MAX];
    unsigned int channel, i;

    for (channel = 0; channel < VKD3D_DBG_CHANNEL_COUNT; channel++)
    {
        if (!vkd3d_get_env_var(env_for_channel[channel], vkd3d_debug, sizeof(vkd3d_debug)))
            strncpy(vkd3d_debug, "", VKD3D_PATH_MAX);

        for (i = 1; i < ARRAY_SIZE(debug_level_names); ++i)
            if (!strcmp(debug_level_names[i], vkd3d_debug))
                vkd3d_dbg_level[channel] = i;

        /* Default debug level. */
        if (vkd3d_dbg_level[channel] == VKD3D_DBG_LEVEL_UNKNOWN)
            vkd3d_dbg_level[channel] = VKD3D_DBG_LEVEL_FIXME;
        else
            explicit_request = true;
    }

    if (vkd3d_get_env_var("VKD3D_LOG_BUFFERED", vkd3d_debug, sizeof(vkd3d_debug)))
        explicit_request = true;

    /* amdgpu-wddm: VKD3D_LOG_FILE first, then AMDGPU_WDDM_LOG, then an explicit request; else no stdio. */
    sink = vkd3d_amdgpu_wddm_log_sink(sink_path, sizeof(sink_path));
    log_file_requested = !vkd3d_disable_file &&
            vkd3d_get_env_var("VKD3D_LOG_FILE", vkd3d_debug, sizeof(vkd3d_debug));
    if (!log_file_requested && sink == VKD3D_AMDGPU_WDDM_SINK_FILE &&
            !(vkd3d_log_file = vkd3d_open_shared_log(sink_path)))
        sink = VKD3D_AMDGPU_WDDM_SINK_NONE;
    if (!log_file_requested && sink == VKD3D_AMDGPU_WDDM_SINK_NONE && !explicit_request)
    {
        vkd3d_log_debugger_only = true;
        for (channel = 0; channel < VKD3D_DBG_CHANNEL_COUNT; channel++)
            vkd3d_dbg_level[channel] = VKD3D_DBG_LEVEL_ERR;
        vkd3d_atomic_uint32_store_explicit(&vkd3d_dbg_initialized, 1, vkd3d_memory_order_release);
        return;
    }

    if (vkd3d_get_env_var("VKD3D_LOG_BUFFERED", vkd3d_debug, sizeof(vkd3d_debug)))
    {
        vkd3d_dbg_buffer.offset = 0;
        vkd3d_dbg_buffer.size = strtoul(vkd3d_debug, NULL, 0);
        if (!vkd3d_dbg_buffer.size)
            vkd3d_dbg_buffer.size = 64 * 1024;
        fprintf(vkd3d_log_file ? vkd3d_log_file : stderr, "Using VKD3D_LOG_BUFFERED with %zu byte chunks.\n",
                vkd3d_dbg_buffer.size);
        vkd3d_dbg_buffer.buffer = malloc(vkd3d_dbg_buffer.size);
    }

    if (!vkd3d_disable_file && vkd3d_get_env_var("VKD3D_LOG_FILE", vkd3d_debug, sizeof(vkd3d_debug)))
    {
        /* Avoid extra formatting overhead when using buffered. */
        vkd3d_log_file = fopen(vkd3d_debug, vkd3d_dbg_buffer.buffer ? "wb" : "w");
        if (!vkd3d_log_file)
        {
            fprintf(stderr, "Failed to open log file: %s!\n", vkd3d_debug);
            fflush(stderr);
        }
    }
    else if (!vkd3d_log_file)
    {
#ifdef _WIN32
        HMODULE module = LoadLibraryA("ntdll.dll");
        if (module)
            wine_log_output = (void*)GetProcAddress(module, "__wine_dbg_output");
#endif
    }

    vkd3d_atomic_uint32_store_explicit(&vkd3d_dbg_initialized, 1, vkd3d_memory_order_release);
}

static inline void vkd3d_dbg_init(void)
{
    /* Early out since we're going to be spamming calls to vkd3d_dbg_init() for every trace call. */
    if (!vkd3d_atomic_uint32_load_explicit(&vkd3d_dbg_initialized, vkd3d_memory_order_acquire))
        pthread_once(&vkd3d_dbg_once, vkd3d_dbg_init_once);
}

enum vkd3d_dbg_level vkd3d_dbg_get_level(enum vkd3d_dbg_channel channel)
{
    vkd3d_dbg_init();
    if (channel >= VKD3D_DBG_CHANNEL_COUNT)
        return VKD3D_DBG_LEVEL_FIXME;
    assert(vkd3d_dbg_level[channel] != VKD3D_DBG_LEVEL_UNKNOWN);
    return vkd3d_dbg_level[channel];
}

static spinlock_t vkd3d_debug_buffer_spin;

void vkd3d_dbg_flush(void)
{
    if (vkd3d_dbg_buffer.buffer)
    {
        spinlock_acquire(&vkd3d_debug_buffer_spin);
        if (vkd3d_dbg_buffer.offset)
        {
            if (vkd3d_log_file)
            {
                fwrite(vkd3d_dbg_buffer.buffer, 1, vkd3d_dbg_buffer.offset, vkd3d_log_file);
            }
            else
            {
                /* Binary vs text matters on Win32.
                 * Don't bother trying to be clever here reopening stdio files as O_BINARY, etc. */
                fputs(vkd3d_dbg_buffer.buffer, stderr);
            }

            vkd3d_dbg_buffer.offset = 0;
            fflush(vkd3d_log_file ? vkd3d_log_file : stderr);
        }
        spinlock_release(&vkd3d_debug_buffer_spin);
    }
}

void vkd3d_dbg_printf(enum vkd3d_dbg_channel channel, enum vkd3d_dbg_level level, const char *function, const char *fmt, ...)
{
    unsigned int tid;
    FILE *log_file;
    va_list args;

    if (vkd3d_dbg_get_level(channel) < level)
        return;
    assert(level < ARRAY_SIZE(debug_level_names));

    if (vkd3d_log_debugger_only)
    {
#ifdef _WIN32
        char local_buffer[1024];
        int offset;

        if (level > VKD3D_DBG_LEVEL_ERR || vkd3d_atomic_uint32_decrement(
                &vkd3d_debugger_budget, vkd3d_memory_order_relaxed) >= 0x80000000u)
            return;
        offset = snprintf(local_buffer, sizeof(local_buffer), "amdgpu_wddm_vkd3d %04x:%s:%s: ",
                vkd3d_get_current_thread_id(), debug_level_names[level], function);
        va_start(args, fmt);
        if (offset > 0 && offset < (int)sizeof(local_buffer))
            vsnprintf(local_buffer + offset, sizeof(local_buffer) - offset, fmt, args);
        va_end(args);
        OutputDebugStringA(local_buffer);
#endif
        return;
    }

    log_file = vkd3d_log_file ? vkd3d_log_file : stderr;

    va_start(args, fmt);
    tid = vkd3d_get_current_thread_id();

    if (vkd3d_dbg_buffer.buffer)
    {
        char prefix_buffer[256];
        int prefix_buffer_count;
        char local_buffer[4096];
        int local_buffer_count;
        int required_count;

        prefix_buffer_count = snprintf(prefix_buffer, sizeof(prefix_buffer),
                "%04x:%s:%s: ", tid, debug_level_names[level], function);
        local_buffer_count = vsnprintf(local_buffer, sizeof(local_buffer), fmt, args);
        required_count = prefix_buffer_count + local_buffer_count;

        spinlock_acquire(&vkd3d_debug_buffer_spin);
        if (vkd3d_dbg_buffer.offset + required_count > vkd3d_dbg_buffer.size)
        {
            if (vkd3d_log_file)
            {
                fwrite(vkd3d_dbg_buffer.buffer, 1, vkd3d_dbg_buffer.offset, vkd3d_log_file);
            }
            else
            {
                /* Binary vs text matters on Win32.
                 * Don't bother trying to be clever here reopening stdio files as O_BINARY, etc. */
                fputs(vkd3d_dbg_buffer.buffer, stderr);
            }

            vkd3d_dbg_buffer.offset = 0;
        }

        /* Here we trade performance for robustness. Some data will be left behind on early termination or crash. */
        if (vkd3d_dbg_buffer.offset + required_count <= vkd3d_dbg_buffer.size)
        {
            memcpy(vkd3d_dbg_buffer.buffer + vkd3d_dbg_buffer.offset, prefix_buffer, prefix_buffer_count);
            vkd3d_dbg_buffer.offset += prefix_buffer_count;
            memcpy(vkd3d_dbg_buffer.buffer + vkd3d_dbg_buffer.offset, local_buffer, local_buffer_count);
            vkd3d_dbg_buffer.offset += local_buffer_count;
        }
        else
        {
            /* If we cannot buffer up, just emit inline. */
            fputs(prefix_buffer, log_file);
            fputs(local_buffer, log_file);
        }
        spinlock_release(&vkd3d_debug_buffer_spin);
    }
#ifdef _WIN32
    else if (wine_log_output)
    {
        char local_buffer[4096];
        uint64_t ticks;
        size_t offset;

        /* Try to match format of Wine log output. */
        ticks = vkd3d_get_current_time_ns();
        offset = snprintf(local_buffer, sizeof(local_buffer),
                "%3u.%03u:%04x:%04x:%s:vkd3d-proton:%s: ",
                (unsigned int)(ticks / 1000000000), (unsigned int)((ticks % 1000000000) / 1000000),
                (UINT)GetCurrentProcessId(), tid,
                debug_level_names[level], function);
        if (offset < sizeof(local_buffer))
            vsnprintf(local_buffer + offset, sizeof(local_buffer) - offset, fmt, args);
        wine_log_output(local_buffer);
    }
#endif
    else
    {
        spinlock_acquire(&vkd3d_debug_buffer_spin);
        fprintf(log_file, "%04x:%s:%s: ", tid, debug_level_names[level], function);
        vfprintf(log_file, fmt, args);
        spinlock_release(&vkd3d_debug_buffer_spin);
        fflush(log_file);
    }
    va_end(args);
}

static char *get_buffer(void)
{
    static VKD3D_THREAD_LOCAL char buffers[VKD3D_DEBUG_BUFFER_COUNT][VKD3D_DEBUG_BUFFER_SIZE];
    static VKD3D_THREAD_LOCAL size_t buffer_index;
    size_t current_index;

    current_index = (++buffer_index) % ARRAY_SIZE(buffers);
    return buffers[current_index];
}

char *vkd3d_dbg_get_buffer(void)
{
    return get_buffer();
}

const char *vkd3d_dbg_vsprintf(const char *fmt, va_list args)
{
    char *buffer;

    buffer = get_buffer();
    vsnprintf(buffer, VKD3D_DEBUG_BUFFER_SIZE, fmt, args);
    buffer[VKD3D_DEBUG_BUFFER_SIZE - 1] = '\0';
    return buffer;
}

const char *vkd3d_dbg_sprintf(const char *fmt, ...)
{
    const char *buffer;
    va_list args;

    va_start(args, fmt);
    buffer = vkd3d_dbg_vsprintf(fmt, args);
    va_end(args);
    return buffer;
}

const char *debugstr_a(const char *str)
{
    char *buffer, *ptr;
    char c;

    if (!str)
        return "(null)";

    ptr = buffer = get_buffer();

    *ptr++ = '"';
    while ((c = *str++) && ptr <= buffer + VKD3D_DEBUG_BUFFER_SIZE - 8)
    {
        int escape_char;

        switch (c)
        {
            case '"':
            case '\\':
            case '\n':
            case '\r':
            case '\t':
                escape_char = c;
                break;
            default:
                escape_char = 0;
                break;
        }

        if (escape_char)
        {
            *ptr++ = '\\';
            *ptr++ = escape_char;
            continue;
        }

        if (isprint(c))
        {
            *ptr++ = c;
        }
        else
        {
            *ptr++ = '\\';
            sprintf(ptr, "%02x", c);
            ptr += 2;
        }
    }
    *ptr++ = '"';

    if (c)
    {
        *ptr++ = '.';
        *ptr++ = '.';
        *ptr++ = '.';
    }
    *ptr = '\0';

    return buffer;
}

const char *debugstr_w(const WCHAR *wstr)
{
    char *buffer, *ptr;
    WCHAR c;

    if (!wstr)
        return "(null)";

    ptr = buffer = get_buffer();

    *ptr++ = '"';
    while ((c = *wstr++) && ptr <= buffer + VKD3D_DEBUG_BUFFER_SIZE - 10)
    {
        int escape_char;

        switch (c)
        {
            case '"':
            case '\\':
            case '\n':
            case '\r':
            case '\t':
                escape_char = c;
                break;
            default:
                escape_char = 0;
                break;
        }

        if (escape_char)
        {
            *ptr++ = '\\';
            *ptr++ = escape_char;
            continue;
        }

        if (isprint(c))
        {
            *ptr++ = c;
        }
        else
        {
            *ptr++ = '\\';
            sprintf(ptr, "%04x", c);
            ptr += 4;
        }
    }
    *ptr++ = '"';

    if (c)
    {
        *ptr++ = '.';
        *ptr++ = '.';
        *ptr++ = '.';
    }
    *ptr = '\0';

    return buffer;
}

unsigned int vkd3d_env_var_as_uint(const char *name, unsigned int default_value)
{
    char value[VKD3D_PATH_MAX];
    unsigned long r;
    char *end_ptr;

    if (vkd3d_get_env_var(name, value, sizeof(value)) && strlen(value) > 0)
    {
        errno = 0;
        r = strtoul(value, &end_ptr, 0);
        if (!errno && end_ptr != value)
            return min(r, UINT_MAX);
    }

    return default_value;
}

static bool is_option_separator(char c)
{
    return c == ',' || c == ';' || c == '\0';
}

bool vkd3d_debug_list_has_member(const char *string, const char *member)
{
    char prev_char, next_char;
    const char *p;

    p = string;
    while (p)
    {
        if ((p = strstr(p, member)))
        {
            prev_char = p > string ? p[-1] : 0;
            p += strlen(member);
            next_char = *p;

            if (is_option_separator(prev_char) && is_option_separator(next_char))
                return true;
        }
    }

    return false;
}

union vkd3d_config_flags vkd3d_parse_debug_options(const char *string,
        const struct vkd3d_debug_option *options, unsigned int option_count)
{
    union vkd3d_config_flags flags = VKD3D_CONFIG_FLAGS_NONE;
    unsigned int i;

    for (i = 0; i < option_count; ++i)
    {
        const struct vkd3d_debug_option *opt = &options[i];

        if (vkd3d_debug_list_has_member(string, opt->name))
            vkd3d_config_flag_add(&flags, opt->flag);
    }

    return flags;
}
