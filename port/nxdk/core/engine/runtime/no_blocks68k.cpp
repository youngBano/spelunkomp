// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// A build with no translated 68000 code (the game's has not been made yet: tools/translate68k.sh): the interpreter
// runs everything.
#include "native68k.h"

namespace spelunk {
const NativeCode kNativeCode = {nullptr, 0, nullptr, 0, nullptr, 0};
} // namespace spelunk
