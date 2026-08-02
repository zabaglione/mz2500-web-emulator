#include "core/opn.h"

#include "core/timing.h"

namespace mz {

namespace {
// FM is roughly 14-bit, the three SSG channels are up to 32767 each; these
// gains put both on a comparable footing (validated against EmuZ WAV levels
// in the P5 comparison).
constexpr float FM_GAIN = 1.0f / 32768.0f;
constexpr float SSG_GAIN = 1.0f / (3.0f * 49152.0f);
} // namespace

OpnYm2203::OpnYm2203() : chip_(*this) {
    chip_.set_fidelity(ymfm::OPN_FIDELITY_MED); // clock/12 ~= 166.7 kHz
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
}

void OpnYm2203::set_output_rate(uint32_t rate) {
    if (rate > 0) out_rate_ = rate;
}

uint8_t OpnYm2203::read_status(uint64_t now) {
    now_ = now;
    return chip_.read_status();
}

uint8_t OpnYm2203::read_data() { return chip_.read_data(); }

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
        const float fm = (float)out.data[0] * FM_GAIN;
        const float ssg = (float)(out.data[1] + out.data[2] + out.data[3]) * SSG_GAIN;
        push_chip_sample(fm + ssg);
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
