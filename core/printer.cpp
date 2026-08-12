#include "core/printer.h"

namespace mz {

void PrinterPort::reset(uint64_t now) {
    control_ = 0xC0;
    data_ = 0xFF;
    busy_ = false;
    ack_high_ = true;
    interrupt_ = false;
    busy_until_ = now;
    ack_until_ = now;
}

void PrinterPort::advance(uint64_t now) {
    if (busy_ && now >= busy_until_) {
        busy_ = false;
        ack_high_ = false;
        ack_until_ = busy_until_ + ACK_CYCLES;
        interrupt_ = true;
    }
    if (!ack_high_ && now >= ack_until_) ack_high_ = true;
}

uint8_t PrinterPort::read_control(uint64_t now) {
    advance(now);
    // FEh bits 3 and 2 are documented fixed ones. Bit 1 is STA (the
    // Centronics ACK line) and bit 0 is BUSY. The output latch is returned
    // for the two write bits so diagnostics can observe its own setting.
    return static_cast<uint8_t>((control_ & 0xC0) | 0x0C |
                                (ack_high_ ? 0x02 : 0x00) |
                                ((busy_ || !online_) ? 0x01 : 0x00));
}

void PrinterPort::write_control(uint8_t value, uint64_t now) {
    advance(now);
    const bool old_stb_high = (control_ & 0x80) != 0;
    const bool old_prim_high = (control_ & 0x40) != 0;
    control_ = value & 0xC0;
    const bool stb_high = (control_ & 0x80) != 0;
    const bool prim_high = (control_ & 0x40) != 0;

    // PRIM is active low. It resets the peripheral handshake without
    // deleting host-captured output that was already accepted.
    if (old_prim_high && !prim_high) {
        busy_ = false;
        ack_high_ = true;
        interrupt_ = false;
        busy_until_ = now;
        ack_until_ = now;
        return;
    }

    // Centronics data is accepted on the falling edge of active-low STB.
    if (old_stb_high && !stb_high && prim_high && online_ && !busy_) {
        if (output_.size() < MAX_OUTPUT) output_.push_back(data_);
        else dropped_bytes_++;
        dirty_ = true;
        busy_ = true;
        ack_high_ = true;
        interrupt_ = false;
        busy_until_ = now + BUSY_CYCLES;
        ack_until_ = busy_until_ + ACK_CYCLES;
    }
}

bool PrinterPort::consume_interrupt() {
    const bool value = interrupt_;
    interrupt_ = false;
    return value;
}

void PrinterPort::set_online(bool online, uint64_t now) {
    advance(now);
    online_ = online;
    if (!online_) {
        busy_ = false;
        ack_high_ = true;
        interrupt_ = false;
    }
}

void PrinterPort::clear_output() {
    output_.clear();
    dropped_bytes_ = 0;
    dirty_ = false;
}

} // namespace mz
