// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
#include "romset.h"
#include "romset_source.h"

#include "../machine/cave68k.h"
#include "../machine/cv1000.h"
#include "../machine/zip.h"

#include <zlib.h>

#include <fstream>
#include <sstream>

namespace spelunk {

using namespace romset;

namespace {
    std::string Trim(const std::string &s) {
        const size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) {
            return {};
        }
        return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
    }

} // namespace

void ParseGameConfig(const std::string &text, GameConfig &config) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        config[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
}

bool ReadGameConfig(const std::string &path, GameConfig &config, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    ParseGameConfig(text.str(), config);
    return true;
}

bool LoadSet(const std::string &where, const GameConfig &config, Cv1000 &board, std::string &error) {
    auto value = [&](const char *key) {
        const auto it = config.find(key);
        return it == config.end() ? std::string() : it->second;
    };
    const std::string set = value("rom_set");
    // (the place itself, or the set's folder or zip inside it)
    const std::vector<std::string> places = Places(where, set, value("rom_parent"));
    board.boardD = value("board") == "d";
    const size_t programSize = board.boardD ? 0x400000 : 0x200000;

    std::string name, missing, first;
    uint32_t crc = 0;
    for (const std::string &place : places) {
        Source source;
        if (!source.Open(place)) {
            continue;
        }
        source.OpenParents(place, where, value("rom_parent"));
        // (the message tells of the first place that has some of the set: later ones are tried in case)
        if (first.empty()) {
            first = missing;
        }
        missing.clear();
        std::vector<uint8_t> data;
        // U4: the program
        if (!ParseFile(value("program"), name, crc) || !source.Find(crc, programSize, data)) {
            missing = name;
            continue;
        }
        SwapWords(data.data(), data.size());
        for (size_t at = 0; at < board.program.size(); at += data.size()) {
            std::memcpy(&board.program[at], data.data(), data.size());
        }
        // U2: the NAND flash
        if (!ParseFile(value("nand"), name, crc) || !source.Find(crc, Nand::kSize, board.nand.data)) {
            missing = name;
            continue;
        }
        // U23, U24: the sound chip's two ROMs
        std::istringstream sound(value("sound"));
        std::string one;
        size_t at = 0;
        while (sound >> one && missing.empty()) {
            if (!ParseFile(one, name, crc) || !source.Find(crc, 0x400000, data) || at + data.size() > board.soundRom.size()) {
                missing = name;
                break;
            }
            SwapWords(data.data(), data.size());
            std::memcpy(&board.soundRom[at], data.data(), data.size());
            at += data.size();
        }
        if (missing.empty()) {
            board.Reset();
            return true;
        }
    }
    missing = first.empty() ? missing : first;
    error = missing.empty() ? "no " + set + " set in " + where : "the " + set + " set in " + where + " has no " + missing + " with the checksum expected" + ParentsNeeded(value("rom_parent"));
    return false;
}

// A Cave 68000 board's set: the program in two halves (the even bytes, the odd), the sprites, each layer's tiles,
// the samples; the EEPROM as the factory left it, where the set has one
bool LoadSet(const std::string &where, const GameConfig &config, Cave68k &board, std::string &error) {
    auto value = [&](const char *key) {
        const auto it = config.find(key);
        return it == config.end() ? std::string() : it->second;
    };
    const std::string set = value("rom_set");
    const std::vector<std::string> places = Places(where, set, value("rom_parent"));
    std::string missing, first;
    for (const std::string &place : places) {
        Source source;
        if (!source.Open(place)) {
            continue;
        }
        source.OpenParents(place, where, value("rom_parent"));
        // (the message tells of the first place that has some of the set: later ones are tried in case)
        if (first.empty()) {
            first = missing;
        }
        missing.clear();
        // Every file a key names, one after another
        auto all = [&](const char *key, std::vector<std::vector<uint8_t>> &parts) {
            std::istringstream names(value(key));
            std::string one, name;
            uint32_t crc = 0;
            while (missing.empty() && names >> one) {
                parts.emplace_back();
                if (!ParseFile(one, name, crc) || !source.FindAny(crc, parts.back())) {
                    missing = name;
                }
            }
            return missing.empty();
        };
        auto joined = [&](const char *key, std::vector<uint8_t> &whole) {
            std::vector<std::vector<uint8_t>> parts;
            if (!all(key, parts)) {
                return false;
            }
            whole.clear();
            for (const auto &part : parts) {
                whole.insert(whole.end(), part.begin(), part.end());
            }
            return true;
        };
        // The program: two halves (the even bytes, the odd), or one file of 16-bit words held low byte first
        std::vector<std::vector<uint8_t>> halves;
        if (!all("program", halves) || halves.empty() || halves.size() > 2 || (halves.size() == 2 && halves[0].size() != halves[1].size())) {
            missing = missing.empty() ? "program" : missing;
            continue;
        }
        if (halves.size() == 1) {
            board.program = halves[0];
            SwapWords(board.program.data(), board.program.size());
        } else {
            board.program.resize(halves[0].size() * 2);
            for (size_t n = 0; n < halves[0].size(); n++) {
                board.program[n * 2] = halves[0][n];
                board.program[n * 2 + 1] = halves[1][n];
            }
        }
        auto number = [&](const char *key, uint32_t otherwise) {
            const std::string v = value(key);
            return v.empty() ? otherwise : static_cast<uint32_t>(strtoul(v.c_str(), nullptr, 0));
        };
        // The sprites (`sprites_bits`): 4 bits a pixel, the left pixel of a byte in its high half (`sprites_format =
        // swapped`: the low half), the dumps holding each 16-bit word low byte first (`sprites_order = bytes`: in
        // order); or 8 bits a pixel, the files in pairs, each pair's 16-bit words side by side (low byte first), four
        // pixels in each 32 bits: their high halves in the first word's bytes, their low halves in the second's
        std::vector<uint8_t> packed;
        const bool eightBits = number("sprites_bits", 4) == 8;
        if (eightBits) {
            std::vector<std::vector<uint8_t>> parts;
            if (!all("sprites", parts) || parts.size() % 2) {
                missing = missing.empty() ? "sprites" : missing;
                continue;
            }
            packed.clear();
            for (size_t n = 0; n + 1 < parts.size(); n += 2) {
                const std::vector<uint8_t> &a = parts[n], &b = parts[n + 1];
                const size_t at = packed.size();
                packed.resize(at + a.size() * 2);
                for (size_t w = 0; w + 1 < a.size() && w + 1 < b.size(); w += 2) {
                    packed[at + w * 2 + 0] = a[w + 1];
                    packed[at + w * 2 + 1] = a[w];
                    packed[at + w * 2 + 2] = b[w + 1];
                    packed[at + w * 2 + 3] = b[w];
                }
            }
            // (a board with less than a power of two of sprite ROM sees the last pair again above it)
            const size_t last = parts.empty() ? 0 : parts.back().size() * 2;
            while (last && (packed.size() & (packed.size() - 1))) {
                packed.insert(packed.end(), packed.end() - static_cast<std::ptrdiff_t>(last), packed.end());
            }
            board.video.spritePixels.assign(packed.size(), 0);
            for (size_t at = 0; at + 3 < packed.size(); at += 4) {
                const uint8_t b0 = packed[at], b1 = packed[at + 1], b2 = packed[at + 2], b3 = packed[at + 3];
                uint8_t *p = &board.video.spritePixels[at];
                p[0] = static_cast<uint8_t>(((b1 & 15) << 4) | (b3 & 15));
                p[1] = static_cast<uint8_t>((b1 & 0xF0) | (b3 >> 4));
                p[2] = static_cast<uint8_t>(((b0 & 15) << 4) | (b2 & 15));
                p[3] = static_cast<uint8_t>((b0 & 0xF0) | (b2 >> 4));
            }
        } else {
            if (!joined("sprites", packed)) {
                continue;
            }
            if (value("sprites_order") != "bytes") {
                SwapWords(packed.data(), packed.size());
            }
            if (value("sprites_cipher") == "mazinger") {
                // (Mazinger Z's sprite ROMs are read through scrambled address lines: the byte at each address is
                // the one a rearrangement of its bits names; as MAME's mazinger_decrypt_cb has it. The board's 4 MB
                // of sprite memory, FF where there is no ROM, is scrambled as a whole)
                packed.resize(0x400000, 0xFF);
                static constexpr uint8_t kFrom[24] = {0, 12, 10, 1, 13, 6, 8, 11, 5, 16, 2, 18, 14, 17, 4, 15, 3, 7, 9, 19, 20, 21, 22, 23};
                std::vector<uint8_t> clear(packed.size());
                for (uint32_t i = 0; i < packed.size(); i++) {
                    uint32_t from = 0;
                    for (int bit = 0; bit < 24; bit++) {
                        from |= ((i >> kFrom[bit]) & 1u) << bit;
                    }
                    clear[i ^ 0xDF88] = packed[from];
                }
                packed.swap(clear);
            }
            size_t size = 1;
            while (size < packed.size() * 2) {
                size <<= 1;
            }
            const bool swapped = value("sprites_format") == "swapped";
            board.video.spritePixels.assign(size, 0);
            for (size_t n = 0; n < packed.size(); n++) {
                board.video.spritePixels[n * 2 + (swapped ? 1 : 0)] = packed[n] >> 4;
                board.video.spritePixels[n * 2 + (swapped ? 0 : 1)] = packed[n] & 15;
            }
        }
        board.video.spritesZoom = value("sprites_zoom") == "1";
        board.video.spriteColourStep = number("sprites_colour_step", 256);
        // Where the layers' colours sit in the palette (the first of them, and the step from one to the next), and
        // the pen shown where nothing is
        const uint32_t layerBase = number("layer_colour_base", 0x4000), layerStep = number("layer_colour_step", 256);
        board.video.backgroundPen = number("background_pen", 0x7F00);
        // The layers' tiles, 8 x 8: 4 bits a pixel (32 bytes a tile), or 8 (64 bytes: the two halves of a pixel in
        // the same half of two bytes side by side)
        board.video.layers = 0;
        for (int n = 0; n < CaveVideo::kLayers && missing.empty(); n++) {
            const std::string key = "layer" + std::to_string(n);
            if (value(key.c_str()).empty()) {
                break;
            }
            if (!joined(key.c_str(), packed)) {
                break;
            }
            CaveVideo::LayerKind &kind = board.video.kinds[n];
            const int bits = atoi(value((key + "_bits").c_str()).c_str());
            // (`layerN_from`: the board has only the file's bytes from there on)
            if (const uint32_t from = number((key + "_from").c_str(), 0); from && from < packed.size()) {
                packed.erase(packed.begin(), packed.begin() + from);
            }
            std::vector<uint8_t> high;
            if (!value((key + "_high").c_str()).empty()) {
                if (!joined((key + "_high").c_str(), high)) {
                    break;
                }
                if (const uint32_t from = number((key + "_high_from").c_str(), 0); from && from < high.size()) {
                    high.erase(high.begin(), high.begin() + from);
                }
            }
            if (bits == 6 && !high.empty()) {
                // 6 bits a pixel in two ROMs: 4 bits as the 4-bit tiles have them, and 2 more for each pixel in the
                // other (`layerN_high`), four pixels a byte from its low bits up
                kind.pixels.resize(packed.size() * 2);
                for (size_t at = 0; at < kind.pixels.size(); at++) {
                    const uint8_t four = at & 1 ? packed[at >> 1] & 15 : packed[at >> 1] >> 4;
                    const uint8_t two = at >> 2 < high.size() ? (high[at >> 2] >> ((at & 3) * 2)) & 3 : 0;
                    kind.pixels[at] = static_cast<uint8_t>((two << 4) | four);
                }
            } else if (bits == 6) {
                // 6 bits a pixel laid out as 8 are, the upper half's low two bits not there
                kind.pixels.resize(packed.size());
                for (size_t at = 0; at + 1 < packed.size(); at += 2) {
                    kind.pixels[at] = static_cast<uint8_t>(((packed[at + 1] >> 2) & 0x30) | (packed[at] >> 4));
                    kind.pixels[at + 1] = static_cast<uint8_t>(((packed[at + 1] << 2) & 0x30) | (packed[at] & 15));
                }
            } else if (bits == 8) {
                kind.pixels.resize(packed.size());
                for (size_t at = 0; at + 1 < packed.size(); at += 2) {
                    kind.pixels[at] = static_cast<uint8_t>((packed[at + 1] & 0xF0) | (packed[at] >> 4));
                    kind.pixels[at + 1] = static_cast<uint8_t>(((packed[at + 1] & 15) << 4) | (packed[at] & 15));
                }
            } else {
                kind.pixels.resize(packed.size() * 2);
                for (size_t at = 0; at < packed.size(); at++) {
                    kind.pixels[at * 2] = packed[at] >> 4;
                    kind.pixels[at * 2 + 1] = packed[at] & 15;
                }
            }
            kind.only8 = value((key + "_tiles").c_str()) == "8";
            // (a layer's own place in the palette, where the layers differ; 6-bit tiles have 16 colours of 64 entries)
            kind.colourBase = number((key + "_colour_base").c_str(), layerBase);
            kind.colourStep = number((key + "_colour_step").c_str(), bits == 6 ? 64 : layerStep);
            kind.colourMask = bits == 6 ? 0x0F : 0x3F;
            kind.banked = value((key + "_banked").c_str()) == "1";
            board.video.layers = n + 1;
        }
        board.video.left = static_cast<int>(number("screen_left", 0));
        board.video.spritesDelay = value("sprites_delay") == "1";
        if (!missing.empty() || !joined("sound", board.soundRom)) {
            continue;
        }
        // (a board with a Z80 for sound: its program; Mazinger Z's second ROM of data, 16-bit words low byte first)
        board.z80.rom.clear();
        if (!value("z80").empty() && !joined("z80", board.z80.rom)) {
            continue;
        }
        board.extraRom.clear();
        if (!value("program2").empty()) {
            if (!joined("program2", board.extraRom)) {
                continue;
            }
            SwapWords(board.extraRom.data(), board.extraRom.size());
        }
        board.soundRom2.clear();
        if (!value("sound2").empty() && !joined("sound2", board.soundRom2)) {
            continue;
        }
        if (!value("eeprom").empty()) {
            // (not needed: without it the game writes its own)
            std::string name;
            uint32_t crc = 0;
            std::vector<uint8_t> data;
            if (ParseFile(value("eeprom"), name, crc) && source.FindAny(crc, data) && data.size() == 128) {
                for (int n = 0; n < 64; n++) {
                    board.eeprom.words[n] = static_cast<uint16_t>((data[n * 2] << 8) | data[n * 2 + 1]);
                }
            }
        }
        if (!board.SetBoard(value("board"))) {
            error = "game.cfg names a board this build does not know: " + value("board");
            return false;
        }
        board.Reset();
        return true;
    }
    missing = first.empty() ? missing : first;
    error = missing.empty() ? "no " + set + " set in " + where : "the " + set + " set in " + where + " has no " + missing + " with the checksum expected" + ParentsNeeded(value("rom_parent"));
    return false;
}

} // namespace spelunk
