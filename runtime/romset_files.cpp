// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
#include "romset_files.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if defined(NXDK)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace spelunk::romset {

PathKind GetPathKind(const std::string &path) {
#if defined(NXDK)
    const DWORD attributes = GetFileAttributes(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return PathKind::Missing;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Directory : PathKind::RegularFile;
#else
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        return PathKind::Missing;
    }
    if (S_ISDIR(status.st_mode)) {
        return PathKind::Directory;
    }
    return S_ISREG(status.st_mode) ? PathKind::RegularFile : PathKind::Missing;
#endif
}

bool ListRegularFiles(const std::string &directory, std::vector<FileInfo> &files) {
    files.clear();
#if defined(NXDK)
    std::string pattern = directory;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') {
        pattern.push_back('\\');
    }
    pattern.push_back('*');

    WIN32_FIND_DATA findData {};
    HANDLE find = FindFirstFile(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::string path = directory;
        if (!path.empty() && path.back() != '\\' && path.back() != '/') {
            path.push_back('\\');
        }
        path.append(findData.cFileName);
        const uint64_t size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
        files.push_back({std::move(path), size});
    } while (FindNextFile(find, &findData));
    FindClose(find);
    return true;
#else
    DIR *handle = opendir(directory.c_str());
    if (!handle) {
        return false;
    }
    while (const dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' ||
            (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        std::string path = directory;
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path.append(entry->d_name);
        struct stat status {};
        if (stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)) {
            files.push_back({std::move(path), static_cast<uint64_t>(status.st_size)});
        }
    }
    closedir(handle);
    return true;
#endif
}

} // namespace spelunk::romset
