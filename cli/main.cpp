// Headless CLI for the MZ-2500 web emulator core.
//
// Flag vocabulary intentionally mirrors the EmuZ-2500 macOS runner
// (shared/mz2500/emulators/emuz_macos/main.cpp) so the existing verification
// scripts can drive both emulators with the same pulse sequences.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/keyboard.h"
#include "core/mz2500.h"
#include "core/timing.h"
#include "core/wav_writer.h"

extern "C" {
#include "z80/z80.h"
}
#include "ymfm/ymfm_opn.h"

namespace {

// ---------------------------------------------------------------------------
// P0 selftest: prove the vendored Z80 core (with the 16-bit I/O address
// patch) and ymfm's YM2203 both work before any machine code exists.
// ---------------------------------------------------------------------------

struct SelftestBus {
    uint8_t mem[0x10000] = {};
    uint16_t last_out_port = 0;
    uint8_t last_out_val = 0;
    uint16_t last_in_port = 0;
};

uint8_t st_read(void* ud, uint16_t addr) {
    return static_cast<SelftestBus*>(ud)->mem[addr];
}
void st_write(void* ud, uint16_t addr, uint8_t val) {
    static_cast<SelftestBus*>(ud)->mem[addr] = val;
}
uint8_t st_in(z80* z, uint16_t port) {
    auto* bus = static_cast<SelftestBus*>(z->userdata);
    bus->last_in_port = port;
    return 0x99;
}
void st_out(z80* z, uint16_t port, uint8_t val) {
    auto* bus = static_cast<SelftestBus*>(z->userdata);
    bus->last_out_port = port;
    bus->last_out_val = val;
}

class SilentChipInterface : public ymfm::ymfm_interface {};

int run_selftest() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[selftest] %-40s %s\n", what, ok ? "OK" : "FAIL");
        if (!ok) failures++;
    };

    // --- Z80: arithmetic, memory store, 16-bit I/O address bus ---
    SelftestBus bus;
    const uint8_t program[] = {
        0x3E, 0x12,             // ld a, 12h
        0x06, 0x34,             // ld b, 34h
        0x80,                   // add a, b        -> a = 46h
        0x32, 0x00, 0x80,       // ld (8000h), a
        0x01, 0x34, 0x12,       // ld bc, 1234h
        0xED, 0x79,             // out (c), a      -> port bus must be 1234h
        0xDB, 0x42,             // in a, (42h)     -> port bus must be 4642h
        0x32, 0x01, 0x80,       // ld (8001h), a
        0x76,                   // halt
    };
    std::memcpy(bus.mem, program, sizeof(program));

    z80 cpu;
    z80_init(&cpu);
    cpu.read_byte = st_read;
    cpu.write_byte = st_write;
    cpu.port_in = st_in;
    cpu.port_out = st_out;
    cpu.userdata = &bus;

    for (int i = 0; i < 100 && !cpu.halted; i++) z80_step(&cpu);

    check(cpu.halted, "Z80 reaches HALT");
    check(bus.mem[0x8000] == 0x46, "Z80 add result stored (12h+34h=46h)");
    check(bus.last_out_port == 0x1234 && bus.last_out_val == 0x46,
          "OUT (C),r drives 16-bit port 1234h");
    check(bus.last_in_port == 0x4642, "IN A,(n) drives 16-bit port 4642h");
    check(bus.mem[0x8001] == 0x99, "IN A,(n) value stored");
    check(cpu.cyc > 0, "Z80 cycle counter advances");

    // --- ymfm: YM2203 at 2 MHz produces one second of silence at power-on ---
    SilentChipInterface intf;
    ymfm::ym2203 opn(intf);
    opn.reset();
    // ymfm's internal rate depends on fidelity: clock/24 (MIN) .. clock/4 (MAX)
    const uint32_t rate = opn.sample_rate(mz::OPN_CLOCK_HZ);
    check(rate >= mz::OPN_CLOCK_HZ / 24 && rate <= mz::OPN_CLOCK_HZ / 4,
          "YM2203 sample_rate plausible");

    std::vector<int16_t> wav;
    wav.reserve(rate);
    int32_t peak = 0;
    ymfm::ym2203::output_data out;
    for (uint32_t i = 0; i < rate; i++) {
        opn.generate(&out);
        // FM channel + 3 SSG channels, mixed to mono for the selftest
        int32_t mixed = 0;
        for (uint32_t ch = 0; ch < ymfm::ym2203::OUTPUTS; ch++) mixed += out.data[ch];
        if (mixed > peak) peak = mixed;
        if (-mixed > peak) peak = -mixed;
        int32_t clamped = mixed;
        if (clamped > 32767) clamped = 32767;
        if (clamped < -32768) clamped = -32768;
        wav.push_back(static_cast<int16_t>(clamped));
    }
    std::printf("[selftest] YM2203 rate=%u Hz, 1s peak amplitude=%d\n", rate, peak);
    check(peak < 328, "YM2203 power-on output is silence (<1% FS)");

    const char* wav_path = "selftest_silence.wav";
    check(mz::write_wav16(wav_path, wav, rate, 1), "WAV writer produces file");
    std::printf("[selftest] wrote %s (%zu samples @ %u Hz)\n", wav_path, wav.size(), rate);

    std::printf("[selftest] %s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}

// Program the SSG directly and measure output frequencies - detects any
// scaling mismatch between ymfm's SSG and the MZ-2500 formula
// (f = 2MHz / (32 * TP), the relation the MZSD driver and EmuZ agree on).
int run_ssg_test() {
    for (int tp : {47, 40, 100}) {
        mz::OpnYm2203 opn;
        opn.set_output_rate(48000);
        uint64_t t = 0;
        auto wr = [&](uint8_t reg, uint8_t val) {
            opn.write_address(reg, t);
            t += 100;
            opn.write_data(val, t);
            t += 100;
        };
        wr(0x07, 0x3E);            // mixer: tone A only
        wr(0x08, 0x0F);            // channel A volume max
        wr(0x00, (uint8_t)tp);     // tone period low
        wr(0x01, (uint8_t)(tp >> 8));
        if (tp == 47) {
            // vibrato pattern like the MZSD driver: rewrite the period at
            // every 125 Hz tick, alternating two neighbouring values
            for (int tick = 0; t < 6'000'000; tick++) {
                t += 48'000; // 125 Hz in CPU cycles
                wr(0x00, (uint8_t)(tick & 1 ? 44 : 47));
            }
        }
        opn.flush_to(6'000'000);   // one emulated second
        std::vector<float> buf(65536);
        std::vector<float> all;
        size_t n;
        while ((n = opn.read_audio(buf.data(), buf.size())) > 0)
            all.insert(all.end(), buf.begin(), buf.begin() + n);
        // count rising zero crossings over the last half (settled region)
        int crossings = 0;
        for (size_t i = all.size() / 2 + 1; i < all.size(); i++)
            if (all[i - 1] < 0 && all[i] >= 0) crossings++;
        const double seconds = (all.size() / 2.0) / 48000.0;
        const double measured = crossings / seconds;
        const double expected = 2'000'000.0 / (32.0 * tp);
        std::printf("[ssg] TP=%3d expected %7.1f Hz measured %7.1f Hz (ratio %.4f)\n",
                    tp, expected, measured, measured / expected);
        std::vector<int16_t> wav;
        for (float v : all) wav.push_back((int16_t)(std::max(-1.f, std::min(1.f, v)) * 32767));
        char path[64];
        std::snprintf(path, sizeof(path), "ssg_test_tp%d.wav", tp);
        mz::write_wav16(path, wav, 48000, 1);
    }
    return 0;
}

void usage() {
    std::printf(
        "mz2500w-cli - headless MZ-2500 web emulator core\n"
        "  --selftest            run P0 self test (Z80 + YM2203 + WAV)\n"
        "  --disk-a PATH         mount a D88 image in drive 0 (FD1) and boot it\n"
        "  --disk-b PATH         mount a D88 image in drive 1 (FD2)\n"
        "  --frames N            run N emulated frames (default 600)\n"
        "  --trace-boot          log dummy-IPL and boot progress\n"
        "  --cpu-report          print CPU state at exit\n"
        "  --memory-report ADDR  print memory[ADDR] at exit (hex, repeatable)\n"
        "  --trace-memory ADDR   log every frame where memory[ADDR] changes\n"
        "  --boot-delay N        override IPL boot delay in frames (calibration)\n"
        "  --fdc-latency-us N    override FDC per-read latency (calibration)\n"
        "  --screenshot PATH     write the final frame as a 640x400 PPM (P6)\n"
        "  --key-pulse K:S[:N]   hold key K for N frames (default 4) from frame S\n"
        "  --joy-pulse M:S[:N]   hold joystick mask M (hex) for N frames from S\n"
        "  --memory-poke A:V:F   write hex value V to hex address A at frame F\n"
        "  --audio-wav PATH      write mono 16-bit 44.1kHz WAV of the OPN output\n"
        "  --audio-range S:E     restrict the WAV to frames [S,E)\n"
        "  --trace-opn PATH      log every OPN register write as cycle,reg,value\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string disk_a;
    std::string disk_b;
    long frames = 600;
    bool trace_boot = false;
    bool cpu_report = false;
    long boot_delay = -1;
    long fdc_latency_us = -1;
    long fdc_step_us = -1;
    bool fdc_stats = false;
    std::string screenshot_path;
    std::string audio_wav_path;
    std::string trace_opn_path;
    bool mute_fm = false, mute_ssg = false;
    long fm_lpf_hz = -1;
    long audio_start = 0, audio_end = -1;
    std::vector<uint16_t> memory_reports;
    std::vector<uint16_t> trace_addrs;

    struct KeyPulse { mz::KeyPos pos; long start, end; };
    struct JoyPulse { uint8_t mask; long start, end; };
    struct Poke { uint16_t addr; uint8_t value; long frame; };
    std::vector<KeyPulse> key_pulses;
    std::vector<JoyPulse> joy_pulses;
    std::vector<Poke> pokes;

    // shared parser for K:S[:N] pulse specs; returns the K part
    auto split_spec = [](const std::string& spec, long& start, long& dur) -> std::string {
        const size_t a = spec.find(':');
        if (a == std::string::npos) return "";
        const size_t b = spec.find(':', a + 1);
        start = std::strtol(spec.c_str() + a + 1, nullptr, 10);
        dur = b == std::string::npos ? 4 : std::strtol(spec.c_str() + b + 1, nullptr, 10);
        return spec.substr(0, a);
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--selftest") {
            return run_selftest();
        } else if (arg == "--ssg-test") {
            return run_ssg_test();
        } else if (arg == "--disk-a") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            disk_a = v;
        } else if (arg == "--disk-b") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            disk_b = v;
        } else if (arg == "--frames") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            frames = std::strtol(v, nullptr, 10);
        } else if (arg == "--trace-boot") {
            trace_boot = true;
        } else if (arg == "--cpu-report") {
            cpu_report = true;
        } else if (arg == "--memory-report") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            memory_reports.push_back(static_cast<uint16_t>(std::strtoul(v, nullptr, 16)));
        } else if (arg == "--trace-memory") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            trace_addrs.push_back(static_cast<uint16_t>(std::strtoul(v, nullptr, 16)));
        } else if (arg == "--boot-delay") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            boot_delay = std::strtol(v, nullptr, 10);
        } else if (arg == "--fdc-latency-us") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            fdc_latency_us = std::strtol(v, nullptr, 10);
        } else if (arg == "--fdc-step-us") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            fdc_step_us = std::strtol(v, nullptr, 10);
        } else if (arg == "--fdc-stats") {
            fdc_stats = true;
        } else if (arg == "--screenshot") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            screenshot_path = v;
        } else if (arg == "--key-pulse") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            long start = 0, dur = 4;
            const std::string name = split_spec(v, start, dur);
            const mz::KeyPos pos = mz::key_from_name(name);
            if (pos.row < 0) { std::fprintf(stderr, "unknown key: %s\n", name.c_str()); return 2; }
            key_pulses.push_back({pos, start, start + dur});
        } else if (arg == "--joy-pulse") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            long start = 0, dur = 4;
            const std::string mask = split_spec(v, start, dur);
            joy_pulses.push_back({static_cast<uint8_t>(std::strtoul(mask.c_str(), nullptr, 16)),
                                  start, start + dur});
        } else if (arg == "--memory-poke") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            unsigned addr = 0, val = 0;
            long frame = 0;
            if (std::sscanf(v, "%x:%x:%ld", &addr, &val, &frame) != 3) { usage(); return 2; }
            pokes.push_back({static_cast<uint16_t>(addr), static_cast<uint8_t>(val), frame});
        } else if (arg == "--audio-wav") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            audio_wav_path = v;
        } else if (arg == "--audio-range") {
            const char* v = value();
            if (!v || std::sscanf(v, "%ld:%ld", &audio_start, &audio_end) != 2) { usage(); return 2; }
        } else if (arg == "--fm-lpf-hz") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            fm_lpf_hz = std::strtol(v, nullptr, 10);
        } else if (arg == "--mute-fm") {
            mute_fm = true;
        } else if (arg == "--mute-ssg") {
            mute_ssg = true;
        } else if (arg == "--trace-opn") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            trace_opn_path = v;
        } else {
            usage();
            return 2;
        }
    }

    if (disk_a.empty()) {
        usage();
        return 2;
    }

    mz::Mz2500 machine;
    machine.set_trace_boot(trace_boot);
    FILE* trace_opn = nullptr;
    if (!trace_opn_path.empty()) {
        trace_opn = std::fopen(trace_opn_path.c_str(), "w");
        if (!trace_opn) { std::fprintf(stderr, "cannot write %s\n", trace_opn_path.c_str()); return 1; }
        machine.opn().set_trace(trace_opn);
    }
    if (boot_delay >= 0) machine.set_boot_delay_frames(static_cast<int>(boot_delay));
    if (fdc_latency_us >= 0) machine.fdc().set_read_latency_us(static_cast<uint32_t>(fdc_latency_us));
    if (fdc_step_us >= 0) machine.fdc().set_step_time_us(static_cast<uint32_t>(fdc_step_us));
    if (mute_fm || mute_ssg) machine.opn().set_layer_gains(mute_fm ? 0.f : 1.f, mute_ssg ? 0.f : 1.f);
    if (fm_lpf_hz >= 0) machine.opn().set_fm_lowpass_hz(static_cast<uint32_t>(fm_lpf_hz));
    if (!machine.insert_disk(0, disk_a)) return 1;
    if (!disk_b.empty() && !machine.insert_disk(1, disk_b)) return 1;
    if (!machine.boot_from_disk()) return 1;

    std::vector<int> trace_last(trace_addrs.size(), -1);
    std::vector<int16_t> wav_samples;
    for (long i = 0; i < frames; i++) {
        for (const auto& p : pokes) {
            if (p.frame == i) machine.poke_memory(p.addr, p.value);
        }
        for (const auto& k : key_pulses) {
            if (i == k.start) machine.set_key(k.pos.row, k.pos.bit, true);
            if (i == k.end) machine.set_key(k.pos.row, k.pos.bit, false);
        }
        uint8_t joy = 0;
        for (const auto& j : joy_pulses) {
            if (i >= j.start && i < j.end) joy |= j.mask;
        }
        machine.set_joystick_mask(joy);
        machine.run_frame();
        {
            float buf[4096];
            size_t n;
            while ((n = machine.opn().read_audio(buf, 4096)) > 0) {
                if (audio_wav_path.empty()) continue;
                if (i < audio_start || (audio_end >= 0 && i >= audio_end)) continue;
                for (size_t s = 0; s < n; s++) {
                    float v = buf[s];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    wav_samples.push_back(static_cast<int16_t>(v * 32767.0f));
                }
            }
        }
        for (size_t t = 0; t < trace_addrs.size(); t++) {
            const uint8_t v = machine.read_memory(trace_addrs[t]);
            if (v != trace_last[t]) {
                trace_last[t] = v;
                std::printf("trace[%04x] frame=%ld value=%02x\n", trace_addrs[t], i, v);
            }
        }
    }

    if (!screenshot_path.empty()) {
        std::vector<uint8_t> rgba(640 * 400 * 4);
        machine.render(rgba.data());
        FILE* f = std::fopen(screenshot_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", screenshot_path.c_str());
            return 1;
        }
        std::fprintf(f, "P6\n640 400\n255\n");
        for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
        std::fclose(f);
    }

    if (cpu_report) {
        const z80& c = machine.cpu();
        std::printf("cpu: pc=%04x sp=%04x a=%02x bc=%04x de=%04x hl=%04x "
                    "halted=%d iff1=%d im=%d cycles=%llu\n",
                    c.pc, c.sp, c.a, (c.b << 8) | c.c, (c.d << 8) | c.e,
                    (c.h << 8) | c.l, c.halted, c.iff1, c.interrupt_mode,
                    static_cast<unsigned long long>(machine.cycles()));
    }
    for (uint16_t addr : memory_reports) {
        std::printf("memory[%04x]=%02x\n", addr, machine.read_memory(addr));
    }
    if (trace_opn) std::fclose(trace_opn);
    if (!audio_wav_path.empty()) {
        if (!mz::write_wav16(audio_wav_path, wav_samples, machine.opn().output_rate(), 1)) {
            std::fprintf(stderr, "cannot write %s\n", audio_wav_path.c_str());
            return 1;
        }
        std::printf("wav: %zu samples -> %s\n", wav_samples.size(), audio_wav_path.c_str());
    }
    if (fdc_stats) {
        std::printf("fdc: reads=%llu seeks=%llu steps=%llu\n",
                    (unsigned long long)machine.fdc().stat_reads,
                    (unsigned long long)machine.fdc().stat_seeks,
                    (unsigned long long)machine.fdc().stat_steps);
    }
    return 0;
}
