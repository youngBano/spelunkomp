// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The board the game this program is built for runs on (the build says which, from game.cfg's `hardware`): what the
// port, the features and a game's own sources call Board. (SPELUNK_CAVE68K is defined for every 68000 board of Cave's
// games, the PGM's too: what is the 68000's is shared; SPELUNK_PGM then picks IGS's board.)
#pragma once

#if defined(SPELUNK_PGM)
#include "pgm.h"
namespace spelunk {
using Board = Pgm;
}
#elif defined(SPELUNK_CAVE68K)
#include "cave68k.h"
namespace spelunk {
using Board = Cave68k;
}
#else
#include "cv1000.h"
namespace spelunk {
using Board = Cv1000;
}
#endif
