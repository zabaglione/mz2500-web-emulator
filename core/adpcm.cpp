#include "core/adpcm.h"

#include <algorithm>
#include <cmath>

#include "core/timing.h"

namespace mz {

namespace {
// The Y8950 mixes FM and delta-T into one roughly 16-bit stream. The level
// RELATIVE TO THE OPN is an uncalibrated guess: the OPN's gains were tuned
// against EmuZ WAV captures, but no such oracle exists for this board
// (EmuZ's Y8950 stays quiet). Revisit against real hardware or a known
// recording. Note the machine mix simply adds the two chips, so both
// peaking together can reach +-2.0 before the host-side clamp.
constexpr float OUT_GAIN = 1.0f / 32768.0f;
} // namespace

void AdpcmY8950::Biquad::design_lowpass(double fs, double fc, double q) {
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

uint64_t AdpcmY8950::clocks_to_cycles(uint64_t clocks) {
    // ceil(clocks * CPU_HZ / chip clock): never round a wait down to zero
    return (clocks * CPU_HZ + ADPCM_CLOCK_HZ - 1) / ADPCM_CLOCK_HZ;
}

void AdpcmY8950::design_filters() {
    const double fc = std::min(0.45 * out_rate_, 20000.0);
    lp1_.design_lowpass(chip_rate_, fc, 0.5412);
    lp2_.design_lowpass(chip_rate_, fc, 1.3066);
}

AdpcmY8950::AdpcmY8950() : chip_(*this) { reset(); }

void AdpcmY8950::reset() {
    chip_.reset();
    chip_rate_ = chip_.sample_rate(ADPCM_CLOCK_HZ);
    generated_ = 0;
    now_ = 0;
    busy_end_ = 0;
    timer_due_[0] = timer_due_[1] = NEVER;
    io_latch_[0] = io_latch_[1] = 0xFF;
    src_pos_ = 0.0;
    prev_sample_ = 0.0f;
    ring_.clear();
    ring_read_ = 0;
    design_filters();
}

void AdpcmY8950::set_output_rate(uint32_t rate) {
    if (rate > 0) out_rate_ = rate;
    design_filters();
}

void AdpcmY8950::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    if (tnum > 1) return;
    // ymfm hands a negative duration to cancel the timer
    timer_due_[tnum] = duration_in_clocks < 0
        ? NEVER
        : now_ + clocks_to_cycles((uint64_t)duration_in_clocks);
}

void AdpcmY8950::fire_timers(uint64_t now) {
    // No IRQ line is wired on the MZ-2500 (the I/O controller's sources are
    // VBLANK, 8253, printer and RTC), so expiry only sets the status flags
    // the software polls.
    // Deliberately coarse: at most one expiry per timer per observation,
    // rescheduled from the observation time rather than the true deadline -
    // periods stretch by the polling lag. The flags latch until reset, so
    // polled software cannot tell; do not mistake this for a cycle-accurate
    // timer.
    for (int t = 0; t < 2; t++) {
        if (timer_due_[t] != NEVER && now >= timer_due_[t]) {
            timer_due_[t] = NEVER;
            m_engine->engine_timer_expired(t);
        }
    }
}

uint8_t AdpcmY8950::ymfm_external_read(ymfm::access_class type, uint32_t address) {
    if (type == ymfm::ACCESS_ADPCM_B)
        return address < ADPCM_RAM_SIZE ? adpcm_ram_[address] : 0x00;
    if (type == ymfm::ACCESS_IO)
        // The board's GPIO (register 19h; line/mic switching per the I/O
        // map, bit layout undocumented) and the keyboard port (regs
        // 05h/06h). Nothing is attached, so a read hands back the last
        // latched write - open bus (FFh) before anything has been written.
        return address < 2 ? io_latch_[address] : 0xFF;
    return 0x00;
}

void AdpcmY8950::ymfm_external_write(ymfm::access_class type, uint32_t address,
                                     uint8_t data) {
    if (type == ymfm::ACCESS_ADPCM_B) {
        if (address < ADPCM_RAM_SIZE) adpcm_ram_[address] = data;
        return;
    }
    if (type == ymfm::ACCESS_IO && address < 2) io_latch_[address] = data;
}

uint8_t AdpcmY8950::read_status(uint64_t now) {
    // a polling loop (waiting on BRDY/EOS/PLAYING) must advance the chip,
    // not just the timers, or those bits freeze between register writes
    flush_to(now);
    return chip_.read_status();
}

uint8_t AdpcmY8950::read_data(uint64_t now) {
    flush_to(now); // delta-T readback has side effects; keep time honest
    return chip_.read_data();
}

void AdpcmY8950::write_address(uint8_t value, uint64_t now) {
    flush_to(now); // already sets now_ and fires timers
    chip_.write_address(value);
}

void AdpcmY8950::write_data(uint8_t value, uint64_t now) {
    flush_to(now); // already sets now_ and fires timers
    chip_.write_data(value);
}

void AdpcmY8950::push_chip_sample(float mono) {
    const double step = (double)chip_rate_ / (double)out_rate_;
    src_pos_ += 1.0;
    while (src_pos_ >= step) {
        src_pos_ -= step;
        const double frac = 1.0 - src_pos_ / 1.0;
        ring_.push_back(prev_sample_ + (mono - prev_sample_) * (float)frac);
    }
    prev_sample_ = mono;
}

void AdpcmY8950::flush_to(uint64_t now) {
    now_ = now;
    fire_timers(now);
    const uint64_t target = now * chip_rate_ / CPU_HZ;
    ymfm::y8950::output_data out;
    while (generated_ < target) {
        chip_.generate(&out);
        push_chip_sample(lp2_.run(lp1_.run((float)out.data[0] * OUT_GAIN)));
        generated_++;
    }
}

size_t AdpcmY8950::read_audio(float* out, size_t max_samples) {
    size_t n = 0;
    while (n < max_samples && ring_read_ < ring_.size()) out[n++] = ring_[ring_read_++];
    if (ring_read_ >= ring_.size()) {
        ring_.clear();
        ring_read_ = 0;
    }
    return n;
}

} // namespace mz
