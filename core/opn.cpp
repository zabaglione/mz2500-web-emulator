#include "core/opn.h"

#include <algorithm>
#include <cmath>

#include "core/timing.h"

namespace mz {

namespace {
// FM is roughly 14-bit, the three SSG channels are up to 32767 each; these
// gains put both on a comparable footing (validated against EmuZ WAV levels
// in the P5 comparison).
constexpr float FM_GAIN = 1.0f / 32768.0f;
constexpr float SSG_GAIN = 1.0f / (3.0f * 49152.0f);
} // namespace

void OpnYm2203::Biquad::design_lowpass(double fs, double fc, double q) {
    const double w0 = 2.0 * M_PI * fc / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    b0 = ((1.0 - cw) / 2.0) / a0;
    b1 = (1.0 - cw) / a0;
    b2 = b0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha) / a0;
    clear();
}

void OpnYm2203::design_filters() {
    // 4th-order Butterworth (Q pair 0.5412 / 1.3066) just under the output
    // Nyquist; the real chip's analog path has no folded images, so neither
    // should we
    const double fc = std::min(0.45 * out_rate_, 20000.0);
    lp1_.design_lowpass(chip_rate_, fc, 0.5412);
    lp2_.design_lowpass(chip_rate_, fc, 1.3066);
    const double fm_fc = fm_lpf_hz_ > 0 ? (double)fm_lpf_hz_ : fc;
    fm_lp1_.design_lowpass(chip_rate_, fm_fc, 0.5412);
    fm_lp2_.design_lowpass(chip_rate_, fm_fc, 1.3066);
}

OpnYm2203::OpnYm2203() : chip_(*this) {
    chip_.set_fidelity(ymfm::OPN_FIDELITY_MAX); // clock/4 = 500 kHz
    reset();
}

void OpnYm2203::reset() {
    chip_.reset();
    chip_rate_ = chip_.sample_rate(OPN_CLOCK_HZ);
    generated_ = 0;
    now_ = 0;
    busy_end_ = 0;
    address_ = 0;
    src_pos_ = 0.0;
    prev_sample_ = 0.0f;
    ring_.clear();
    ring_read_ = 0;
    design_filters();
}

void OpnYm2203::set_output_rate(uint32_t rate) {
    if (rate > 0) out_rate_ = rate;
    design_filters();
}

uint8_t OpnYm2203::read_status(uint64_t now) {
    now_ = now;
    return chip_.read_status();
}

uint8_t OpnYm2203::read_data() {
    // register 0Fh = SSG port B: the sense lines live outside the chip
    if (address_ == 0x0F) return port_b_input_;
    return chip_.read_data();
}

void OpnYm2203::write_address(uint8_t value, uint64_t now) {
    flush_to(now);
    now_ = now;
    address_ = value;
    chip_.write_address(value);
}

void OpnYm2203::write_data(uint8_t value, uint64_t now) {
    flush_to(now);
    now_ = now;
    if (trace_) {
        std::fprintf(trace_, "%llu,%02x,%02x\n", (unsigned long long)now, address_, value);
    }
    chip_.write_data(value);
}

void OpnYm2203::push_chip_sample(float mono) {
    // linear-interpolation resample from chip_rate_ to out_rate_
    const double step = (double)chip_rate_ / (double)out_rate_;
    src_pos_ += 1.0;
    while (src_pos_ >= step) {
        src_pos_ -= step;
        const double frac = 1.0 - src_pos_ / 1.0; // position between prev and cur
        ring_.push_back(prev_sample_ + (mono - prev_sample_) * (float)frac);
    }
    prev_sample_ = mono;
}

void OpnYm2203::flush_to(uint64_t now) {
    // chip sample period in CPU cycles: CPU_HZ / chip_rate_ (36 at MED)
    const uint64_t target = now * chip_rate_ / CPU_HZ;
    ymfm::ym2203::output_data out;
    while (generated_ < target) {
        chip_.generate(&out);
        const float fm = fm_lp2_.run(fm_lp1_.run((float)out.data[0] * FM_GAIN));
        const float ssg = (float)(out.data[1] + out.data[2] + out.data[3]) * SSG_GAIN;
        push_chip_sample(lp2_.run(lp1_.run(fm * fm_mix_ + ssg * ssg_mix_ + beeper_)));
        generated_++;
    }
}

size_t OpnYm2203::read_audio(float* out, size_t max_samples) {
    size_t n = 0;
    while (n < max_samples && ring_read_ < ring_.size()) out[n++] = ring_[ring_read_++];
    if (ring_read_ >= ring_.size()) {
        ring_.clear();
        ring_read_ = 0;
    }
    return n;
}

} // namespace mz
