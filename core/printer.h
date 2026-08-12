// MZ-2500 parallel printer interface (ports FEh/FFh).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mz {

class PrinterPort {
public:
    // The board documentation names the handshake pins but gives no timing.
    // These short, observable pulses use ordinary Centronics-scale timing at
    // the MZ-2500's 6 MHz CPU clock: 5 us busy followed by a 5 us ACK pulse.
    static constexpr uint64_t BUSY_CYCLES = 30;
    static constexpr uint64_t ACK_CYCLES = 30;
    static constexpr size_t MAX_OUTPUT = 8 * 1024 * 1024;

    PrinterPort() = default;

    // RESET affects the interface latches and handshake only. Bytes already
    // accepted by an external printer are not pulled back from the paper.
    void reset(uint64_t now = 0);

    uint8_t read_control(uint64_t now);
    void write_control(uint8_t value, uint64_t now);
    void write_data(uint8_t value) { data_ = value; }
    void advance(uint64_t now);

    bool consume_interrupt();
    void set_online(bool online, uint64_t now);
    bool online() const { return online_; }

    const std::vector<uint8_t>& output() const { return output_; }
    uint64_t dropped_bytes() const { return dropped_bytes_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }
    void clear_output();

private:
    uint8_t control_ = 0xC0; // STB and PRIM inactive (high)
    uint8_t data_ = 0xFF;
    bool online_ = true;
    bool busy_ = false;
    bool ack_high_ = true;   // FEh bit1, named STA in the MZ I/O map
    bool interrupt_ = false;
    uint64_t busy_until_ = 0;
    uint64_t ack_until_ = 0;
    std::vector<uint8_t> output_;
    uint64_t dropped_bytes_ = 0;
    bool dirty_ = false;
};

} // namespace mz
