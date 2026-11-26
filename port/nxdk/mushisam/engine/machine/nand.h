// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// U2: the 128 MB NAND flash (Samsung K9F1G08U0M) that holds a CV1000 game's graphics, and on some games its
// settings and scores. 65536 pages of 2048 data bytes and 64 spare bytes, 64 pages to a block. The player's file is
// the chip's whole contents, spare bytes included; it is only read (what the game programs or erases is kept in
// memory and saved with the player's data).
#pragma once

#include "../runtime/state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>
#ifdef NXDK
#include <windows.h>
#endif

namespace spelunk {

class Nand {
public:
    static constexpr uint32_t kPageData = 2048, kPageTotal = 2048 + 64, kPages = 64 * 1024, kBlockPages = 64;
    static constexpr size_t kSize = static_cast<size_t>(kPageTotal) * kPages;
    // 256-slot 4-way set-associative LRU streaming cache (64 sets * 4 ways * 2,112 B = ~540 KB)
    // + 256 KB sector-aligned sequential read-ahead buffer to eliminate DVD/HDD seek stalls.
    static constexpr uint32_t kCacheSets = 64;
    static constexpr uint32_t kCacheWays = 4;
    static constexpr uint32_t kCacheSlots = kCacheSets * kCacheWays; // 256
    static constexpr size_t kReadAheadBytes = 256u * 1024u;

    std::vector<uint8_t> data;     // Optional full in-memory image (host); empty when streaming from file on Xbox
    std::set<uint32_t> written;    // blocks the game has programmed or erased

    ~Nand() { CloseFile(); }

    bool AttachFile(const char *path) {
        CloseFile();
#ifdef NXDK
        // Bypass nxdk pdclib's 1 KB byte-by-byte fread() loop with direct 128 KB Xbox ReadFile() syscalls.
        m_handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(m_handle, &sz) || sz.QuadPart <= 0) {
            CloseFile();
            return false;
        }
        m_fileSize = static_cast<size_t>(sz.QuadPart);
#else
        m_file = std::fopen(path, "rb");
        if (!m_file) {
            return false;
        }
        std::fseek(m_file, 0, SEEK_END);
        const long sz = std::ftell(m_file);
        std::fseek(m_file, 0, SEEK_SET);
        m_fileSize = sz > 0 ? static_cast<size_t>(sz) : 0;
#endif
        for (auto &c : m_cache) {
            c.page = UINT32_MAX;
            c.lru = 0;
        }
        m_lruClock = 0;
        m_raOff = 0;
        m_raLen = 0;
        m_curPageBuf = nullptr;
        m_curPageIdx = UINT32_MAX;
        return m_fileSize > 0;
    }

    void CloseFile() {
#ifdef NXDK
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_file) {
            std::fclose(m_file);
            m_file = nullptr;
        }
#endif
        m_fileSize = 0;
        m_raOff = 0;
        m_raLen = 0;
        m_curPageBuf = nullptr;
        m_curPageIdx = UINT32_MAX;
    }

    bool HasStreamFile() const {
#ifdef NXDK
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_file != nullptr;
#endif
    }

    size_t ImageSize() const { return HasStreamFile() ? m_fileSize : data.size(); }

    void Command(uint8_t c) {
        switch (c) {
        case 0xFF:
            m_mode = Mode::Idle;
            m_status = 0xC0;
            break;
        case 0x00:
            m_mode = Mode::Read;
            m_addressCount = 0;
            break;
        case 0x30:
            if (m_mode == Mode::Read && m_addressCount >= 4) {
                m_at = static_cast<size_t>(m_page) * kPageTotal + m_column;
                m_streamPage = m_page;
                m_streamCol = m_column;
                m_mode = Mode::Reading;
                reads++;
            }
            break;
        case 0x05: // (another column of the page being read)
            m_mode = Mode::Column;
            m_addressCount = 0;
            break;
        case 0xE0:
            if (m_mode == Mode::Column) {
                m_at = static_cast<size_t>(m_page) * kPageTotal + m_column;
                m_streamPage = m_page;
                m_streamCol = m_column;
                m_mode = Mode::Reading;
            }
            break;
        case 0x80:
            m_mode = Mode::Program;
            m_addressCount = 0;
            m_programmed = 0;
            std::memset(m_pageRegister, 0xFF, sizeof m_pageRegister);
            break;
        case 0x10:
            if (m_mode == Mode::Program) {
                if (m_page < kPages) {
                    if (!HasStreamFile() && static_cast<size_t>(m_page + 1) * kPageTotal <= data.size()) {
                        uint8_t *page = &data[static_cast<size_t>(m_page) * kPageTotal];
                        for (uint32_t i = 0; i < kPageTotal; i++) {
                            page[i] &= m_pageRegister[i];
                        }
                    } else if (HasStreamFile()) {
                        auto &dst = GetOrCreateDirtyPage(m_page);
                        for (uint32_t i = 0; i < kPageTotal; i++) {
                            dst[i] &= m_pageRegister[i];
                        }
                        m_curPageIdx = UINT32_MAX;
                    }
                    written.insert(m_page / kBlockPages);
                }
                m_status = 0xC0;
            }
            m_mode = Mode::Idle;
            break;
        case 0x60:
            m_mode = Mode::Erase;
            m_addressCount = 0;
            m_page = 0;
            break;
        case 0xD0:
            if (m_mode == Mode::Erase && m_page < kPages) {
                const uint32_t first = m_page & ~(kBlockPages - 1);
                if (!HasStreamFile() && static_cast<size_t>(first + kBlockPages) * kPageTotal <= data.size()) {
                    std::memset(&data[static_cast<size_t>(first) * kPageTotal], 0xFF, static_cast<size_t>(kBlockPages) * kPageTotal);
                } else if (HasStreamFile()) {
                    for (uint32_t p = 0; p < kBlockPages; ++p) {
                        auto &dst = m_dirtyPages[first + p];
                        dst.fill(0xFF);
                    }
                    m_curPageIdx = UINT32_MAX;
                }
                written.insert(first / kBlockPages);
                m_status = 0xC0;
            }
            m_mode = Mode::Idle;
            break;
        case 0x70:
            m_mode = Mode::Status;
            break;
        case 0x90:
            m_mode = Mode::Id;
            m_idAt = 0;
            break;
        default:
            m_mode = Mode::Idle;
            break;
        }
    }
    void Address(uint8_t a) {
        switch (m_mode) {
        case Mode::Read:
        case Mode::Program:
            // (two column bytes, then two page bytes)
            if (m_addressCount == 0) {
                m_column = a;
                m_page = 0;
            } else if (m_addressCount == 1) {
                m_column |= (a & 0x0F) << 8;
            } else if (m_addressCount < 4) {
                m_page |= static_cast<uint32_t>(a) << (8 * (m_addressCount - 2));
            }
            m_addressCount++;
            break;
        case Mode::Column:
            if (m_addressCount == 0) {
                m_column = a;
            } else if (m_addressCount == 1) {
                m_column |= (a & 0x0F) << 8;
            }
            m_addressCount++;
            break;
        case Mode::Erase:
            if (m_addressCount < 2) {
                m_page |= static_cast<uint32_t>(a) << (8 * m_addressCount);
            }
            m_addressCount++;
            break;
        default:
            break;
        }
    }
    uint8_t Read() {
        switch (m_mode) {
        case Mode::Reading:
            if (!HasStreamFile()) {
                return m_at < data.size() ? data[m_at++] : 0xFF;
            }
            return ReadStreamingByte();
        case Mode::Status:
            return m_status;
        case Mode::Id: {
            static const uint8_t kId[4] = {0xEC, 0xF1, 0x00, 0x15};
            return kId[m_idAt++ & 3];
        }
        default:
            return 0xFF;
        }
    }
    // Bulk DMA fast path: copies `count` bytes directly from the active NAND page(s) into `dst`.
    bool ReadBlock(uint8_t *dst, uint32_t count) {
        if (m_mode != Mode::Reading) {
            return false;
        }
        if (!HasStreamFile()) {
            const size_t avail = m_at < data.size() ? (data.size() - m_at) : 0;
            const size_t n = avail < count ? avail : count;
            if (n > 0) {
                std::memcpy(dst, data.data() + m_at, n);
                m_at += n;
            }
            if (n < count) {
                std::memset(dst + n, 0xFF, count - n);
            }
            return true;
        }
        uint32_t done = 0;
        while (done < count) {
            if (m_at >= m_fileSize) {
                std::memset(dst + done, 0xFF, count - done);
                return true;
            }
            if (m_streamCol >= kPageTotal) {
                m_streamCol -= kPageTotal;
                ++m_streamPage;
            }
            const uint8_t *buf = (m_streamPage == m_curPageIdx && m_curPageBuf)
                                     ? m_curPageBuf
                                     : FetchStreamingPage(m_streamPage);
            const uint32_t pageLeft = kPageTotal - m_streamCol;
            const size_t fileLeft = m_fileSize - m_at;
            uint32_t chunk = count - done;
            if (chunk > pageLeft) chunk = pageLeft;
            if (static_cast<size_t>(chunk) > fileLeft) chunk = static_cast<uint32_t>(fileLeft);
            std::memcpy(dst + done, buf + m_streamCol, chunk);
            m_streamCol += chunk;
            m_at += chunk;
            done += chunk;
        }
        return true;
    }
    void Write(uint8_t v) {
        if (m_mode == Mode::Program) {
            const uint32_t at = m_column + m_programmed++;
            if (at < kPageTotal) {
                m_pageRegister[at] = v;
            }
        }
    }
    uint64_t reads = 0; // page reads started (the checks: how much loading a frame did)
    uint64_t cacheMisses = 0; // streaming disk/DVD misses (for Phase 3 verification)

    void Serialize(Archive &ar) {
        ar.Mark(0x4E414E44);
        ar.Pod(m_mode);
        ar.Pod(m_status);
        ar.Pod(m_addressCount);
        ar.Pod(m_column);
        ar.Pod(m_page);
        ar.Pod(m_at);
        ar.Pod(m_idAt);
        ar.Pod(m_programmed);
        ar.Pod(m_pageRegister);
        // (the blocks the game has written, with their contents)
        std::vector<uint32_t> blocks(written.begin(), written.end());
        ar.Vec(blocks);
        if (!ar.Saving()) {
            written = std::set<uint32_t>(blocks.begin(), blocks.end());
        }
        for (uint32_t b : blocks) {
            if (b < kPages / kBlockPages) {
                ar.Bytes(&data[static_cast<size_t>(b) * kBlockPages * kPageTotal], static_cast<size_t>(kBlockPages) * kPageTotal);
            }
        }
    }

private:
    struct CacheSlot {
        uint32_t page = UINT32_MAX;
        uint32_t lru = 0;
        uint8_t bytes[kPageTotal] = {};
    };

    const uint8_t *FetchStreamingPage(uint32_t pageIdx) {
        if (pageIdx == m_curPageIdx && m_curPageBuf) {
            return m_curPageBuf;
        }
        if (!m_dirtyPages.empty()) {
            auto it = m_dirtyPages.find(pageIdx);
            if (it != m_dirtyPages.end()) {
                m_curPageIdx = pageIdx;
                m_curPageBuf = it->second.data();
                return m_curPageBuf;
            }
        }
        // O(1) 4-way set-associative lookup (checks 4 tags instead of scanning 512 slots).
        const uint32_t setBase = (pageIdx & (kCacheSets - 1u)) * kCacheWays;
        uint32_t victim = setBase;
        uint32_t oldest = UINT32_MAX;
        for (uint32_t w = 0; w < kCacheWays; ++w) {
            CacheSlot &cand = m_cache[setBase + w];
            if (cand.page == pageIdx) {
                cand.lru = ++m_lruClock;
                m_curPageIdx = pageIdx;
                m_curPageBuf = cand.bytes;
                return m_curPageBuf;
            }
            if (cand.lru < oldest) {
                oldest = cand.lru;
                victim = setBase + w;
            }
        }
        CacheSlot &slot = m_cache[victim];
        slot.page = pageIdx;
        slot.lru = ++m_lruClock;
        ++cacheMisses;
        const size_t off = static_cast<size_t>(pageIdx) * kPageTotal;
        if (off < m_fileSize) {
            // 128 KB sequential read-ahead eliminates per-page Xbox DVD fseek/fread stalls.
            if (off < m_raOff || off + kPageTotal > m_raOff + m_raLen) {
                m_raOff = off;
#ifdef NXDK
                LARGE_INTEGER pos{};
                pos.QuadPart = static_cast<LONGLONG>(m_raOff);
                SetFilePointerEx(m_handle, pos, nullptr, FILE_BEGIN);
                DWORD got = 0;
                m_raLen = ReadFile(m_handle, m_raBuf, static_cast<DWORD>(sizeof(m_raBuf)), &got, nullptr)
                              ? static_cast<size_t>(got)
                              : 0u;
#else
                std::fseek(m_file, static_cast<long>(m_raOff), SEEK_SET);
                m_raLen = std::fread(m_raBuf, 1, sizeof(m_raBuf), m_file);
#endif
            }
            const size_t avail = (off < m_raOff + m_raLen) ? (m_raOff + m_raLen - off) : 0;
            const size_t copyLen = avail < kPageTotal ? avail : kPageTotal;
            if (copyLen > 0) {
                std::memcpy(slot.bytes, m_raBuf + (off - m_raOff), copyLen);
            }
            if (copyLen < kPageTotal) {
                std::memset(slot.bytes + copyLen, 0xFF, kPageTotal - copyLen);
            }
        } else {
            std::memset(slot.bytes, 0xFF, kPageTotal);
        }
        m_curPageIdx = pageIdx;
        m_curPageBuf = slot.bytes;
        return m_curPageBuf;
    }

    std::array<uint8_t, kPageTotal> &GetOrCreateDirtyPage(uint32_t pageIdx) {
        auto it = m_dirtyPages.find(pageIdx);
        if (it != m_dirtyPages.end()) {
            return it->second;
        }
        auto &buf = m_dirtyPages[pageIdx];
        const uint8_t *src = FetchStreamingPage(pageIdx);
        std::memcpy(buf.data(), src, kPageTotal);
        return buf;
    }

    inline uint8_t ReadStreamingByte() {
        if (m_at >= m_fileSize) {
            return 0xFF;
        }
        ++m_at;
        // Zero-division fast path: track page & column directly instead of 64-bit __udivdi3 per byte!
        if (m_streamCol >= kPageTotal) {
            m_streamCol -= kPageTotal;
            ++m_streamPage;
        }
        const uint8_t *buf = (m_streamPage == m_curPageIdx && m_curPageBuf)
                                 ? m_curPageBuf
                                 : FetchStreamingPage(m_streamPage);
        return buf[m_streamCol++];
    }

    enum class Mode : uint8_t { Idle, Read, Reading, Column, Program, Erase, Status, Id };
    Mode m_mode = Mode::Idle;
    uint8_t m_status = 0xC0; // (ready, not write-protected)
    uint32_t m_addressCount = 0, m_column = 0, m_page = 0, m_idAt = 0, m_programmed = 0;
    uint32_t m_streamPage = 0, m_streamCol = 0;
    uint64_t m_at = 0;
    uint8_t m_pageRegister[kPageTotal] = {};

#ifdef NXDK
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    FILE *m_file = nullptr;
#endif
    size_t m_fileSize = 0;
    size_t m_raOff = 0;
    size_t m_raLen = 0;
    uint8_t m_raBuf[kReadAheadBytes] = {};
    CacheSlot m_cache[kCacheSlots] = {};
    uint32_t m_lruClock = 0;
    uint32_t m_curPageIdx = UINT32_MAX;
    const uint8_t *m_curPageBuf = nullptr;
    std::unordered_map<uint32_t, std::array<uint8_t, kPageTotal>> m_dirtyPages;
};

} // namespace spelunk
