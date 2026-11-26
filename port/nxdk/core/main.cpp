// Standalone link, ROM-load, and frame-execution smoke test for the Cave 68000 core.
#include "machine/cave68k.h"
#include "nv2a_presenter.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <SDL.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstdarg>
#include <vector>

namespace {

std::vector<uint8_t> ReadFileBytes(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return {};
    }
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(n);
    return out;
}

inline uint8_t InB(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

inline void OutB(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline void SerialWriteByte(uint8_t val) {
    for (int spin = 0; spin < 10000; ++spin) {
        if ((InB(0x3F8 + 5) & 0x20) != 0) {
            break;
        }
    }
    OutB(0x3F8, val);
}

void SerialPrintf(const char* fmt, ...) {
    static bool uartInit = false;
    if (!uartInit) {
        uartInit = true;
        // Enable COM1 (0x3F8) in LPC47M157 SuperIO
        OutB(0x2E, 0x55);
        OutB(0x2E, 0x07); OutB(0x2F, 0x04); // LDN 4: Serial Port 1
        OutB(0x2E, 0x60); OutB(0x2F, 0x03); // Base high 0x03
        OutB(0x2E, 0x61); OutB(0x2F, 0xF8); // Base low 0xF8
        OutB(0x2E, 0x70); OutB(0x2F, 0x04); // IRQ 4
        OutB(0x2E, 0x30); OutB(0x2F, 0x01); // Activate
        OutB(0x2E, 0xAA);
        OutB(0x3F8 + 1, 0x00);
        OutB(0x3F8 + 3, 0x80);
        OutB(0x3F8 + 0, 0x01);
        OutB(0x3F8 + 1, 0x00);
        OutB(0x3F8 + 3, 0x03);
        OutB(0x3F8 + 2, 0xC7);
        OutB(0x3F8 + 4, 0x0B);
    }
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    DbgPrint("%s", buf);
    for (const char* p = buf; *p; ++p) {
        if (*p == '\n') {
            SerialWriteByte('\r');
        }
        SerialWriteByte(static_cast<uint8_t>(*p));
    }
}

void SwapWords(uint8_t *data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

bool LoadDdonpachRoms(spelunk::Cave68k &board) {
    const auto even = ReadFileBytes("D:\\ddonpach\\b1.u27");
    const auto odd = ReadFileBytes("D:\\ddonpach\\b2.u26");
    if (even.empty() || even.size() != odd.size()) {
        SerialPrintf("LoadDdonpachRoms: failed b1.u27 (%zu) / b2.u26 (%zu)\n", even.size(), odd.size());
        return false;
    }
    board.program.resize(even.size() * 2);
    for (size_t i = 0; i < even.size(); ++i) {
        board.program[i * 2] = even[i];
        board.program[i * 2 + 1] = odd[i];
    }

    const char *spNames[] = {
        "D:\\ddonpach\\u50.bin",
        "D:\\ddonpach\\u51.bin",
        "D:\\ddonpach\\u52.bin",
        "D:\\ddonpach\\u53.bin",
    };
    std::vector<uint8_t> packed;
    for (const char *name : spNames) {
        auto part = ReadFileBytes(name);
        if (part.empty()) {
            SerialPrintf("LoadDdonpachRoms: failed %s\n", name);
            return false;
        }
        packed.insert(packed.end(), part.begin(), part.end());
    }
    SwapWords(packed.data(), packed.size());
    size_t spSize = 1;
    while (spSize < packed.size() * 2) {
        spSize <<= 1;
    }
    board.video.spritePixels.assign(spSize, 0);
    for (size_t i = 0; i < packed.size(); ++i) {
        board.video.spritePixels[i * 2] = packed[i] >> 4;
        board.video.spritePixels[i * 2 + 1] = packed[i] & 15;
    }

    const char *layerNames[] = {
        "D:\\ddonpach\\u60.bin",
        "D:\\ddonpach\\u61.bin",
        "D:\\ddonpach\\u62.bin",
    };
    board.video.layers = 3;
    for (int n = 0; n < 3; ++n) {
        const auto layer = ReadFileBytes(layerNames[n]);
        if (layer.empty()) {
            SerialPrintf("LoadDdonpachRoms: failed %s\n", layerNames[n]);
            return false;
        }
        auto &kind = board.video.kinds[n];
        if (n == 2) {
            kind.pixels.resize(layer.size());
            for (size_t at = 0; at + 1 < layer.size(); at += 2) {
                kind.pixels[at] = static_cast<uint8_t>((layer[at + 1] & 0xF0) | (layer[at] >> 4));
                kind.pixels[at + 1] = static_cast<uint8_t>(((layer[at + 1] & 15) << 4) | (layer[at] & 15));
            }
            kind.only8 = true;
        } else {
            kind.pixels.resize(layer.size() * 2);
            for (size_t at = 0; at < layer.size(); ++at) {
                kind.pixels[at * 2] = layer[at] >> 4;
                kind.pixels[at * 2 + 1] = layer[at] & 15;
            }
            kind.only8 = false;
        }
        kind.colourBase = 0x4000;
        kind.colourStep = 256;
        kind.colourMask = 0x3F;
    }

    board.soundRom.clear();
    const char *soundNames[] = {
        "D:\\ddonpach\\u6.bin",
        "D:\\ddonpach\\u7.bin",
    };
    for (const char *name : soundNames) {
        auto part = ReadFileBytes(name);
        if (part.empty()) {
            SerialPrintf("LoadDdonpachRoms: failed %s\n", name);
            return false;
        }
        board.soundRom.insert(board.soundRom.end(), part.begin(), part.end());
    }

    const auto eep = ReadFileBytes("D:\\ddonpach\\eeprom-ddonpach.bin");
    if (eep.size() == 128) {
        for (int i = 0; i < 64; ++i) {
            board.eeprom.words[i] = static_cast<uint16_t>((eep[i * 2] << 8) | eep[i * 2 + 1]);
        }
    }
    return true;
}

} // namespace

int main(void) {
    SerialPrintf("Cave 68000 core: main() entered\n");
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    spelunk::Cave68k board;
    if (!LoadDdonpachRoms(board)) {
        debugPrint("Cave 68000 core: ROM load failed\n");
        while (true) {
            Sleep(1000);
        }
    }

    // SetBoard() maps m_parts[0..N] from board.program.size(), so it MUST run after LoadDdonpachRoms().
    if (!board.SetBoard("ddonpach")) {
        debugPrint("Cave 68000 core: board setup failed\n");
        SerialPrintf("Cave 68000 core: board setup failed\n");
        while (true) {
            Sleep(1000);
        }
    }

    debugPrint("Cave 68000 core: ROMs loaded (prog=%uKB sprites=%uKB)\n",
               static_cast<unsigned>(board.program.size() / 1024),
               static_cast<unsigned>(board.video.spritePixels.size() / 1024));
    SerialPrintf("Cave 68000 core: ROMs loaded (prog=%uKB sprites=%uKB)\n",
             static_cast<unsigned>(board.program.size() / 1024),
             static_cast<unsigned>(board.video.spritePixels.size() / 1024));
    board.Reset();

    // Fast-forward initial black/POST frames so video appears immediately on boot.
    board.noPicture = true;
    for (int bootFrame = 0; bootFrame < 300; ++bootFrame) {
        board.RunFrame();
        board.SoundOut().clear();
    }
    board.noPicture = false;
    board.Redraw();

    if (pb_init() != 0) {
        debugPrint("Cave 68000 core: pb_init failed\n");
        while (true) {
            Sleep(1000);
        }
    }
    pb_show_front_screen();

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        debugPrint("Cave 68000 core: SDL_Init failed: %s\n", SDL_GetError());
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16LSB;
    want.channels = 1;
    want.samples = 1024;
    want.callback = nullptr;
    SDL_AudioDeviceID audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDev != 0) {
        SDL_PauseAudioDevice(audioDev, 0);
    } else {
        debugPrint("Cave 68000 core: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    }
    SDL_GameController *pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) {
                break;
            }
        }
    }

    spelunk_xbox::Nv2aPresenter presenter;
    if (!presenter.Init()) {
        debugPrint("Cave 68000 core: NV2A presenter init failed\n");
        while (true) {
            Sleep(1000);
        }
    }

    bool tateMode = false;
    bool bilinear = false;
    bool integerScale = false;
    bool prevRb = false;
    bool prevLb = false;
    bool prevL3 = false;

    while (true) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
                if (pad == SDL_GameControllerFromInstanceID(e.cdevice.which)) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
            }
        }
        SDL_GameControllerUpdate();

        uint32_t p1 = 0;
        uint32_t sys = 0;
        if (pad) {
            const int16_t lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            const int16_t ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -8000) p1 |= spelunk::kInUp;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 8000) p1 |= spelunk::kInDown;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -8000) p1 |= spelunk::kInLeft;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 8000) p1 |= spelunk::kInRight;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) p1 |= spelunk::kInButton1;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) p1 |= spelunk::kInButton2;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) p1 |= spelunk::kInButton3;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) p1 |= spelunk::kInButton4;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) sys |= spelunk::kSysCoin1;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) sys |= spelunk::kSysStart1;
            const bool rb = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
            if (rb && !prevRb) {
                tateMode = !tateMode;
            }
            prevRb = rb;
            const bool lb = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            if (lb && !prevLb) {
                bilinear = !bilinear;
            }
            prevLb = lb;
            const bool l3 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
            if (l3 && !prevL3) {
                integerScale = !integerScale;
            }
            prevL3 = l3;
        }
        board.inputs[0] = p1;
        board.system = sys;

        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();
        pb_fill(0, 0, 640, 480, 0xFF000000);

        board.RunFrame();
        auto &pcm = board.SoundOut();
        if (audioDev != 0 && !pcm.empty()) {
            if (SDL_GetQueuedAudioSize(audioDev) < 44100 * sizeof(int16_t) / 4) {
                SDL_QueueAudio(audioDev, pcm.data(), static_cast<Uint32>(pcm.size() * sizeof(int16_t)));
            }
        }
        pcm.clear();
        board.Picture(presenter.TexBuffer());
        static uint32_t frameNum = 0;
        ++frameNum;
        if (frameNum == 1 || frameNum == 10 || (frameNum % 60) == 0) {
            const uint32_t *fb = presenter.TexBuffer();
            const int w = spelunk_xbox::Nv2aPresenter::kSrcWidth;
            const int h = spelunk_xbox::Nv2aPresenter::kSrcHeight;
            uint32_t nonBlack = 0;
            uint32_t hash = 2166136261u;
            for (int i = 0; i < w * h; ++i) {
                const uint32_t px = fb[i];
                if ((px & 0x00FFFFFFu) != 0) {
                    ++nonBlack;
                }
                hash ^= px;
                hash *= 16777619u;
            }
            SerialPrintf("FB frame=%u nonBlack=%u/%d hash=0x%08X\n",
                     frameNum, nonBlack, w * h, hash);
            static const char kRamp[] = " .:-=+*#%@";
            for (int y = 0; y < h; y += 20) {
                char row[33];
                int col = 0;
                for (int x = 0; x < w && col < 32; x += 10) {
                    const uint32_t px = fb[y * w + x];
                    const uint32_t r = px & 0xFFu;
                    const uint32_t g = (px >> 8) & 0xFFu;
                    const uint32_t b = (px >> 16) & 0xFFu;
                    const uint32_t luma = (r * 3u + g * 6u + b) / 10u;
                    row[col++] = kRamp[(luma * 9u) / 255u];
                }
                row[col] = '\0';
                SerialPrintf("FB[%03d]: %s\n", y, row);
            }
        }

        presenter.Present(tateMode, bilinear, integerScale);
        while (pb_busy()) {
        }
        while (pb_finished()) {
        }
    }

    presenter.Shutdown();

    if (audioDev != 0) {
        SDL_CloseAudioDevice(audioDev);
    }
    if (pad) {
        SDL_GameControllerClose(pad);
    }
    SDL_Quit();

    pb_kill();
    return 0;
}
