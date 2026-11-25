// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spelunk::romset {

struct FileInfo {
    std::string path;
    uint64_t size = 0;
};

enum class PathKind { Missing, RegularFile, Directory };

PathKind GetPathKind(const std::string &path);
bool ListRegularFiles(const std::string &directory, std::vector<FileInfo> &files);

} // namespace spelunk::romset
