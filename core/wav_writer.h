// Minimal 16-bit PCM WAV writer used by the headless CLI (--audio-wav) and
// the P0 selftest.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mz {

inline bool write_wav16(const std::string& path, const std::vector<int16_t>& samples,
                        uint32_t sample_rate, uint16_t channels) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t byte_rate = sample_rate * channels * 2;
    const uint16_t block_align = channels * 2;

    auto put16 = [&](uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); };
    auto put32 = [&](uint32_t v) {
        std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f);
        std::fputc((v >> 16) & 0xFF, f); std::fputc((v >> 24) & 0xFF, f);
    };

    std::fwrite("RIFF", 1, 4, f);
    put32(36 + data_bytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    put32(16);
    put16(1); // PCM
    put16(channels);
    put32(sample_rate);
    put32(byte_rate);
    put16(block_align);
    put16(16); // bits per sample
    std::fwrite("data", 1, 4, f);
    put32(data_bytes);
    std::fwrite(samples.data(), 1, data_bytes, f);
    std::fclose(f);
    return true;
}

} // namespace mz
