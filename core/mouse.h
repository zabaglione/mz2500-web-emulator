// MZ-1X10 mouse. The machine's driver strobes DTR on SIO channel B and
// expects three bytes back: buttons, then signed X and Y movement. The
// protocol was measured off the firmware's own driver - see
// web_emulator/docs/mouse-protocol.md.
//
// Movement arrives from the host at whatever rate and resolution the host
// has; what one packet can carry is a signed byte. Anything over that is
// kept and sent in the packets that follow, so no motion is thrown away.
// Scaling belongs to the host input path, not here: the machine has its own
// ratio setting (BASIC `mouse 3,...`) which is the software's to choose.
#pragma once

#include <cstdint>

namespace mz {

class Mouse {
public:
    void move(int dx, int dy) {
        pending_x_ += dx;
        pending_y_ += dy;
    }

    void set_button(int index, bool down) {
        const uint8_t bit = (uint8_t)(1 << (index & 1));
        if (down) buttons_ |= bit;
        else buttons_ &= (uint8_t)~bit;
    }

    // Fill one packet. An idle mouse answers with zeros, which is what the
    // driver sees from real hardware between movements.
    void take_packet(uint8_t out[3]) {
        out[0] = buttons_;
        out[1] = (uint8_t)(int8_t)take(pending_x_);
        out[2] = (uint8_t)(int8_t)take(pending_y_);
    }

    void reset() {
        pending_x_ = pending_y_ = 0;
        buttons_ = 0;
    }

private:
    static int take(int& pending) {
        int step = pending;
        if (step > 127) step = 127;
        if (step < -128) step = -128;
        pending -= step;
        return step;
    }

    int pending_x_ = 0;
    int pending_y_ = 0;
    uint8_t buttons_ = 0;
};

} // namespace mz
