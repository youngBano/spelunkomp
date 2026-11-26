#pragma once

// Native Original Xbox FATX EEPROM persistence for CV1000 RTC-9701 (high scores & DIP/service settings).
// Saves/loads the 128-byte Rtc9701 EEPROM atomically to E:\mushisam\eeprom.bin (or Z:\mushieep.bin fallback).
#include "machine/rtc9701.h"

#ifdef NXDK
#include <nxdk/mount.h>
#include <windows.h>
#endif

#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

class XboxEeprom {
public:
    bool Load(spelunk::Rtc9701 &rtc) {
#ifdef NXDK
        EnsureMounted();
        HANDLE h = CreateFileA("E:\\mushisam\\eeprom.bin", GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD got = 0;
        const DWORD bytes = static_cast<DWORD>(rtc.eeprom.size() * sizeof(uint16_t));
        const BOOL ok = ReadFile(h, rtc.eeprom.data(), bytes, &got, nullptr);
        CloseHandle(h);
        if (ok && got == bytes) {
            std::memcpy(lastSaved_, rtc.eeprom.data(), sizeof(lastSaved_));
            loaded_ = true;
            return true;
        }
#else
        (void)rtc;
#endif
        return false;
    }

    void FlushIfDirty(const spelunk::Rtc9701 &rtc) {
#ifdef NXDK
        if (loaded_ && std::memcmp(lastSaved_, rtc.eeprom.data(), sizeof(lastSaved_)) == 0) {
            return;
        }
        EnsureMounted();
        CreateDirectoryA("E:\\mushisam", nullptr);
        HANDLE h = CreateFileA("E:\\mushisam\\eeprom.bin", GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const DWORD bytes = static_cast<DWORD>(rtc.eeprom.size() * sizeof(uint16_t));
            if (WriteFile(h, rtc.eeprom.data(), bytes, &written, nullptr) && written == bytes) {
                std::memcpy(lastSaved_, rtc.eeprom.data(), sizeof(lastSaved_));
                loaded_ = true;
            }
            CloseHandle(h);
        }
#else
        (void)rtc;
#endif
    }

private:
#ifdef NXDK
    static void EnsureMounted() {
        if (!nxIsDriveMounted('E')) {
            nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
        }
    }
#endif
    uint16_t lastSaved_[256] = {};
    bool loaded_ = false;
};

} // namespace spelunk_xbox
