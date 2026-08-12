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
    keyboard_latch_ = 0xFF;
    gpio_direction_ = 0;
    gpio_latch_ = 0;
    gpio_input_ = 0x0F;
    address_ = 0;
    control2_ = 0;
    adc_prescale_ = 225;
    adc_enabled_ = false;
    adc_last_cycle_ = 0;
    adc_clock_fraction_ = 0;
    adc_data_ = 0;
    clear_adc_samples();
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

uint8_t AdpcmY8950::ymfm_external_read(ymfm::access_class type, uint32_t address) {
    if (type == ymfm::ACCESS_ADPCM_B)
        return address < adpcm_ram_.size() ? adpcm_ram_[address] : 0x00;
    if (type == ymfm::ACCESS_IO) {
        if (address == 0) {
            const uint8_t pins = static_cast<uint8_t>(
                (gpio_latch_ & gpio_direction_) | (gpio_input_ & ~gpio_direction_));
            return static_cast<uint8_t>(0xF0 | (pins & 0x0F));
        }
        if (address == 1) return 0xFF; // no keyboard matrix on MZ-1E35
    }
    return 0x00;
}

void AdpcmY8950::ymfm_external_write(ymfm::access_class type, uint32_t address,
                                     uint8_t data) {
    if (type == ymfm::ACCESS_ADPCM_B) {
        if (address < adpcm_ram_.size()) adpcm_ram_[address] = data;
        return;
    }
    if (type == ymfm::ACCESS_IO && address == 0) gpio_latch_ = data & 0x0F;
    if (type == ymfm::ACCESS_IO && address == 1) keyboard_latch_ = data;
}

uint8_t AdpcmY8950::read_status(uint64_t now) {
    // a polling loop (waiting on BRDY/EOS/PLAYING) must advance the chip,
    // not just the timers, or those bits freeze between register writes
    flush_to(now);
    return chip_.read_status();
}

uint8_t AdpcmY8950::read_data(uint64_t now) {
    flush_to(now); // delta-T readback has side effects; keep time honest
    if (address_ == 0x1A) return adc_data_;
    return chip_.read_data();
}

void AdpcmY8950::write_address(uint8_t value, uint64_t now) {
    flush_to(now); // already sets now_ and fires timers
    address_ = value;
    chip_.write_address(value);
}

void AdpcmY8950::write_data(uint8_t value, uint64_t now) {
    flush_to(now); // already sets now_ and fires timers
    if (address_ == 0x08) {
        control2_ = value;
        const bool next = (value & 0x0C) == 0x08; // SAMPLE=1, DA/AD=0
        if (next && !adc_enabled_) {
            adc_last_cycle_ = now;
            adc_clock_fraction_ = 0;
            adc_source_fraction_ = 0;
        }
        adc_enabled_ = next;
    } else if (address_ == 0x0D) {
        adc_prescale_ = static_cast<uint16_t>((adc_prescale_ & 0x0700) | value);
        adc_clock_fraction_ = 0;
    } else if (address_ == 0x0E) {
        adc_prescale_ = static_cast<uint16_t>((adc_prescale_ & 0x00FF) |
                                              ((value & 0x07) << 8));
        adc_clock_fraction_ = 0;
    } else if (address_ == 0x18) {
        gpio_direction_ = value & 0x0F;
    }
    chip_.write_data(value);
}

bool AdpcmY8950::set_adpcm_ram_size(uint32_t size) {
    if (size < MIN_ADPCM_RAM_SIZE || size > MAX_ADPCM_RAM_SIZE ||
        (size & (size - 1)) != 0)
        return false;
    adpcm_ram_.resize(size, 0);
    return true;
}

size_t AdpcmY8950::queue_adc_samples(const float* samples, size_t count,
                                     uint32_t rate) {
    if (!samples || count == 0 || rate < 1000 || rate > 384000) return 0;
    if (adc_input_read_ != 0) {
        adc_input_.erase(adc_input_.begin(), adc_input_.begin() + adc_input_read_);
        adc_input_read_ = 0;
    }
    if (adc_input_rate_ != 0 && adc_input_rate_ != rate) clear_adc_samples();
    adc_input_rate_ = rate;
    const size_t limit = static_cast<size_t>(rate) * 2;
    if (count > limit) {
        samples += count - limit;
        count = limit;
    }
    const size_t available = adc_input_.size() - adc_input_read_;
    if (available + count > limit) {
        const size_t discard = available + count - limit;
        adc_input_read_ = std::min(adc_input_read_ + discard, adc_input_.size());
    }
    if (adc_input_read_ != 0) {
        adc_input_.erase(adc_input_.begin(), adc_input_.begin() + adc_input_read_);
        adc_input_read_ = 0;
    }
    try {
        adc_input_.insert(adc_input_.end(), samples, samples + count);
    } catch (...) {
        return 0;
    }
    return count;
}

void AdpcmY8950::clear_adc_samples() {
    adc_input_.clear();
    adc_input_read_ = 0;
    adc_input_rate_ = 0;
    adc_source_fraction_ = 0;
}

void AdpcmY8950::update_adc_to(uint64_t now) {
    if (!adc_enabled_ || now <= adc_last_cycle_) {
        adc_last_cycle_ = now;
        return;
    }
    const uint32_t prescale = std::clamp<uint32_t>(adc_prescale_, 225, 2047);
    const uint64_t denominator = static_cast<uint64_t>(CPU_HZ) * prescale;
    const unsigned __int128 clocks = static_cast<unsigned __int128>(now - adc_last_cycle_) *
                                     ADPCM_CLOCK_HZ + adc_clock_fraction_;
    const uint64_t conversions = static_cast<uint64_t>(clocks / denominator);
    adc_clock_fraction_ = static_cast<uint64_t>(clocks % denominator);
    adc_last_cycle_ = now;
    if (conversions == 0 || adc_input_rate_ == 0) return;

    const uint32_t conversion_rate = ADPCM_CLOCK_HZ / prescale;
    for (uint64_t i = 0; i < conversions; i++) {
        if (adc_input_read_ < adc_input_.size()) {
            const float sample = std::clamp(adc_input_[adc_input_read_], -1.0f, 1.0f);
            const int value = std::clamp(static_cast<int>(std::lround(sample * 128.0f)),
                                         -128, 127);
            adc_data_ = static_cast<uint8_t>(static_cast<int8_t>(value));
        }
        adc_source_fraction_ += adc_input_rate_;
        while (adc_source_fraction_ >= conversion_rate &&
               adc_input_read_ < adc_input_.size()) {
            adc_source_fraction_ -= conversion_rate;
            adc_input_read_++;
        }
    }
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

void AdpcmY8950::generate_to(uint64_t now) {
    const uint64_t target = now * chip_rate_ / CPU_HZ;
    ymfm::y8950::output_data out;
    while (generated_ < target) {
        chip_.generate(&out);
        push_chip_sample(lp2_.run(lp1_.run((float)out.data[0] * OUT_GAIN)) * mix_gain_);
        generated_++;
    }
}

void AdpcmY8950::flush_to(uint64_t now) {
    update_adc_to(now);
    // Advance through every real timer deadline. Timer expiry immediately
    // schedules the next period through ymfm_set_timer(), so keeping now_ at
    // the deadline preserves phase even when software polls the status late.
    // The MZ-2500 does not wire the board's IRQ output into its interrupt
    // controller; expiry is nevertheless visible through the polled flags.
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
