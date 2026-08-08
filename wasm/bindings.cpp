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
    if (audio_rate > 0) g_machine->set_audio_rate(static_cast<uint32_t>(audio_rate));
    return 1;
}

// Hot-insert a disk into drive 0 or 1 (no reset - mid-game swaps work).
// data is copied; returns 1 when the image parses.
EMSCRIPTEN_KEEPALIVE int emu_insert_disk(int drive, const uint8_t* data, int size) {
    if (!g_machine || size <= 0) return 0;
    std::vector<uint8_t> bytes(data, data + size);
    return g_machine->insert_disk_bytes(drive, std::move(bytes)) ? 1 : 0;
}

// Put an unformatted disk in a drive: the write tests' target.
EMSCRIPTEN_KEEPALIVE int emu_insert_blank_disk(int drive) {
    if (!g_machine) return 0;
    return g_machine->insert_blank_disk(drive) ? 1 : 0;
}

// ---- written disks: hand the image back for the browser to keep ---------
namespace {
std::vector<uint8_t> g_disk_out;
}

EMSCRIPTEN_KEEPALIVE int emu_disk_dirty(int drive) {
    return (g_machine && g_machine->disk_dirty(drive)) ? 1 : 0;
}

// Snapshot a drive's image into a buffer emu_disk_data() then points at,
// and report how many bytes it holds. Two calls rather than one because the
// JS side needs the length before it can read the memory.
//
// Calling contract: g_disk_out is assigned by value on every call, so its
// underlying address can move each time emu_disk_snapshot() runs. The
// pointer emu_disk_data() returns is only valid until the *next* call to
// emu_disk_snapshot() (for either drive). Callers MUST: call
// emu_disk_snapshot(drive), then emu_disk_data(), then copy the bytes out
// - all before making any other emu_disk_snapshot() call. Never cache the
// emu_disk_data() pointer across calls (e.g. snapshotting drive 0, then
// drive 1, then reading both) - a snapshot of the other drive in between
// invalidates it. This differs from emu_frame_buffer() and
// emu_audio_buffer() below, whose backing storage is fixed-size and never
// reassigned, so their pointers stay stable across calls.
EMSCRIPTEN_KEEPALIVE int emu_disk_snapshot(int drive) {
    if (!g_machine) return 0;
    g_disk_out = g_machine->disk_image(drive);
    return (int)g_disk_out.size();
}

EMSCRIPTEN_KEEPALIVE const uint8_t* emu_disk_data() {
    return g_disk_out.data();
}

EMSCRIPTEN_KEEPALIVE void emu_disk_clear_dirty(int drive) {
    if (g_machine) g_machine->clear_disk_dirty(drive);
}

EMSCRIPTEN_KEEPALIVE void emu_disk_set_wp(int drive, int on) {
    if (g_machine) g_machine->set_disk_write_protected(drive, on != 0);
}

EMSCRIPTEN_KEEPALIVE int emu_disk_wp(int drive) {
    return (g_machine && g_machine->disk_write_protected(drive)) ? 1 : 0;
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
    return (int)g_machine->read_audio(g_audio.data(), g_audio.size());
}

EMSCRIPTEN_KEEPALIVE void emu_key(int row, int bit, int down) {
    if (g_machine) g_machine->set_key(row, bit, down != 0);
}

EMSCRIPTEN_KEEPALIVE void emu_joy(int mask) {
    if (g_machine) g_machine->set_joystick_mask(static_cast<uint8_t>(mask));
}

// Host mouse. Movement is in machine units: the browser scales its own
// movementX/movementY before calling, because the machine's own ratio
// setting belongs to the software running on it.
EMSCRIPTEN_KEEPALIVE void emu_mouse_motion(int dx, int dy) {
    if (g_machine) g_machine->mouse_move(dx, dy);
}

EMSCRIPTEN_KEEPALIVE void emu_mouse_button(int index, int down) {
    if (g_machine) g_machine->mouse_button(index, down != 0);
}

EMSCRIPTEN_KEEPALIVE int emu_frames() {
    return g_machine ? (int)g_machine->frames() : 0;
}

// FDD access lamps: bit n = drive n LED (drive-select + motor line, like
// the LED on a real drive)
EMSCRIPTEN_KEEPALIVE int emu_fdd_lamps() {
    return g_machine ? g_machine->fdc().lamp_mask() : 0;
}

// ---- user ROM slots (files stay in the browser; never bundled) ----------
EMSCRIPTEN_KEEPALIVE int emu_set_rom(int kind, const uint8_t* data, int size) {
    if (!g_machine || size <= 0) return 0;
    g_machine->set_rom(kind, data, (size_t)size);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int emu_has_ipl() {
    return (g_machine && g_machine->has_ipl_rom()) ? 1 : 0;
}

// experimental: cold boot through the user-provided IPL ROM
EMSCRIPTEN_KEEPALIVE int emu_boot_real_ipl() {
    return (g_machine && g_machine->boot_with_real_ipl()) ? 1 : 0;
}

// expansion boards: kind 0=exp RAM, 1=exp GRAM, 2=MZ-1M10 palette
EMSCRIPTEN_KEEPALIVE void emu_set_hw_option(int kind, int on) {
    if (g_machine) g_machine->set_hw_option(kind, on != 0);
}

// ---- debug panel ---------------------------------------------------------
namespace {
char g_debug[1024];
}

EMSCRIPTEN_KEEPALIVE const char* emu_debug_json() {
    if (!g_machine) return "";
    g_machine->debug_json(g_debug, sizeof(g_debug));
    return g_debug;
}

EMSCRIPTEN_KEEPALIVE int emu_read_mem(int addr) {
    return g_machine ? g_machine->read_memory((uint16_t)addr) : 0;
}

// ---- MCP server observability (mcp/) ------------------------------------

EMSCRIPTEN_KEEPALIVE void emu_poke(int addr, int value) {
    if (g_machine) g_machine->poke_memory((uint16_t)addr, (uint8_t)value);
}

// Physical address space: 64 banks x 8KB, bypassing the CPU bank map.
EMSCRIPTEN_KEEPALIVE int emu_read_phys(int phys) {
    if (!g_machine || phys < 0 || phys >= 64 * 0x2000) return 0;
    return g_machine->memory().bank_ptr(phys >> 13)[phys & 0x1FFF];
}

namespace {
char g_text[8192];
}

// UTF-8 dump of the text layer (Mz2500::screen_text in renderer.cpp)
EMSCRIPTEN_KEEPALIVE const char* emu_screen_text() {
    if (!g_machine) return "";
    g_machine->screen_text(g_text, sizeof(g_text));
    return g_text;
}

// The 256-byte OPN register shadow. Pointer is stable while the machine
// lives; re-fetch after emu_init().
EMSCRIPTEN_KEEPALIVE const uint8_t* emu_opn_regs() {
    return g_machine ? g_machine->opn_reg_shadow() : nullptr;
}

// FM key-on slot mask for channel 0-2 (from OPN reg 28h writes)
EMSCRIPTEN_KEEPALIVE int emu_fm_keyon(int ch) {
    return g_machine ? g_machine->fm_keyon(ch) : 0;
}

// BEEP speaker line (8255 port C bit2)
EMSCRIPTEN_KEEPALIVE int emu_beep() {
    return (g_machine && g_machine->beep_on()) ? 1 : 0;
}

// count of non-black pixels in the last rendered frame (blank-screen detector)
EMSCRIPTEN_KEEPALIVE int emu_frame_nonblack() {
    int n = 0;
    for (size_t i = 0; i < g_frame.size(); i += 4) {
        if (g_frame[i] | g_frame[i + 1] | g_frame[i + 2]) n++;
    }
    return n;
}

} // extern "C"
