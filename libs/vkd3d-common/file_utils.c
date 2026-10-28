/*
 * Copyright 2022 Hans-Kristian Arntzen for Valve Corporation
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

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_file_utils.h"
#include "vkd3d_debug.h"

/* For disk cache. */
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
/* amdgpu-wddm fork: the paths are UTF-8 (the embedder's cache directory and vkd3d_get_program_name), and the file
 * calls are the wide ones. An ANSI call turns a character outside the process code page into '?', and the call then
 * fails, so a user name such as "Łucja" on an English Windows had no disk cache. A path that is not valid UTF-8 (a
 * lab variable read with the ANSI environment call) is taken in the process code page, as before. The caller frees
 * the result; NULL when the path cannot be converted. */
WCHAR *vkd3d_file_wide_path(const char *path)
{
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    WCHAR *wide;
    int count;

    if (!path)
        return NULL;
    if (!(count = MultiByteToWideChar(code_page, flags, path, -1, NULL, 0)))
    {
        code_page = CP_ACP;
        flags = 0;
        if (!(count = MultiByteToWideChar(code_page, flags, path, -1, NULL, 0)))
            return NULL;
    }
    if (!(wide = malloc(count * sizeof(*wide))))
        return NULL;
    if (!MultiByteToWideChar(code_page, flags, path, -1, wide, count))
    {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

bool vkd3d_file_rename_overwrite(const char *from_path, const char *to_path)
{
#ifdef _WIN32
    WCHAR *from = vkd3d_file_wide_path(from_path), *to = vkd3d_file_wide_path(to_path);
    DWORD code = ERROR_INVALID_NAME;

    if (from && to)
    {
        code = ERROR_SUCCESS;
        if (!MoveFileW(from, to))
        {
            code = GetLastError();
            if (code == ERROR_ALREADY_EXISTS)
            {
                code = ERROR_SUCCESS;
                if (!ReplaceFileW(to, from, NULL, 0, NULL, NULL))
                    code = GetLastError();
            }
        }
    }
    free(from);
    free(to);

    return code == ERROR_SUCCESS;
#else
    return rename(from_path, to_path) == 0;
#endif
}

bool vkd3d_file_rename_no_replace(const char *from_path, const char *to_path)
{
#ifdef _WIN32
    WCHAR *from = vkd3d_file_wide_path(from_path), *to = vkd3d_file_wide_path(to_path);
    bool ok = from && to && MoveFileW(from, to);

    free(from);
    free(to);
    return ok;
#else
    return renameat2(AT_FDCWD, from_path, AT_FDCWD, to_path, RENAME_NOREPLACE) == 0;
#endif
}

bool vkd3d_file_delete(const char *path)
{
#ifdef _WIN32
    WCHAR *wide = vkd3d_file_wide_path(path);
    bool ok = wide && DeleteFileW(wide);

    free(wide);
    return ok;
#else
    return unlink(path) == 0;
#endif
}

FILE *vkd3d_file_open(const char *path, const char *mode)
{
#ifdef _WIN32
    WCHAR wide_mode[8];
    FILE *file = NULL;
    WCHAR *wide;
    size_t i;

    for (i = 0; mode[i] && i + 1 < ARRAYSIZE(wide_mode); i++)
        wide_mode[i] = (unsigned char)mode[i];
    wide_mode[i] = 0;
    if ((wide = vkd3d_file_wide_path(path)))
        file = _wfopen(wide, wide_mode);
    free(wide);
    return file;
#else
    return fopen(path, mode);
#endif
}

FILE *vkd3d_file_open_exclusive_write(const char *path)
{
#ifdef _WIN32
    /* From Fossilize. AFAIK, there is no direct way to make this work with FILE interface, so have to roundtrip
     * through jank POSIX layer.
     * wbx kinda works, but Wine warns about it, despite it working anyways.
     * Older MSVC runtimes do not support wbx. */
    WCHAR *wide = vkd3d_file_wide_path(path);
    FILE *file = NULL;
    int fd = -1;

    if (wide)
        fd = _wopen(wide, _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL | _O_TRUNC | _O_SEQUENTIAL,
                _S_IWRITE | _S_IREAD);
    free(wide);
    if (fd >= 0)
    {
        file = _fdopen(fd, "wb");
        /* _fdopen takes ownership. */
        if (!file)
            _close(fd);
    }
    return file;
#else
    return fopen(path, "wbx");
#endif
}

void vkd3d_file_unmap(struct vkd3d_memory_mapped_file *file)
{
    if (file->mapped)
    {
#ifdef _WIN32
        UnmapViewOfFile(file->mapped);
#else
        munmap(file->mapped, file->mapped_size);
#endif
    }
    memset(file, 0, sizeof(*file));
}

bool vkd3d_file_map_read_only(const char *path, struct vkd3d_memory_mapped_file *file)
{
#ifdef _WIN32
    DWORD size_hi, size_lo;
    HANDLE file_mapping;
    HANDLE handle;
    WCHAR *wide;
#else
    struct stat stat_buf;
    int fd;
#endif

    file->mapped = NULL;
    file->mapped_size = 0;

#ifdef _WIN32
    handle = INVALID_HANDLE_VALUE;
    if ((wide = vkd3d_file_wide_path(path)))
    {
        handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        free(wide);
    }
    if (handle == INVALID_HANDLE_VALUE)
        goto out;

    size_lo = GetFileSize(handle, &size_hi);
    file->mapped_size = size_lo | (((uint64_t)size_hi) << 32);

    file_mapping = CreateFileMappingA(handle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (file_mapping == INVALID_HANDLE_VALUE)
        goto out;

    file->mapped = MapViewOfFile(file_mapping, FILE_MAP_READ, 0, 0, file->mapped_size);
    CloseHandle(file_mapping);
    file_mapping = INVALID_HANDLE_VALUE;
    if (!file->mapped)
    {
        ERR("Failed to MapViewOfFile for %s.\n", path);
        goto out;
    }

out:
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
#else
    fd = open(path, O_RDONLY);
    if (fd < 0)
        goto out;

    if (fstat(fd, &stat_buf) < 0)
    {
        ERR("Failed to fstat pipeline cache.\n");
        goto out;
    }

    /* Map private to make sure we get CoW behavior in case someone clobbers
     * the cache while in flight. We need to read data directly out of the cache. */
    file->mapped = mmap(NULL, stat_buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file->mapped != MAP_FAILED)
        file->mapped_size = stat_buf.st_size;
    else
        goto out;

out:
    if (fd >= 0)
        close(fd);
#endif

    if (!file->mapped)
        file->mapped_size = 0;
    return file->mapped != NULL;
}
