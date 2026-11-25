// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// What the ROM set loaders share (runtime/romset.cpp, runtime/romset_pgm.cpp): a set's files found by checksum in a
// folder or a zip.
#pragma once

#include "../machine/zip.h"
#include "romset_files.h"

#include <zlib.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace spelunk::romset {

// The files of one place (a folder or a zip), read on demand by checksum; and, for a split set (MAME's, where a
// clone's zip holds only the files that differ from its parent's), the places of its parents, looked in after it
struct Source {
    Zip zip;
    bool isZip = false;
    struct Plain {
        std::string path;
        uint64_t size;
        bool hashed = false;
        uint32_t crc = 0;
    };
    std::vector<Plain> files;
    std::vector<Source> parents;

    bool Open(const std::string &where) {
        const PathKind kind = GetPathKind(where);
        if (kind == PathKind::Missing) {
            return false;
        }
        if (kind == PathKind::RegularFile) {
            isZip = true;
            return zip.Open(where);
        }
        std::vector<FileInfo> entries;
        if (!ListRegularFiles(where, entries)) {
            return false;
        }
        for (FileInfo &entry : entries) {
            files.push_back({std::move(entry.path), entry.size});
        }
        return true;
    }
    // The file of this size and checksum (some dumps of a flash chip carry 0x100 more bytes after its contents:
    // the checksum is the whole file's, the contents are its first `size` bytes)
    static constexpr uint64_t kExtra = 0x100;
    // The file of this checksum, whatever its size
    bool FindAny(uint32_t crc, std::vector<uint8_t> &data) {
        if (FindAnyHere(crc, data)) {
            return true;
        }
        for (Source &parent : parents) {
            if (parent.FindAny(crc, data)) {
                return true;
            }
        }
        return false;
    }
    bool Find(uint32_t crc, uint64_t size, std::vector<uint8_t> &data) {
        if (FindHere(crc, size, data)) {
            return true;
        }
        for (Source &parent : parents) {
            if (parent.Find(crc, size, data)) {
                return true;
            }
        }
        return false;
    }
    // The set's parents (`rom_parent`: MAME's parent, then its parent or BIOS, if any), each a <name>.zip or <name>
    // folder found beside the set: in the folder the set's zip or folder is in (`place` the set's own zip or folder,
    // `where` the folder or zip the player gave), or in the folder given when its files are the set's
    void OpenParents(const std::string &place, const std::string &where, const std::string &names) {
        auto folderOf = [](const std::string &path) {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        };
        std::vector<std::string> folders;
        if (place != where) {
            folders.push_back(where); // (where/<set>.zip or where/<set>: the parents beside it in `where`)
        } else {
            if (GetPathKind(where) == PathKind::Directory) {
                folders.push_back(where); // (a folder of the set's files, perhaps its parents' too or their zips)
            }
            folders.push_back(folderOf(where)); // (the set's own zip or folder, chosen: its parents beside it)
        }
        std::istringstream list(names);
        std::string name;
        while (list >> name) {
            for (const std::string &folder : folders) {
                bool found = false;
                for (const std::string &path : {folder + "/" + name + ".zip", folder + "/" + name}) {
                    Source parent;
                    if (path != place && path != where && parent.Open(path)) {
                        parents.push_back(std::move(parent));
                        found = true;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
    }
    bool FindAnyHere(uint32_t crc, std::vector<uint8_t> &data) {
        if (isZip) {
            for (const Zip::Entry &e : zip.entries) {
                if (e.crc == crc) {
                    return zip.Read(e, data);
                }
            }
            return false;
        }
        for (Plain &f : files) {
            std::vector<uint8_t> bytes;
            if (!f.hashed) {
                if (!ReadWholeFile(f.path, bytes)) {
                    continue;
                }
                f.crc = static_cast<uint32_t>(crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
                f.hashed = true;
            }
            if (f.crc == crc && (!bytes.empty() || ReadWholeFile(f.path, bytes))) {
                data = std::move(bytes);
                return true;
            }
        }
        return false;
    }
    bool FindHere(uint32_t crc, uint64_t size, std::vector<uint8_t> &data) {
        if (isZip) {
            for (const Zip::Entry &e : zip.entries) {
                if (e.crc == crc && (e.size == size || e.size == size + kExtra)) {
                    if (!zip.Read(e, data)) {
                        return false;
                    }
                    data.resize(size);
                    return true;
                }
            }
            return false;
        }
        for (Plain &f : files) {
            if (f.size != size && f.size != size + kExtra) {
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!ReadWholeFile(f.path, bytes)) {
                continue;
            }
            if (!f.hashed) {
                f.crc = static_cast<uint32_t>(crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
                f.hashed = true;
            }
            if (f.crc == crc) {
                data = std::move(bytes);
                data.resize(size);
                return true;
            }
        }
        return false;
    }
};

// Where a game's set is looked for in `where` (the folder or zip the player gave): its own folder or zip in it, then
// `where` itself; then, for a merged set (a clone's files kept in its parent's zip), the parents' zips or folders in it
inline std::vector<std::string> Places(const std::string &where, const std::string &set, const std::string &parents) {
    std::vector<std::string> places = {where + "/" + set, where + "/" + set + ".zip", where};
    std::istringstream names(parents);
    std::string name;
    while (names >> name) {
        places.push_back(where + "/" + name + ".zip");
        places.push_back(where + "/" + name);
    }
    return places;
}

// The words added to a "has no <file>" message when the set has parents: a split set needs them beside it
inline std::string ParentsNeeded(const std::string &parents) {
    if (parents.empty()) {
        return {};
    }
    std::istringstream names(parents);
    std::string name, list;
    while (names >> name) {
        list += (list.empty() ? "" : " and ") + name + ".zip";
    }
    return " (a split set also needs " + list + " beside it)";
}

// "name:crc" (the name is for messages only)
inline bool ParseFile(const std::string &text, std::string &name, uint32_t &crc) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    name = text.substr(0, colon);
    crc = static_cast<uint32_t>(strtoul(text.substr(colon + 1).c_str(), nullptr, 16));
    return true;
}

// The dumps hold each 16-bit word low byte first; the board's memory is big-endian
inline void SwapWords(uint8_t *p, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(p[i], p[i + 1]);
    }
}

} // namespace spelunk::romset
