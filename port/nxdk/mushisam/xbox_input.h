#pragma once

// Native Original Xbox USB XID gamepad driver (replaces SDL2 GameController).
#include <usbh_lib.h>
#include <xid_driver.h>

#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

struct PadState {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool start = false;
    bool back = false;
    bool white = false; // LB
    bool black = false; // RB
    bool l3 = false;
    bool r3 = false;
};

class XboxInput {
public:
    void Init() {
        s_instance = this;
        padDev_ = nullptr;
        hasPacket_ = false;
        std::memset(const_cast<xid_gamepad_in *>(&raw_), 0, sizeof(raw_));

        usbh_core_init();
        usbh_xid_init();
        usbh_install_xid_conn_callback(&OnConnect, &OnDisconnect);

        // Attach any already-enumerated gamepad.
        for (xid_dev_t *dev = usbh_xid_get_device_list(); dev != nullptr; dev = dev->next) {
            if (dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER) {
                Attach(dev);
                break;
            }
        }
    }

    void Shutdown() {
        padDev_ = nullptr;
        hasPacket_ = false;
        usbh_core_deinit();
        s_instance = nullptr;
    }

    void Poll(PadState &out) {
        // Only poll USB OHCI hub registers every 60 frames once a controller is attached;
        // live button packets arrive automatically via OHCI interrupt callback OnReadComplete().
        if (!padDev_ || (++hubPollDiv_ >= 60u)) {
            hubPollDiv_ = 0;
            usbh_pooling_hubs();
        }
        out = {};
        if (!padDev_ || !hasPacket_) {
            return;
        }

        xid_gamepad_in pkt;
        std::memcpy(&pkt, const_cast<const xid_gamepad_in *>(&raw_), sizeof(pkt));

        constexpr uint16_t kDpadUp = 0x0001;
        constexpr uint16_t kDpadDown = 0x0002;
        constexpr uint16_t kDpadLeft = 0x0004;
        constexpr uint16_t kDpadRight = 0x0008;
        constexpr uint16_t kStart = 0x0010;
        constexpr uint16_t kBack = 0x0020;
        constexpr uint16_t kL3 = 0x0040;
        constexpr uint16_t kR3 = 0x0080;
        constexpr uint8_t kAnalogThreshold = 0x20;
        constexpr int16_t kStickDeadzone = 8000;

        out.up = ((pkt.dButtons & kDpadUp) != 0) || (pkt.leftStickY > kStickDeadzone);
        out.down = ((pkt.dButtons & kDpadDown) != 0) || (pkt.leftStickY < -kStickDeadzone);
        out.left = ((pkt.dButtons & kDpadLeft) != 0) || (pkt.leftStickX < -kStickDeadzone);
        out.right = ((pkt.dButtons & kDpadRight) != 0) || (pkt.leftStickX > kStickDeadzone);
        out.start = (pkt.dButtons & kStart) != 0;
        out.back = (pkt.dButtons & kBack) != 0;
        out.l3 = (pkt.dButtons & kL3) != 0;
        out.r3 = (pkt.dButtons & kR3) != 0;
        out.a = pkt.a > kAnalogThreshold;
        out.b = pkt.b > kAnalogThreshold;
        out.x = pkt.x > kAnalogThreshold;
        out.y = pkt.y > kAnalogThreshold;
        out.white = pkt.white > kAnalogThreshold;
        out.black = pkt.black > kAnalogThreshold;
    }

private:
    void Attach(xid_dev_t *dev) {
        if (!dev || padDev_ != nullptr) {
            return;
        }
        padDev_ = dev;
        hasPacket_ = false;
        dev->user_data = this;
        usbh_xid_read(dev, 0, reinterpret_cast<void *>(&OnReadComplete));
    }

    static void OnConnect(xid_dev_t *dev, int /*status*/) {
        if (s_instance && dev && dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER) {
            s_instance->Attach(dev);
        }
    }

    static void OnDisconnect(xid_dev_t *dev, int /*status*/) {
        if (s_instance && s_instance->padDev_ == dev) {
            s_instance->padDev_ = nullptr;
            s_instance->hasPacket_ = false;
        }
    }

    static void OnReadComplete(UTR_T *utr) {
        if (!utr || utr->status < 0) {
            return;
        }
        auto *dev = static_cast<xid_dev_t *>(utr->context);
        if (!dev || !dev->user_data) {
            return;
        }
        auto *self = static_cast<XboxInput *>(dev->user_data);
        if (dev != self->padDev_) {
            return;
        }
        if (utr->xfer_len >= sizeof(xid_gamepad_in)) {
            std::memcpy(const_cast<xid_gamepad_in *>(&self->raw_), utr->buff, sizeof(xid_gamepad_in));
            self->hasPacket_ = true;
        }
        utr->xfer_len = 0;
        utr->bIsTransferDone = 0;
        usbh_int_xfer(utr);
    }

    static inline XboxInput *s_instance = nullptr;
    // Separate USB OHCI interrupt-written packet from game-thread pointer using explicit padding (no alignas(32) heap movaps).
    xid_dev_t *padDev_ = nullptr;
    uint8_t padLine1_[28] = {};
    volatile bool hasPacket_ = false;
    uint8_t padLine2_[31] = {};
    uint32_t hubPollDiv_ = 0;
    volatile xid_gamepad_in raw_{};
};

} // namespace spelunk_xbox
