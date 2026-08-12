#include "core/cmt.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "core/timing.h"

namespace mz {
namespace {

constexpr uint32_t LOGICAL_TAPE_RATE = 44100;
constexpr int16_t LOGICAL_TAPE_LEVEL = 24576;
constexpr size_t MAX_LOGICAL_TAPE_SAMPLES = 100u * 1024u * 1024u;

uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void append_le16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void append_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

int32_t decode_pcm(const uint8_t* p, int bits) {
    if (bits == 8) return (static_cast<int32_t>(p[0]) - 128) << 8;
    if (bits == 16) return static_cast<int16_t>(read_le16(p));
    if (bits == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000);
        return v >> 8;
    }
    return static_cast<int32_t>(read_le32(p)) >> 16;
}

struct LogicalTapeRecord {
    const uint8_t* header;
    const uint8_t* data;
    size_t data_size;
};

class LogicalTapeEncoder {
public:
    explicit LogicalTapeEncoder(bool mz80b_mode)
        : long_low_(mz80b_mode ? 14 : 21),
          long_high_(mz80b_mode ? 15 : 22),
          short_low_(mz80b_mode ? 7 : 11),
          short_high_(mz80b_mode ? 8 : 12) {}

    bool encode(const std::vector<LogicalTapeRecord>& records,
                std::vector<int16_t>& out) {
        samples_ = &out;
        for (const auto& record : records) {
            if (!gap(15000) || !tapemark(40) ||
                !block(record.header, 128) || !gap(256) ||
                !block(record.header, 128) || !gap(8000) ||
                !tapemark(20) || !block(record.data, record.data_size) ||
                !gap(256) || !block(record.data, record.data_size))
                return false;
        }
        return !out.empty();
    }

private:
    bool level(int16_t value, size_t count) {
        if (count > MAX_LOGICAL_TAPE_SAMPLES - samples_->size()) return false;
        samples_->insert(samples_->end(), count, value);
        return true;
    }

    bool pulse(bool long_pulse) {
        const size_t low = long_pulse ? long_low_ : short_low_;
        const size_t high = long_pulse ? long_high_ : short_high_;
        return level(-LOGICAL_TAPE_LEVEL, low) &&
               level(LOGICAL_TAPE_LEVEL, high);
    }

    bool gap(size_t count) {
        for (size_t i = 0; i < count; ++i)
            if (!pulse(false)) return false;
        return true;
    }

    bool tapemark(size_t count) {
        for (size_t i = 0; i < count; ++i)
            if (!pulse(true)) return false;
        for (size_t i = 0; i < count; ++i)
            if (!pulse(false)) return false;
        return pulse(true) && pulse(true);
    }

    bool byte(uint8_t value) {
        for (int bit = 7; bit >= 0; --bit)
            if (!pulse((value & (1u << bit)) != 0)) return false;
        return pulse(true);
    }

    bool checksum(uint16_t value) {
        for (int bit = 15; bit >= 0; --bit) {
            if (!pulse((value & (1u << bit)) != 0)) return false;
            if (bit == 8 || bit == 0)
                if (!pulse(true)) return false;
        }
        return pulse(true);
    }

    bool block(const uint8_t* data, size_t size) {
        uint16_t sum = 0;
        for (size_t i = 0; i < size; ++i) {
            const uint8_t value = data[i];
            for (int bit = 0; bit < 8; ++bit)
                sum = static_cast<uint16_t>(sum + ((value >> bit) & 1));
            if (!byte(value)) return false;
        }
        return checksum(sum);
    }

    std::vector<int16_t>* samples_ = nullptr;
    size_t long_low_;
    size_t long_high_;
    size_t short_low_;
    size_t short_high_;
};

} // namespace

void CmtDeck::reset(uint64_t now) {
    transport_ = Transport::Stop;
    last_cycle_ = now;
    sample_fraction_ = 0;
    port_a_enabled_ = false;
    port_c_enabled_ = false;
    port_a_ = 0xFF;
    port_c_ = 0xFF;
    port_a_armed_ = 0;
    open_armed_ = false;
}

void CmtDeck::set_media(std::vector<int16_t> samples, uint32_t sample_rate) {
    samples_ = std::move(samples);
    inserted_ = true;
    sample_rate_ = sample_rate;
    position_ = 0;
    transport_ = Transport::Stop;
    sample_fraction_ = 0;
    write_protected_ = false;
    dirty_ = false;
}

void CmtDeck::mount_pcm_for_test(std::vector<int16_t> samples,
                                uint32_t sample_rate) {
    set_media(std::move(samples), sample_rate);
}

bool CmtDeck::load_wav(const uint8_t* data, size_t size) {
    if (!data || size < 44 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0)
        return false;

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;
    for (size_t off = 12; off + 8 <= size;) {
        const uint32_t chunk_size = read_le32(data + off + 4);
        const size_t body = off + 8;
        if (body > size || chunk_size > size - body) return false;
        if (std::memcmp(data + off, "fmt ", 4) == 0 && chunk_size >= 16) {
            format = read_le16(data + body);
            channels = read_le16(data + body + 2);
            rate = read_le32(data + body + 4);
            bits = read_le16(data + body + 14);
        } else if (std::memcmp(data + off, "data", 4) == 0) {
            pcm = data + body;
            pcm_size = chunk_size;
        }
        const size_t padded = static_cast<size_t>(chunk_size) + (chunk_size & 1);
        if (padded > size - body) break;
        off = body + padded;
    }

    if (format != 1 || channels == 0 || channels > 8 || rate == 0 ||
        rate > 384000 || (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        !pcm)
        return false;
    const size_t bytes_per_sample = bits / 8;
    const size_t frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0 || pcm_size < frame_bytes) return false;
    const size_t frame_count = pcm_size / frame_bytes;
    std::vector<int16_t> mono(frame_count);
    for (size_t frame = 0; frame < frame_count; frame++) {
        int64_t sum = 0;
        const uint8_t* p = pcm + frame * frame_bytes;
        for (uint16_t ch = 0; ch < channels; ch++)
            sum += decode_pcm(p + ch * bytes_per_sample, bits);
        const int64_t mixed = sum / channels;
        mono[frame] = static_cast<int16_t>(std::clamp<int64_t>(mixed, -32768, 32767));
    }
    set_media(std::move(mono), rate);
    return true;
}

bool CmtDeck::load_mzf(const uint8_t* data, size_t size, bool mz80b_mode) {
    if (!data || size < 129) return false;

    std::vector<LogicalTapeRecord> records;
    for (size_t offset = 0; offset < size;) {
        if (size - offset < 128) return false;
        const size_t data_size = read_le16(data + offset + 0x12);
        if (data_size == 0 || data_size > size - offset - 128) return false;
        records.push_back({data + offset, data + offset + 128, data_size});
        offset += 128 + data_size;
    }

    std::vector<int16_t> samples;
    const size_t reserve_hint = size >
            (MAX_LOGICAL_TAPE_SAMPLES - 1000000) / 800
        ? MAX_LOGICAL_TAPE_SAMPLES
        : static_cast<size_t>(1000000) + size * static_cast<size_t>(800);
    try {
        samples.reserve(reserve_hint);
        LogicalTapeEncoder encoder(mz80b_mode);
        if (!encoder.encode(records, samples)) return false;
    } catch (...) {
        return false;
    }
    mz80b_mode_ = mz80b_mode;
    set_media(std::move(samples), LOGICAL_TAPE_RATE);
    return true;
}

bool CmtDeck::create_blank(uint32_t seconds, uint32_t sample_rate) {
    if (seconds == 0 || seconds > 24 * 60 * 60 || sample_rate < 1000 ||
        sample_rate > 384000)
        return false;
    const uint64_t count = static_cast<uint64_t>(seconds) * sample_rate;
    if (count > std::numeric_limits<size_t>::max()) return false;
    try {
        set_media(std::vector<int16_t>(static_cast<size_t>(count), 0), sample_rate);
    } catch (...) {
        return false;
    }
    return true;
}

void CmtDeck::eject() {
    // Keep the image available for the host to persist after a CPU-driven
    // OPEN pulse. Loading or creating another tape replaces it.
    inserted_ = false;
    transport_ = Transport::Stop;
    sample_fraction_ = 0;
}

void CmtDeck::command(Transport next) {
    if (next == Transport::Stop) {
        transport_ = Transport::Stop;
        sample_fraction_ = 0;
        return;
    }
    if (!inserted_ || samples_.empty()) {
        transport_ = Transport::Stop;
        return;
    }
    if (next == Transport::Play && port_c_enabled_ &&
        !(port_c_ & 0x40) && write_protected_) {
        transport_ = Transport::Stop;
        return;
    }
    if (next == Transport::Play && position_ >= samples_.size()) position_ = samples_.size();
    transport_ = next;
    sample_fraction_ = 0;
}

void CmtDeck::set_port_a(bool enabled, uint8_t value, uint64_t now) {
    sync(now);
    if (!enabled) {
        port_a_enabled_ = false;
        port_a_armed_ = 0;
        return;
    }
    if (!port_a_enabled_) {
        port_a_enabled_ = true;
        port_a_ = value;
        port_a_armed_ = 0;
        return;
    }

    if (mz80b_mode_) {
        // The older deck interface does not use the MZ-2000/2500's four
        // active-low pulse inputs. Bit 0 is a shared FF/REW trigger whose
        // rising edge samples bit 1; PLAY and STOP are rising edges on bits
        // 2 and 3. STOP is physically last in the decode and wins if a
        // malformed write raises more than one command at once.
        const uint8_t rising = static_cast<uint8_t>(~port_a_ & value);
        port_a_ = value;
        if (rising & 0x08) command(Transport::Stop);
        else if (rising & 0x04) command(Transport::Play);
        else if (rising & 0x01)
            command((value & 0x02) ? Transport::FastForward : Transport::Rewind);
        return;
    }

    const uint8_t command_bits = 0x0F;
    const uint8_t falling = static_cast<uint8_t>(port_a_ & ~value & command_bits);
    const uint8_t rising = static_cast<uint8_t>(~port_a_ & value & command_bits);
    port_a_armed_ |= falling;
    const uint8_t completed = static_cast<uint8_t>(rising & port_a_armed_);
    port_a_armed_ &= static_cast<uint8_t>(~rising);
    port_a_ = value;

    // STOP wins if malformed software completes several pulses together;
    // otherwise follow the physical button order in the I/O map.
    if (completed & 0x08) command(Transport::Stop);
    else if (completed & 0x04) command(Transport::Play);
    else if (completed & 0x02) command(Transport::FastForward);
    else if (completed & 0x01) command(Transport::Rewind);
}

void CmtDeck::set_port_c(bool enabled, uint8_t value, uint64_t now) {
    sync(now); // finish old WRITE level and REC2 mode before changing pins
    if (!enabled) {
        port_c_enabled_ = false;
        open_armed_ = false;
        return;
    }
    if (!port_c_enabled_) {
        port_c_enabled_ = true;
        port_c_ = value;
        open_armed_ = false;
        return;
    }

    if ((port_c_ & 0x10) && !(value & 0x10)) open_armed_ = true;
    const bool opened = !(port_c_ & 0x10) && (value & 0x10) && open_armed_;
    port_c_ = value;
    if (opened) {
        open_armed_ = false;
        eject();
    }
}

void CmtDeck::manual_command(Transport next, uint64_t now) {
    sync(now);
    if (port_c_enabled_ && (port_c_ & 0x20)) return; // KINH
    command(next);
}

void CmtDeck::finish_play_end() {
    position_ = samples_.size();
    sample_fraction_ = 0;
    transport_ = (!mz80b_mode_ && port_a_enabled_ && !(port_a_ & 0x20))
        ? Transport::Rewind : Transport::Stop;
}

void CmtDeck::finish_rewind_start() {
    position_ = 0;
    sample_fraction_ = 0;
    transport_ = (!mz80b_mode_ && port_a_enabled_ && !(port_a_ & 0x40))
        ? Transport::Play : Transport::Stop;
}

void CmtDeck::sync(uint64_t now) {
    if (now < last_cycle_) {
        last_cycle_ = now;
        sample_fraction_ = 0;
        return;
    }
    const uint64_t elapsed = now - last_cycle_;
    last_cycle_ = now;
    if (elapsed == 0 || transport_ == Transport::Stop || !inserted_ || samples_.empty()) return;

    const uint64_t speed = transport_ == Transport::Play ? 1 : FAST_SPEED;
    const unsigned __int128 scaled = static_cast<unsigned __int128>(elapsed) *
                                     sample_rate_ * speed + sample_fraction_;
    const uint64_t advance = static_cast<uint64_t>(scaled / CPU_HZ);
    sample_fraction_ = static_cast<uint64_t>(scaled % CPU_HZ);
    if (advance == 0) return;

    if (transport_ == Transport::Rewind) {
        if (advance >= position_) finish_rewind_start();
        else position_ -= advance;
        return;
    }

    const uint64_t remaining = samples_.size() - std::min<uint64_t>(position_, samples_.size());
    const uint64_t amount = std::min<uint64_t>(advance, remaining);
    if (transport_ == Transport::Play && port_c_enabled_ &&
        !(port_c_ & 0x40) && !write_protected_) {
        const int16_t level = (port_c_ & 0x80) ? RECORD_LEVEL : -RECORD_LEVEL;
        std::fill(samples_.begin() + static_cast<size_t>(position_),
                  samples_.begin() + static_cast<size_t>(position_ + amount), level);
        if (amount != 0) dirty_ = true;
    }
    position_ += amount;
    if (advance >= remaining) finish_play_end();
}

bool CmtDeck::read_data(uint64_t now) {
    sync(now);
    if (transport_ != Transport::Play || position_ >= samples_.size()) return false;
    return samples_[static_cast<size_t>(position_)] > 0;
}

bool CmtDeck::running(uint64_t now) {
    sync(now);
    return transport_ != Transport::Stop;
}

uint64_t CmtDeck::position_samples(uint64_t now) {
    sync(now);
    return position_;
}

uint64_t CmtDeck::duration_ms() const {
    return sample_rate_ ? static_cast<uint64_t>(samples_.size()) * 1000 / sample_rate_ : 0;
}

uint64_t CmtDeck::position_ms(uint64_t now) {
    sync(now);
    return sample_rate_ ? position_ * 1000 / sample_rate_ : 0;
}

std::vector<uint8_t> CmtDeck::wav_image() const {
    if (samples_.empty() || samples_.size() > (std::numeric_limits<uint32_t>::max() - 36) / 2)
        return {};
    const uint32_t data_size = static_cast<uint32_t>(samples_.size() * 2);
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(data_size) + 44);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    append_le32(out, 36 + data_size);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append_le32(out, 16);
    append_le16(out, 1); // PCM
    append_le16(out, 1); // mono
    append_le32(out, sample_rate_);
    append_le32(out, sample_rate_ * 2);
    append_le16(out, 2);
    append_le16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    append_le32(out, data_size);
    for (int16_t sample : samples_) append_le16(out, static_cast<uint16_t>(sample));
    return out;
}

} // namespace mz
