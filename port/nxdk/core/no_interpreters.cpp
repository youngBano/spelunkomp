// Minimal stubs removing unused Z80 interpreter, OKI M6295, and YMFM FM-chip emulation
// from the DoDonPachi Original Xbox native build (DoDonPachi uses M68K + YMZ280B only).
#include "machine/cave_z80.h"
#include "machine/okim6295.h"

namespace spelunk {

// OkiPair stubs (unused on ddonpach: map->oki == false)
void OkiPair::Reset() {}
void OkiPair::RunTo(uint64_t, uint64_t) {}
uint8_t OkiPair::Read(int, uint64_t) const { return 0xFF; }
void OkiPair::Write(int, uint8_t, uint64_t) {}
void OkiPair::SetBank(int, uint8_t) {}
void OkiPair::Serialize(Archive &) {}

// CaveZ80 / Z80 stubs (unused on ddonpach: map->z80 == false)
struct CaveZ80::Chips {};

CaveZ80::CaveZ80() : z80(*this) {}
CaveZ80::~CaveZ80() = default;
void CaveZ80::Reset(Kind, uint64_t) {}
void CaveZ80::RunTo(uint64_t) {}
void CaveZ80::Command(uint16_t) {}
void CaveZ80::TakeAnswer() {}
void CaveZ80::Serialize(Archive &) {}
uint8_t CaveZ80::Read(uint16_t) { return 0xFF; }
void CaveZ80::Write(uint16_t, uint8_t) {}
uint8_t CaveZ80::In(uint16_t) { return 0xFF; }
void CaveZ80::Out(uint16_t, uint8_t) {}

} // namespace spelunk
