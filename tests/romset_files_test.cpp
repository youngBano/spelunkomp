// SPDX-License-Identifier: GPL-3.0-only
// Host-side regression test for ROM directory enumeration.
#include "../runtime/romset_files.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "spelunkomp-romset-files-test";
    fs::remove_all(root);
    fs::create_directories(root / "subdir");
    {
        std::ofstream file(root / "alpha.bin", std::ios::binary);
        file << "abc";
    }
    {
        std::ofstream file(root / "beta.zip", std::ios::binary);
        file << "12345";
    }

    assert(spelunk::romset::GetPathKind(root.string()) == spelunk::romset::PathKind::Directory);
    assert(spelunk::romset::GetPathKind((root / "alpha.bin").string()) == spelunk::romset::PathKind::RegularFile);
    assert(spelunk::romset::GetPathKind((root / "missing").string()) == spelunk::romset::PathKind::Missing);

    std::vector<spelunk::romset::FileInfo> files;
    assert(spelunk::romset::ListRegularFiles(root.string(), files));
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    assert(files.size() == 2);
    assert(fs::equivalent(files[0].path, root / "alpha.bin"));
    assert(files[0].size == 3);
    assert(fs::equivalent(files[1].path, root / "beta.zip"));
    assert(files[1].size == 5);

    std::vector<spelunk::romset::FileInfo> missing;
    assert(!spelunk::romset::ListRegularFiles((root / "missing").string(), missing));
    fs::remove_all(root);
}
