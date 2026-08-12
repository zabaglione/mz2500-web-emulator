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

#include "cli/key_schedule.h"
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
        "  --disk-b-blank        put an unformatted blank disk in drive 1 (FD2)\n"
        "  --disk-save N:PATH    write drive N's image out at exit (write tests)\n"
        "  --frames N            run N emulated frames (default 600)\n"
        "  --trace-boot          log dummy-IPL and boot progress\n"
        "  --cpu-report          print CPU state at exit\n"
        "  --stall-profile FRAME profile VRAM waits by instruction from FRAME on\n"
        "  --stall-top N         sites the stall profile lists (default 20)\n"
        "  --sprite-cells        per frame, count text cells holding a sprite code\n"
        "  --loop-monitor A[:B]  cycles from A to B (hex) per pass, from --stall-profile\n"
        "  --memory-report ADDR  print memory[ADDR] at exit (hex, repeatable)\n"
        "  --trace-memory ADDR   log every frame where memory[ADDR] changes\n"
        "  --boot-delay N        override IPL boot delay in frames (calibration)\n"
        "  --fdc-latency-us N    override FDC per-read latency (calibration)\n"
        "  --screenshot PATH     write the final frame as a 640x400 PPM (P6)\n"
        "  --frame-dump D:S:E    write every frame in [S,E] as D/frameNNNNNN.ppm\n"
        "  --reboot-at N         reboot the same machine at frame N (the web\n"
        "                        front end's RESET, which reuses the machine)\n"
        "  --key-pulse K:S[:N]   hold key K for N frames (default 4) from frame S\n"
        "                        K is a name from core/keyboard.h, or a raw\n"
        "                        R,B matrix position (row 0-13, bit 0-7)\n"
        "  --type STR:FRAME      type STR into the machine from frame FRAME\n"
        "                        (4 frames per character; \\r for RETURN)\n"
        "  --joy-pulse M:S[:N]   hold joystick mask M (hex) for N frames from S\n"
        "  --memory-poke A:V:F   write hex value V to hex address A at frame F\n"
        "  --sio-b BYTES:FRAME   feed hex bytes to SIO channel B (the mouse line)\n"
        "  --mouse-move DX:DY:FRAME  inject host mouse movement at frame FRAME\n"
        "                        (drives Mz2500::mouse_move(), through the real\n"
        "                        DTR strobe path, not straight into the SIO line)\n"
        "  --audio-wav PATH      write mono 16-bit 44.1kHz WAV of the OPN output\n"
        "  --audio-range S:E     restrict the WAV to frames [S,E)\n"
        "  --trace-opn PATH      log every OPN register write as cycle,reg,value\n"
        "  --no-adpcm            remove the MZ-1E35 ADPCM board (ports 98h/99h)\n"
        "  --no-emm              remove the MZ-1R37 640K EMM (ports ACh/ADh)\n");
}

// Render the current frame to a 640x400 binary PPM. Shared by --screenshot
// and --frame-dump so both produce byte-identical files.
bool write_ppm(const mz::Mz2500& machine, const std::string& path) {
    std::vector<uint8_t> rgba(640 * 400 * 4);
    machine.render(rgba.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    std::fprintf(f, "P6\n640 400\n255\n");
    for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string disk_a;
    std::string disk_b;
    bool disk_b_blank = false;
    int disk_save_drive = -1;
    std::string disk_save_path;
    long frames = 600;
    bool trace_boot = false;
    bool cpu_report = false;
    long stall_profile_from = -1; // frame the VRAM wait profiler starts at
    int stall_top = 20;           // sites listed in its report
    bool sprite_cells = false;    // per-frame count of text cells >= 128
    int loop_monitor_pc = -1;     // address whose pass cost is measured
    int loop_monitor_end = -1;    // address the pass ends at (default: same)
    long boot_delay = -1;
    long reboot_at = -1;
    long fdc_latency_us = -1;
    long fdc_step_us = -1;
    bool fdc_stats = false;
    std::string screenshot_path;
    std::string frame_dump_dir;
    long frame_dump_start = 0, frame_dump_end = -1;
    std::string audio_wav_path;
    std::string trace_opn_path;
    bool mute_fm = false, mute_ssg = false;
    long fm_lpf_hz = -1;
    std::string rom_dir;
    bool real_ipl = false;
    bool no_exp_ram = false, no_exp_gram = false, no_mz1m10 = false;
    bool no_adpcm = false, no_emm = false;
    long trace_trap = -1;
    bool trace_io = false;
    bool dump_io = false;
    long ram_dump_addr = -1; unsigned ram_dump_len = 0;
    long phys_dump_addr = -1; unsigned phys_dump_len = 0;
    long audio_start = 0, audio_end = -1;
    std::vector<uint16_t> memory_reports;
    std::vector<uint16_t> trace_addrs;

    // KeyPulse/TypedKey live in cli/key_schedule.h so the per-frame union
    // logic (mz_cli::keys_down_at_frame) can be unit-tested independently
    // of main()'s argv parsing and frame loop.
    using KeyPulse = mz_cli::KeyPulse;
    using TypedKey = mz_cli::TypedKey;
    struct JoyPulse { uint8_t mask; long start, end; };
    struct Poke { uint16_t addr; uint8_t value; long frame; };
    struct SioFeed { uint8_t value; long frame; };
    struct MouseMove { int dx, dy; long frame; };
    std::vector<KeyPulse> key_pulses;
    std::vector<TypedKey> typed;
    std::vector<JoyPulse> joy_pulses;
    std::vector<Poke> pokes;
    std::vector<SioFeed> sio_feeds;
    std::vector<MouseMove> mouse_moves;

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
        } else if (arg == "--disk-b-blank") {
            disk_b_blank = true;
        } else if (arg == "--disk-save") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            const std::string spec = v;
            const size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--disk-save requires N:PATH (e.g. 1:/tmp/out.d88)\n");
                return 2;
            }
            // N must be a plain drive number: std::atoi silently returns 0
            // for "x" (a typo'd drive) and truncates "5" down to whatever an
            // out-of-range int decays to in the drive array lookup,
            // producing a saved file for the wrong drive (or, for a
            // negative/garbage value that also fails the >=0 check
            // downstream, no file and no message at all). Validate with
            // strtol's endptr the same way --type does below, instead of
            // coercing garbage into a plausible-looking drive index.
            const std::string drive_str = spec.substr(0, colon);
            char* drive_end = nullptr;
            const long drive = drive_str.empty() ? -1
                                                   : std::strtol(drive_str.c_str(), &drive_end, 10);
            if (drive_str.empty() || drive_end != drive_str.c_str() + drive_str.size() ||
                drive < 0 || drive >= mz::FdcMb8877::NUM_DRIVES) {
                std::fprintf(stderr,
                              "--disk-save requires N to be a drive number 0-%d (got '%s')\n",
                              mz::FdcMb8877::NUM_DRIVES - 1, drive_str.c_str());
                return 2;
            }
            disk_save_drive = (int)drive;
            disk_save_path = spec.substr(colon + 1);
        } else if (arg == "--frames") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            frames = std::strtol(v, nullptr, 10);
        } else if (arg == "--trace-boot") {
            trace_boot = true;
        } else if (arg == "--cpu-report") {
            cpu_report = true;
        } else if (arg == "--stall-profile") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            stall_profile_from = std::strtol(v, nullptr, 10);
        } else if (arg == "--loop-monitor") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            {
                const std::string spec(v);
                const size_t colon = spec.find(':');
                loop_monitor_pc = (int)std::strtoul(spec.c_str(), nullptr, 16);
                loop_monitor_end = colon == std::string::npos
                                       ? loop_monitor_pc
                                       : (int)std::strtoul(spec.c_str() + colon + 1, nullptr, 16);
            }
        } else if (arg == "--sprite-cells") {
            sprite_cells = true;
        } else if (arg == "--stall-top") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            stall_top = (int)std::strtol(v, nullptr, 10);
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
        } else if (arg == "--frame-dump") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            const std::string spec = v;
            const size_t c1 = spec.rfind(':');
            const size_t c0 = c1 == std::string::npos ? c1 : spec.rfind(':', c1 - 1);
            if (c0 == std::string::npos || c1 == std::string::npos) {
                std::fprintf(stderr, "--frame-dump wants DIR:START:END\n");
                return 2;
            }
            frame_dump_dir = spec.substr(0, c0);
            frame_dump_start = std::strtol(spec.c_str() + c0 + 1, nullptr, 10);
            frame_dump_end = std::strtol(spec.c_str() + c1 + 1, nullptr, 10);
        } else if (arg == "--key-pulse") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            long start = 0, dur = 4;
            const std::string name = split_spec(v, start, dur);
            mz::KeyPos pos;
            const size_t comma = name.find(',');
            if (comma != std::string::npos) {
                // Numeric R,B form: probe any of the 14x8 matrix positions
                // directly (strobe row 0-13, data bit 0-7) without having to
                // invent a name for it first - needed to sweep the whole
                // matrix looking for undocumented keys (e.g. ALGO).
                const std::string row_str = name.substr(0, comma);
                const std::string bit_str = name.substr(comma + 1);
                char* row_end = nullptr;
                char* bit_end = nullptr;
                const long row = row_str.empty() ? -1 : std::strtol(row_str.c_str(), &row_end, 10);
                const long bit = bit_str.empty() ? -1 : std::strtol(bit_str.c_str(), &bit_end, 10);
                if (row_str.empty() || bit_str.empty() ||
                    row_end != row_str.c_str() + row_str.size() ||
                    bit_end != bit_str.c_str() + bit_str.size() ||
                    row < 0 || row > 13 || bit < 0 || bit > 7) {
                    std::fprintf(stderr,
                                  "--key-pulse numeric form wants R,B with row 0-13 and bit "
                                  "0-7 (got '%s')\n",
                                  name.c_str());
                    return 2;
                }
                pos = {(int)row, (int)bit};
            } else {
                pos = mz::key_from_name(name);
                if (pos.row < 0) { std::fprintf(stderr, "unknown key: %s\n", name.c_str()); return 2; }
            }
            key_pulses.push_back({pos, start, start + dur});
        } else if (arg == "--type") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            const std::string spec = v;
            // Split on the LAST colon: STR itself may contain colons, e.g.
            // save "FD2:TEST"
            const size_t colon = spec.rfind(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--type wants STR:FRAME\n");
                return 2;
            }
            std::string text = spec.substr(0, colon);
            // FRAME must be a plain non-negative integer: std::atol silently
            // returns 0 for "abc" or a bare trailing colon, and a negative
            // frame would place the whole press/release window below frame
            // 0 so the character is never actually pressed - both failure
            // modes are otherwise silent. Validate with strtol's endptr
            // instead of atol so garbage is caught rather than coerced.
            const std::string frame_str = spec.substr(colon + 1);
            char* frame_end = nullptr;
            const long at = frame_str.empty() ? -1
                                               : std::strtol(frame_str.c_str(), &frame_end, 10);
            if (frame_str.empty() || frame_end != frame_str.c_str() + frame_str.size() ||
                at < 0) {
                std::fprintf(stderr,
                              "--type wants STR:FRAME with a non-negative integer FRAME "
                              "(got '%s')\n",
                              frame_str.c_str());
                return 2;
            }
            long at_frame = at;
            // \r in the string stands for RETURN, so a whole command line
            // including its terminator fits in one argument
            std::string expanded;
            for (size_t k = 0; k < text.size(); k++) {
                if (text[k] == '\\' && k + 1 < text.size() && text[k + 1] == 'r') {
                    expanded.push_back('\r');
                    k++;
                } else {
                    expanded.push_back(text[k]);
                }
            }
            for (char ch : expanded) {
                bool shift = false;
                const mz::KeyPos pos = mz::key_for_char(ch, shift);
                if (pos.row < 0) {
                    std::fprintf(stderr, "--type cannot produce '%c'\n", ch);
                    return 2;
                }
                typed.push_back({pos, shift, at_frame, at_frame + 4});
                at_frame += 8; // 4 frames down, 4 frames up: BASIC's key scan needs the gap
            }
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
        } else if (arg == "--sio-b") {
            // BYTES:FRAME - hand a hex byte string to SIO channel B, the
            // line the mouse arrives on, one byte per frame from FRAME
            const char* v = value();
            if (!v) { usage(); return 2; }
            const char* colon = std::strchr(v, ':');
            if (!colon) { usage(); return 2; }
            const long frame = std::strtol(colon + 1, nullptr, 10);
            for (const char* h = v; h + 1 < colon; h += 2) {
                const char pair[3] = {h[0], h[1], 0};
                sio_feeds.push_back({(uint8_t)std::strtoul(pair, nullptr, 16),
                                     frame + (long)(h - v) / 2});
            }
        } else if (arg == "--mouse-move") {
            // DX:DY:FRAME - queue host mouse movement to be delivered through
            // Mz2500::mouse_move() at a given frame, exercising the DTR
            // strobe path end to end (unlike --sio-b, which pushes bytes
            // straight into the SIO receiver and so proves only the driver's
            // parsing, not this task's wiring).
            const char* v = value();
            if (!v) { usage(); return 2; }
            int dx = 0, dy = 0;
            long frame = 0;
            if (std::sscanf(v, "%d:%d:%ld", &dx, &dy, &frame) != 3) { usage(); return 2; }
            mouse_moves.push_back({dx, dy, frame});
        } else if (arg == "--audio-wav") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            audio_wav_path = v;
        } else if (arg == "--audio-range") {
            const char* v = value();
            if (!v || std::sscanf(v, "%ld:%ld", &audio_start, &audio_end) != 2) { usage(); return 2; }
        } else if (arg == "--rom-dir") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            rom_dir = v;
        } else if (arg == "--real-ipl") {
            real_ipl = true;
        } else if (arg == "--reboot-at") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            reboot_at = std::strtol(v, nullptr, 10);
        } else if (arg == "--phys-dump") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            unsigned a=0, l=0;
            if (std::sscanf(v, "%x:%x", &a, &l) != 2) { usage(); return 2; }
            phys_dump_addr = a; phys_dump_len = l;
        } else if (arg == "--ram-dump") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            unsigned a=0, l=0;
            if (std::sscanf(v, "%x:%x", &a, &l) != 2) { usage(); return 2; }
            ram_dump_addr = a; ram_dump_len = l;
        } else if (arg == "--dump-io") {
            dump_io = true;
        } else if (arg == "--trace-io") {
            trace_io = true;
        } else if (arg == "--trace-trap") {
            const char* v = value();
            if (!v) { usage(); return 2; }
            trace_trap = std::strtol(v, nullptr, 16);
        } else if (arg == "--no-exp-ram") {
            no_exp_ram = true;
        } else if (arg == "--no-exp-gram") {
            no_exp_gram = true;
        } else if (arg == "--no-mz1m10") {
            no_mz1m10 = true;
        } else if (arg == "--no-adpcm") {
            no_adpcm = true;
        } else if (arg == "--no-emm") {
            no_emm = true;
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

    // A run that only wants a blank disk formatted and saved back out (the
    // write tests' use case) has no boot disk and no IPL to run - disk_a is
    // only mandatory for the normal boot-and-run path.
    if (disk_a.empty() && !disk_b_blank) {
        usage();
        return 2;
    }
    // --disk-b loads an image into drive 1; --disk-b-blank immediately
    // replaces drive 1 with an unformatted disk. Together they'd silently
    // discard whichever the caller thought would win, so reject the
    // combination instead of guessing.
    if (!disk_b.empty() && disk_b_blank) {
        std::fprintf(stderr, "--disk-b and --disk-b-blank conflict: pick one for drive 1\n");
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
    if (trace_trap >= 0) machine.set_trap_watch((uint16_t)trace_trap);
    if (trace_io) machine.set_trace_io(true);
    if (no_exp_ram) machine.set_hw_option(0, false);
    if (no_exp_gram) machine.set_hw_option(1, false);
    if (no_mz1m10) machine.set_hw_option(2, false);
    if (no_adpcm) machine.set_hw_option(3, false);
    if (no_emm) machine.set_hw_option(4, false);
    if (!rom_dir.empty()) {
        static const struct { const char* file; int kind; } roms[] = {
            {"ipl.rom", 0}, {"kanji.rom", 2}, {"dict.rom", 3}};
        for (const auto& r : roms) {
            const std::string path = rom_dir + "/" + r.file;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) continue;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> bytes(size > 0 ? size : 0);
            if (size > 0 && std::fread(bytes.data(), 1, size, f) == (size_t)size)
                machine.set_rom(r.kind, bytes.data(), bytes.size());
            std::fclose(f);
        }
    }

    if (!disk_a.empty() && !machine.insert_disk(0, disk_a)) return 1;
    if (!disk_b.empty() && !machine.insert_disk(1, disk_b)) return 1;
    if (disk_b_blank && !machine.insert_blank_disk(1)) {
        std::fprintf(stderr, "cannot create blank disk\n");
        return 1;
    }
    if (!disk_a.empty()) {
        if (real_ipl) {
            if (!machine.boot_with_real_ipl()) return 1;
        } else if (!machine.boot_from_disk()) {
            return 1;
        }
    }

    std::vector<int> trace_last(trace_addrs.size(), -1);
    std::vector<int16_t> wav_samples;
    // Matrix positions held down as of the previous frame. Carried across
    // iterations so we only issue set_key() on an actual transition.
    std::vector<mz::KeyPos> held_keys;
    for (long i = 0; i < frames; i++) {
        if (i == reboot_at) {
            // Reboot the SAME machine, the way the web front end's RESET
            // button does. Every other CLI run gets a machine straight from
            // its constructor, so this is the only way to test what a boot
            // does and does not carry over from the session before it.
            const bool ok = real_ipl ? machine.boot_with_real_ipl()
                                     : machine.boot_from_disk();
            if (!ok) {
                std::fprintf(stderr, "reboot at frame %ld failed\n", i);
                return 1;
            }
        }
        for (const auto& p : pokes) {
            if (p.frame == i) machine.poke_memory(p.addr, p.value);
        }
        for (const auto& f : sio_feeds) {
            if (f.frame == i) machine.sio_receive(1, f.value);
        }
        for (const auto& mv : mouse_moves) {
            if (mv.frame == i) machine.mouse_move(mv.dx, mv.dy);
        }
        // Compute this frame's whole desired matrix state from every
        // window-based source at once (mz_cli::keys_down_at_frame), then
        // apply it in a single pass below. Two sources holding the same key
        // - e.g. an overlapping --key-pulse and --type - no longer fight:
        // the key stays down as long as ANY source's window covers this
        // frame, and only goes up once no source wants it held.
        const std::vector<mz::KeyPos> desired =
            mz_cli::keys_down_at_frame(i, key_pulses, typed);
        auto contains = [](const std::vector<mz::KeyPos>& v, mz::KeyPos pos) {
            for (const auto& p : v) {
                if (p.row == pos.row && p.bit == pos.bit) return true;
            }
            return false;
        };
        for (const auto& pos : held_keys) {
            if (!contains(desired, pos)) machine.set_key(pos.row, pos.bit, false);
        }
        for (const auto& pos : desired) {
            if (!contains(held_keys, pos)) machine.set_key(pos.row, pos.bit, true);
        }
        held_keys = desired;
        uint8_t joy = 0;
        for (const auto& j : joy_pulses) {
            if (i >= j.start && i < j.end) joy |= j.mask;
        }
        machine.set_joystick_mask(joy);
        if (stall_profile_from >= 0 && i == stall_profile_from) {
            machine.enable_stall_profile(true);
            machine.reset_stall_profile();
            if (loop_monitor_pc >= 0) machine.watch_loop(loop_monitor_pc, loop_monitor_end);
        }
        machine.run_frame();
        {
            float buf[4096];
            size_t n;
            while ((n = machine.read_audio(buf, 4096)) > 0) {
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
        if (!frame_dump_dir.empty() && i >= frame_dump_start && i <= frame_dump_end) {
            char name[64];
            std::snprintf(name, sizeof(name), "/frame%06ld.ppm", i);
            if (!write_ppm(machine, frame_dump_dir + name)) return 1;
        }
        if (sprite_cells) {
            // Text VRAM is bank 38h; the character plane is its first 2,000
            // bytes (80x25). Everything the game draws on the text layer -
            // the cat, the dogs, the boss, the HUD - is a non-blank cell, so
            // a dip in this count is a sprite the frame ended up not showing.
            const uint8_t* tv = machine.memory().bank_ptr(0x38);
            int n = 0, dogs = 0;
            for (int c = 0; c < 2000; c++) {
                if (tv[c]) n++;
                if (tv[c] >= 128 && tv[c] <= 207) dogs++; // the dogs' own codes
            }
            std::printf("cells frame=%ld n=%d dog=%d\n", i, n, dogs);
        }
        for (size_t t = 0; t < trace_addrs.size(); t++) {
            const uint8_t v = machine.read_memory(trace_addrs[t]);
            if (v != trace_last[t]) {
                trace_last[t] = v;
                std::printf("trace[%04x] frame=%ld value=%02x\n", trace_addrs[t], i, v);
            }
        }
    }

    if (!screenshot_path.empty() && !write_ppm(machine, screenshot_path)) return 1;

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
    if (phys_dump_addr >= 0) {
        // bank-map independent: reads the physical 512KB array directly
        for (unsigned i = 0; i < phys_dump_len; i++) {
            const unsigned off = (unsigned)phys_dump_addr + i;
            if (i % 16 == 0) std::printf("\nphys %05x:", off);
            std::printf(" %02x", machine.memory().bank_ptr((int)(off >> 13))[off & 0x1FFF]);
        }
        std::printf("\n");
    }
    if (ram_dump_addr >= 0) {
        for (unsigned i = 0; i < ram_dump_len; i++) {
            if (i % 16 == 0) std::printf("\n%04x:", (unsigned)(ram_dump_addr + i));
            std::printf(" %02x", machine.read_memory((uint16_t)(ram_dump_addr + i)));
        }
        std::printf("\n");
    }
    if (dump_io) machine.dump_forensics("exit");
    if (trace_opn) std::fclose(trace_opn);
    if (!audio_wav_path.empty()) {
        if (!mz::write_wav16(audio_wav_path, wav_samples, machine.opn().output_rate(), 1)) {
            std::fprintf(stderr, "cannot write %s\n", audio_wav_path.c_str());
            return 1;
        }
        std::printf("wav: %zu samples -> %s\n", wav_samples.size(), audio_wav_path.c_str());
    }
    if (stall_profile_from >= 0) {
        const std::vector<mz::Mz2500::StallSite> sites = machine.stall_sites();
        const double f = (double)std::max<uint64_t>(1, machine.stall_profile_frames());
        uint64_t total_stall = 0, total_weight = 0, total_hits = 0;
        for (const auto& s : sites) {
            total_stall += s.stall;
            total_weight += s.weight;
            total_hits += s.hits;
        }
        std::printf("stall: frames=%llu accesses/frame=%.0f stall/frame=%.0f "
                    "weight/frame=%.0f budget=%d (%.1f%% of the frame)\n",
                    (unsigned long long)machine.stall_profile_frames(), total_hits / f,
                    total_stall / f, total_weight / f, mz::CYCLES_PER_FRAME,
                    100.0 * (total_stall + total_weight) / f / mz::CYCLES_PER_FRAME);
        std::printf("stall:   pc  accesses/frame  stall/frame  cycles/access\n");
        for (int n = 0; n < stall_top && n < (int)sites.size(); n++) {
            const mz::Mz2500::StallSite& s = sites[n];
            std::printf("stall: %04x  %13.1f  %11.0f  %13.1f\n", s.pc, s.hits / f,
                        s.stall / f, (double)(s.stall + s.weight) / (double)s.hits);
        }
    }
    if (loop_monitor_pc >= 0) {
        const mz::Mz2500::LoopStats& l = machine.loop_stats();
        const double mean = l.passes ? (double)l.total / (double)l.passes : 0.0;
        const double over_mean = l.over ? (double)l.over_total / (double)l.over : 0.0;
        std::printf("loop: pc=%04x->%04x passes=%llu mean=%.0f (%.3f frames) worst=%.3f frames\n"
                    "loop: over deadline: %llu passes (%.1f%%), mean %.0f (%.3f frames)\n",
                    loop_monitor_pc, loop_monitor_end, (unsigned long long)l.passes, mean,
                    mean / mz::CYCLES_PER_FRAME,
                    (double)l.worst / mz::CYCLES_PER_FRAME,
                    (unsigned long long)l.over,
                    l.passes ? 100.0 * (double)l.over / (double)l.passes : 0.0, over_mean,
                    over_mean / mz::CYCLES_PER_FRAME);
    }
    if (fdc_stats) {
        std::printf("fdc: reads=%llu seeks=%llu steps=%llu\n",
                    (unsigned long long)machine.fdc().stat_reads,
                    (unsigned long long)machine.fdc().stat_seeks,
                    (unsigned long long)machine.fdc().stat_steps);
    }
    if (disk_save_drive >= 0 && !disk_save_path.empty()) {
        const std::vector<uint8_t> image = machine.disk_image(disk_save_drive);
        FILE* f = std::fopen(disk_save_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", disk_save_path.c_str());
            return 1;
        }
        std::fwrite(image.data(), 1, image.size(), f);
        std::fclose(f);
        std::printf("wrote %s (%zu bytes)\n", disk_save_path.c_str(), image.size());
    }
    return 0;
}
