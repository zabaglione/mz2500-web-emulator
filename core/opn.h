// YM2203 (OPN) wrapper around ymfm, synchronized to the CPU cycle clock.
//
// The chip runs at 2 MHz (CPU/3). ymfm generates at clock/4 (MAX fidelity,
// 500 kHz - the cleanest internal SSG path); samples are produced lazily
// ("flush") whenever the CPU touches a register and at every frame end.
// Decimation to the host rate goes through a 4th-order Butterworth low-pass
// (two biquads) before linear interpolation: without it the SSG square-wave
// harmonics above the output Nyquist fold back as an audible fizz around
// 8-12 kHz (measured +18 dB over EmuZ on the Vector Raid title tune).
//
// The BUSY flag matters: the MZSD driver spin-waits on status bit 7 before
// every address/data write, so ymfm's busy window is mapped onto CPU cycles
// (1 chip clock = 3 CPU cycles).
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "ymfm/ymfm_opn.h"

namespace mz {

class OpnYm2203 : public ymfm::ymfm_interface {
public:
    OpnYm2203();

    void reset();
    void set_output_rate(uint32_t rate);
    uint32_t output_rate() const { return out_rate_; }

    uint8_t read_status(uint64_t now);
    uint8_t read_data();
    void write_address(uint8_t value, uint64_t now);
    void write_data(uint8_t value, uint64_t now);

    // generate chip samples up to the given CPU time and feed the resampler
    void flush_to(uint64_t now);

    // drain up to max_samples mono float samples; returns the count
    size_t read_audio(float* out, size_t max_samples);

    // register-write trace (P5 golden tests): "cycle,reg,value" CSV lines
    void set_trace(FILE* f) { trace_ = f; }

    // debug mixing controls (CLI layer-isolation tests)
    void set_layer_gains(float fm, float ssg) { fm_mix_ = fm; ssg_mix_ = ssg; }

    // Analog output-stage voicing: the real YM2203 synthesizes FM at
    // clock/72 (27.8 kHz), so partials above 13.9 kHz fold down as a gritty
    // fizz - authentic silicon behaviour, but the real MZ-2500's analog
    // path and speaker roll it off. This low-pass on the FM layer models
    // that; 0 disables it. Calibrated against EmuZ's band profile.
    void set_fm_lowpass_hz(uint32_t hz) { fm_lpf_hz_ = hz; design_filters(); }

    // ymfm_interface
    void ymfm_set_busy_end(uint32_t clocks) override {
        busy_end_ = now_ + (uint64_t)clocks * 3;
    }
    bool ymfm_is_busy() override { return now_ < busy_end_; }

private:
    // RBJ-cookbook low-pass biquad (direct form 1)
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

    ymfm::ym2203 chip_;
    uint32_t chip_rate_ = 1;
    uint64_t cycles_per_sample_num_ = 1; // CPU_HZ
    uint64_t generated_ = 0;             // chip samples generated so far
    uint64_t now_ = 0;
    uint64_t busy_end_ = 0;
    uint8_t address_ = 0;

    float fm_mix_ = 1.0f;
    float ssg_mix_ = 1.0f;
    uint32_t fm_lpf_hz_ = 5000;
    Biquad fm_lp1_, fm_lp2_;

    // anti-alias filter + resampler state
    Biquad lp1_, lp2_;
    uint32_t out_rate_ = 44100;
    double src_pos_ = 0.0;
    float prev_sample_ = 0.0f;
    std::vector<float> ring_;
    size_t ring_read_ = 0;

    FILE* trace_ = nullptr;

    void design_filters();
    void push_chip_sample(float mono);
};

} // namespace mz
