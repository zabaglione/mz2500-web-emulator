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
//    ymfm interface. The MZ-1E35's populated RAM size is not established by
//    the available primary sources, so 32KB remains the selectable default,
//    not a claimed board fact.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ymfm/ymfm_opl.h"

namespace mz {

class AdpcmY8950 : public ymfm::ymfm_interface {
public:
    static constexpr uint32_t ADPCM_RAM_SIZE = 32 * 1024;
    static constexpr uint32_t MIN_ADPCM_RAM_SIZE = 8 * 1024;
    static constexpr uint32_t MAX_ADPCM_RAM_SIZE = 256 * 1024;

    AdpcmY8950();

    // Reset chip/timer/audio/I/O state. External board RAM is provisionally
    // retained until its physical RESET wiring is documented or measured.
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

    // Y8950 4-bit GPIO. Register 18h controls direction per pin (1=output,
    // 0=input); register 19h reads physical pins. Inputs default high/open.
    void set_gpio_inputs(uint8_t value) { gpio_input_ = value & 0x0F; }
    uint8_t gpio_direction() const { return gpio_direction_; }
    uint8_t gpio_output_pins() const {
        return static_cast<uint8_t>(gpio_latch_ & gpio_direction_ & 0x0F);
    }
    uint8_t gpio_pins() const {
        return static_cast<uint8_t>((gpio_latch_ & gpio_direction_) |
                                    (gpio_input_ & ~gpio_direction_)) & 0x0F;
    }

    // Host analogue input for Y8950 AD conversion. Samples are mono floats
    // in [-1,+1], resampled by the converter's NPRE cadence.
    size_t queue_adc_samples(const float* samples, size_t count, uint32_t rate);
    void clear_adc_samples();
    bool adc_enabled() const { return adc_enabled_; }

    // MZ-1E35 board population remains undocumented. Expose every capacity
    // the Y8950 itself supports, keeping 32KB only as the compatibility
    // default instead of presenting it as an established board fact.
    bool set_adpcm_ram_size(uint32_t size);
    uint32_t adpcm_ram_size() const {
        return static_cast<uint32_t>(adpcm_ram_.size());
    }
    void set_mix_gain(float gain) { mix_gain_ = gain < 0.0f ? 0.0f : gain; }
    float mix_gain() const { return mix_gain_; }

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
    void design_filters();
    void generate_to(uint64_t now);
    void push_chip_sample(float mono);
    void update_adc_to(uint64_t now);

    ymfm::y8950 chip_;
    uint32_t chip_rate_ = 1;
    uint64_t generated_ = 0;
    uint64_t now_ = 0;
    uint64_t busy_end_ = 0;
    static constexpr uint64_t NEVER = ~0ULL;
    uint64_t timer_due_[2] = {NEVER, NEVER}; // CPU-cycle deadlines

    std::vector<uint8_t> adpcm_ram_ = std::vector<uint8_t>(ADPCM_RAM_SIZE, 0);
    uint8_t keyboard_latch_ = 0xFF;
    uint8_t gpio_direction_ = 0;
    uint8_t gpio_latch_ = 0;
    uint8_t gpio_input_ = 0x0F;

    uint8_t address_ = 0;
    uint8_t control2_ = 0;
    uint16_t adc_prescale_ = 225;
    bool adc_enabled_ = false;
    uint64_t adc_last_cycle_ = 0;
    uint64_t adc_clock_fraction_ = 0;
    uint8_t adc_data_ = 0;
    std::vector<float> adc_input_;
    size_t adc_input_read_ = 0;
    uint32_t adc_input_rate_ = 0;
    uint64_t adc_source_fraction_ = 0;

    Biquad lp1_, lp2_;
    uint32_t out_rate_ = 44100;
    double src_pos_ = 0.0;
    float prev_sample_ = 0.0f;
    std::vector<float> ring_;
    size_t ring_read_ = 0;
    float mix_gain_ = 1.0f;
};

} // namespace mz
