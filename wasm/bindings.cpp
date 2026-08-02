// Emscripten bindings: a thin C ABI over the machine for the JS frontend.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/mz2500.h"

namespace {
mz::Mz2500* g_machine = nullptr;
std::vector<uint8_t> g_frame(640 * 400 * 4);
std::vector<float> g_audio(16384);
} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int emu_init(int audio_rate) {
    delete g_machine;
    g_machine = new mz::Mz2500();
    if (audio_rate > 0) g_machine->opn().set_output_rate(static_cast<uint32_t>(audio_rate));
    return 1;
}

// Hot-insert a disk into drive 0 or 1 (no reset - mid-game swaps work).
// data is copied; returns 1 when the image parses.
EMSCRIPTEN_KEEPALIVE int emu_insert_disk(int drive, const uint8_t* data, int size) {
    if (!g_machine || size <= 0) return 0;
    std::vector<uint8_t> bytes(data, data + size);
    return g_machine->insert_disk_bytes(drive, std::move(bytes)) ? 1 : 0;
}

// Cold boot from the disk in drive 0 (the dummy IPL path)
EMSCRIPTEN_KEEPALIVE int emu_boot() {
    return (g_machine && g_machine->boot_from_disk()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_run_frame() {
    if (g_machine) g_machine->run_frame();
}

EMSCRIPTEN_KEEPALIVE uint8_t* emu_frame_buffer() { return g_frame.data(); }

EMSCRIPTEN_KEEPALIVE void emu_render() {
    if (g_machine) g_machine->render(g_frame.data());
}

EMSCRIPTEN_KEEPALIVE float* emu_audio_buffer() { return g_audio.data(); }

EMSCRIPTEN_KEEPALIVE int emu_audio_capacity() { return (int)g_audio.size(); }

EMSCRIPTEN_KEEPALIVE int emu_read_audio() {
    if (!g_machine) return 0;
    return (int)g_machine->opn().read_audio(g_audio.data(), g_audio.size());
}

EMSCRIPTEN_KEEPALIVE void emu_key(int row, int bit, int down) {
    if (g_machine) g_machine->set_key(row, bit, down != 0);
}

EMSCRIPTEN_KEEPALIVE void emu_joy(int mask) {
    if (g_machine) g_machine->set_joystick_mask(static_cast<uint8_t>(mask));
}

EMSCRIPTEN_KEEPALIVE int emu_frames() {
    return g_machine ? (int)g_machine->frames() : 0;
}

// FDD access lamps: bit n = drive n LED (drive-select + motor line, like
// the LED on a real drive)
EMSCRIPTEN_KEEPALIVE int emu_fdd_lamps() {
    return g_machine ? g_machine->fdc().lamp_mask() : 0;
}

} // extern "C"
