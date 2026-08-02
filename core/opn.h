// YM2203 (OPN) wrapper around ymfm, synchronized to the CPU cycle clock.
//
// The chip runs at 2 MHz (CPU/3). ymfm generates at clock/12 with the MED
// fidelity setting; samples are produced lazily ("flush") whenever the CPU
// touches a register and at every frame end, then linearly resampled into a
// float ring at the host output rate (AudioContext rate in the browser,
// 44100 Hz in the CLI).
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

    // ymfm_interface
    void ymfm_set_busy_end(uint32_t clocks) override {
        busy_end_ = now_ + (uint64_t)clocks * 3;
    }
    bool ymfm_is_busy() override { return now_ < busy_end_; }

private:
    ymfm::ym2203 chip_;
    uint32_t chip_rate_ = 1;
    uint64_t cycles_per_sample_num_ = 1; // CPU_HZ
    uint64_t generated_ = 0;             // chip samples generated so far
    uint64_t now_ = 0;
    uint64_t busy_end_ = 0;
    uint8_t address_ = 0;

    // resampler state
    uint32_t out_rate_ = 44100;
    double src_pos_ = 0.0;
    float prev_sample_ = 0.0f;
    std::vector<float> ring_;
    size_t ring_read_ = 0;

    FILE* trace_ = nullptr;

    void push_chip_sample(float mono);
};

} // namespace mz
