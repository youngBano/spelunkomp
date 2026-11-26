// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The cabinet's controls as every board here is given them: a bit each, set while held.
#pragma once

#include <cstdint>

namespace spelunk {

enum : uint32_t {
    kInUp = 1u << 0,
    kInDown = 1u << 1,
    kInLeft = 1u << 2,
    kInRight = 1u << 3,
    kInButton1 = 1u << 4,
    kInButton2 = 1u << 5,
    kInButton3 = 1u << 6,
    kInButton4 = 1u << 7,
};
enum : uint32_t {
    kSysService = 1u << 0, // the service coin
    kSysTest = 1u << 1,    // the test button on the JAMMA edge
    kSysCoin1 = 1u << 2,
    kSysCoin2 = 1u << 3,
    kSysStart1 = 1u << 4,
    kSysStart2 = 1u << 5,
    kSysTestButton = 1u << 8, // S3, the push button on a CV1000 board
};

} // namespace spelunk
