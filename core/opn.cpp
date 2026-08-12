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
    now_ = 0;
    timer_due_[0] = timer_due_[1] = NEVER;
    chip_.reset();
    chip_rate_ = chip_.sample_rate(OPN_CLOCK_HZ);
    generated_ = 0;
    busy_end_ = 0;
    address_ = 0;
    beeper_ = 0.0f;
    port_latch_[0] = port_latch_[1] = 0;
    io_direction_ = 0;
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

void OpnYm2203::set_ssg_io_handoff(uint8_t mixer, uint8_t port_a) {
    chip_.write_address(0x07);
    chip_.write_data(mixer);
    io_direction_ = mixer;
    chip_.write_address(0x0E);
    chip_.write_data(port_a);
    port_latch_[0] = port_a;
    chip_.write_address(0x00);
    address_ = 0;
    busy_end_ = 0;
}

uint8_t OpnYm2203::read_status(uint64_t now) {
    flush_to(now);
    return chip_.read_status();
}

void OpnYm2203::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    if (tnum > 1) return;
    timer_due_[tnum] = duration_in_clocks < 0
        ? NEVER
        : now_ + (uint64_t)duration_in_clocks * 3;
}

uint8_t OpnYm2203::ymfm_external_read(ymfm::access_class type,
                                      uint32_t address) {
    if (type != ymfm::ACCESS_IO || address > 1) return 0x00;
    return port_input_[address];
}

void OpnYm2203::ymfm_external_write(ymfm::access_class type,
                                    uint32_t address, uint8_t data) {
    if (type == ymfm::ACCESS_IO && address < 2) port_latch_[address] = data;
}

uint8_t OpnYm2203::read_data() {
    return chip_.read_data();
}

void OpnYm2203::write_address(uint8_t value, uint64_t now) {
    flush_to(now);
    address_ = value;
    chip_.write_address(value);
}

void OpnYm2203::write_data(uint8_t value, uint64_t now) {
    flush_to(now);
    if (trace_) {
        std::fprintf(trace_, "%llu,%02x,%02x\n", (unsigned long long)now, address_, value);
    }
    chip_.write_data(value);
    if (address_ == 0x07) io_direction_ = value;
    else if (address_ == 0x0E) port_latch_[0] = value;
    else if (address_ == 0x0F) port_latch_[1] = value;
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

void OpnYm2203::generate_to(uint64_t now) {
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

void OpnYm2203::flush_to(uint64_t now) {
    // ymfm delegates Timer A/B scheduling to its host. Generate audio up to
    // each exact deadline before notifying the engine, so timer status and
    // CSM key-ons become visible at the same CPU time instead of being
    // stretched to the next status poll or register write.
    for (;;) {
        const uint64_t next = std::min(timer_due_[0], timer_due_[1]);
        if (next == NEVER || next > now) break;

        generate_to(next);
        now_ = next;
        for (uint32_t t = 0; t < 2; t++) {
            if (timer_due_[t] != NEVER && timer_due_[t] <= next) {
                timer_due_[t] = NEVER;
                m_engine->engine_timer_expired(t);
            }
        }
    }

    generate_to(now);
    now_ = now;
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
