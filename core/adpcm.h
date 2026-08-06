// Y8950 (MZ-1E35 ADPCM board) wrapper around ymfm, synchronized to the CPU
// cycle clock - the same shape as core/opn.h.
//
// The chip runs at 3.58 MHz; ymfm generates at clock/72 (~49.7 kHz).
// Samples are produced lazily whenever the CPU touches a register and at
// every frame end, then decimated to the host rate through a 4th-order
// Butterworth low-pass and linear interpolation.
//
// Differences from the OPN wrapper:
//  - OPL timers matter here (software polls status bits 6/5 with no IRQ
//    line wired on the MZ-2500), so ymfm_set_timer is honoured: deadlines
//    are kept in CPU cycles and fired from flush_to()/read_status().
//  - The delta-T ADPCM engine reads and writes external RAM through the
//    ymfm interface. The MZ-1E35's RAM size is not in the primary I/O map;
//    32KB is the provisional figure (see the design doc) and lives in one
//    constant.
#pragma once

#include <cstdint>
#include <vector>

#include "ymfm/ymfm_opl.h"

namespace mz {

class AdpcmY8950 : public ymfm::ymfm_interface {
public:
    static constexpr uint32_t ADPCM_RAM_SIZE = 32 * 1024;

    AdpcmY8950();

    void reset();
    void set_output_rate(uint32_t rate);
    uint32_t output_rate() const { return out_rate_; }

    uint8_t read_status(uint64_t now);
    uint8_t read_data(uint64_t now);
    void write_address(uint8_t value, uint64_t now);
    void write_data(uint8_t value, uint64_t now);

    // generate chip samples up to the given CPU time and feed the resampler
    void flush_to(uint64_t now);

    // drain up to max_samples mono float samples; returns the count
    size_t read_audio(float* out, size_t max_samples);

    // drop buffered output (used while the board is pulled from the slot,
    // so an undrained ring cannot grow without bound)
    void discard_audio() { ring_.clear(); ring_read_ = 0; }
    // buffered samples not yet drained (test visibility)
    size_t pending_audio() const { return ring_.size() - ring_read_; }

    // ymfm_interface
    void ymfm_set_busy_end(uint32_t clocks) override {
        busy_end_ = now_ + clocks_to_cycles(clocks);
    }
    bool ymfm_is_busy() override { return now_ < busy_end_; }
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override;
    void ymfm_external_write(ymfm::access_class type, uint32_t address,
                             uint8_t data) override;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void design_lowpass(double fs, double fc, double q);
        inline float run(float x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return (float)y;
        }
        void clear() { x1 = x2 = y1 = y2 = 0; }
    };

    static uint64_t clocks_to_cycles(uint64_t clocks);
    void fire_timers(uint64_t now);
    void design_filters();
    void push_chip_sample(float mono);

    ymfm::y8950 chip_;
    uint32_t chip_rate_ = 1;
    uint64_t generated_ = 0;
    uint64_t now_ = 0;
    uint64_t busy_end_ = 0;
    static constexpr uint64_t NEVER = ~0ULL;
    uint64_t timer_due_[2] = {NEVER, NEVER}; // CPU-cycle deadlines

    std::vector<uint8_t> adpcm_ram_ = std::vector<uint8_t>(ADPCM_RAM_SIZE, 0);
    // GPIO (reg 19h) and keyboard-port (regs 05h/06h) latches: nothing is
    // attached to either, so writes are held and read back; FFh = open bus
    uint8_t io_latch_[2] = {0xFF, 0xFF};

    Biquad lp1_, lp2_;
    uint32_t out_rate_ = 44100;
    double src_pos_ = 0.0;
    float prev_sample_ = 0.0f;
    std::vector<float> ring_;
    size_t ring_read_ = 0;
};

} // namespace mz
