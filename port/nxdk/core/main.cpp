// Standalone link, ROM-load, and frame-execution smoke test for the Cave 68000 core.
#include "machine/cave68k.h"
#include "nv2a_presenter.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <SDL.h>
#include <windows.h>

#include <cstdio>
#include <vector>

namespace {

bool ReadFileBytes(const char *path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return n == out.size();
}

void SwapWords(uint8_t *data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

bool LoadDdonpachRoms(spelunk::Cave68k &board, const char *dir) {
    auto path = [&](const char *name) {
        return std::string(dir) + "\\" + name;
    };

    std::vector<uint8_t> even, odd;
    if (!ReadFileBytes(path("b1.u27").c_str(), even) ||
        !ReadFileBytes(path("b2.u26").c_str(), odd) ||
        even.size() != odd.size()) {
        return false;
    }
    board.program.resize(even.size() * 2);
    for (size_t i = 0; i < even.size(); ++i) {
        board.program[i * 2] = even[i];
        board.program[i * 2 + 1] = odd[i];
    }

    std::vector<uint8_t> packed;
    for (const char *sp : {"u50.bin", "u51.bin", "u52.bin", "u53.bin"}) {
        std::vector<uint8_t> part;
        if (!ReadFileBytes(path(sp).c_str(), part)) {
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

    board.video.layers = 3;
    for (int n = 0; n < 3; ++n) {
        const char *lfile = (n == 0) ? "u60.bin" : (n == 1) ? "u61.bin" : "u62.bin";
        if (!ReadFileBytes(path(lfile).c_str(), packed)) {
            return false;
        }
        auto &kind = board.video.kinds[n];
        if (n == 2) {
            kind.pixels.resize(packed.size());
            for (size_t at = 0; at + 1 < packed.size(); at += 2) {
                kind.pixels[at] = static_cast<uint8_t>((packed[at + 1] & 0xF0) | (packed[at] >> 4));
                kind.pixels[at + 1] = static_cast<uint8_t>(((packed[at + 1] & 15) << 4) | (packed[at] & 15));
            }
            kind.only8 = true;
        } else {
            kind.pixels.resize(packed.size() * 2);
            for (size_t at = 0; at < packed.size(); ++at) {
                kind.pixels[at * 2] = packed[at] >> 4;
                kind.pixels[at * 2 + 1] = packed[at] & 15;
            }
            kind.only8 = false;
        }
        kind.colourBase = 0x4000;
        kind.colourStep = 256;
        kind.colourMask = 0x3F;
    }

    board.soundRom.clear();
    for (const char *sf : {"u6.bin", "u7.bin"}) {
        std::vector<uint8_t> part;
        if (!ReadFileBytes(path(sf).c_str(), part)) {
            return false;
        }
        board.soundRom.insert(board.soundRom.end(), part.begin(), part.end());
    }

    std::vector<uint8_t> eep;
    if (ReadFileBytes(path("eeprom-ddonpach.bin").c_str(), eep) && eep.size() == 128) {
        for (int i = 0; i < 64; ++i) {
            board.eeprom.words[i] = static_cast<uint16_t>((eep[i * 2] << 8) | eep[i * 2 + 1]);
        }
    }
    return true;
}

} // namespace

int main(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    spelunk::Cave68k board;
    if (!board.SetBoard("ddonpach")) {
        debugPrint("Cave 68000 core: board setup failed\n");
        return 1;
    }

    if (!LoadDdonpachRoms(board, "D:\\ddonpach")) {
        debugPrint("Cave 68000 core: ROM load failed\n");
        while (true) {
            Sleep(1000);
        }
    }

    debugPrint("Cave 68000 core: ROMs loaded (prog=%uKB sprites=%uKB)\n",
               static_cast<unsigned>(board.program.size() / 1024),
               static_cast<unsigned>(board.video.spritePixels.size() / 1024));
    board.Reset();

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
