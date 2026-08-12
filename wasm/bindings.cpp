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
std::vector<uint8_t> g_cmt_out;
std::vector<uint8_t> g_printer_out;
std::vector<uint8_t> g_sasi_out;
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

// ---- built-in cassette data recorder -----------------------------------

EMSCRIPTEN_KEEPALIVE int emu_cmt_insert_wav(const uint8_t* data, int size) {
    return (g_machine && data && size > 0 &&
            g_machine->insert_cmt_wav(data, static_cast<size_t>(size))) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_create_blank(int seconds) {
    return (g_machine && seconds > 0 &&
            g_machine->create_blank_cmt(static_cast<uint32_t>(seconds))) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_cmt_eject() {
    if (g_machine) g_machine->eject_cmt();
}

EMSCRIPTEN_KEEPALIVE void emu_cmt_command(int command) {
    if (g_machine) g_machine->cmt_manual_command(command);
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_loaded() {
    return (g_machine && g_machine->cmt_loaded()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_transport() {
    return g_machine ? g_machine->cmt_transport() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_recording() {
    return (g_machine && g_machine->cmt_recording()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_position_ms() {
    return g_machine ? static_cast<int>(g_machine->cmt_position_ms()) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_duration_ms() {
    return g_machine ? static_cast<int>(g_machine->cmt_duration_ms()) : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_cmt_set_wp(int on) {
    if (g_machine) g_machine->set_cmt_write_protected(on != 0);
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_wp() {
    return (g_machine && g_machine->cmt_write_protected()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_dirty() {
    return (g_machine && g_machine->cmt_dirty()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_cmt_snapshot() {
    if (!g_machine) return 0;
    g_cmt_out = g_machine->cmt_wav_image();
    return static_cast<int>(g_cmt_out.size());
}

EMSCRIPTEN_KEEPALIVE const uint8_t* emu_cmt_data() {
    return g_cmt_out.data();
}

EMSCRIPTEN_KEEPALIVE void emu_cmt_clear_dirty() {
    if (g_machine) g_machine->clear_cmt_dirty();
}

// ---- parallel printer capture ------------------------------------------

EMSCRIPTEN_KEEPALIVE int emu_printer_snapshot() {
    if (!g_machine) return 0;
    g_printer_out = g_machine->printer_output();
    return static_cast<int>(g_printer_out.size());
}

EMSCRIPTEN_KEEPALIVE const uint8_t* emu_printer_data() {
    return g_printer_out.data();
}

EMSCRIPTEN_KEEPALIVE int emu_printer_dirty() {
    return (g_machine && g_machine->printer_dirty()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_printer_clear_dirty() {
    if (g_machine) g_machine->clear_printer_dirty();
}

EMSCRIPTEN_KEEPALIVE void emu_printer_clear_output() {
    if (g_machine) g_machine->clear_printer_output();
    g_printer_out.clear();
}

EMSCRIPTEN_KEEPALIVE void emu_printer_set_online(int on) {
    if (g_machine) g_machine->set_printer_online(on != 0);
}

EMSCRIPTEN_KEEPALIVE int emu_printer_online() {
    return (g_machine && g_machine->printer_online()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_printer_dropped() {
    return g_machine ? static_cast<int>(g_machine->printer_dropped_bytes()) : 0;
}

// ---- MZ-1E30 SASI hard disk --------------------------------------------

EMSCRIPTEN_KEEPALIVE int emu_sasi_insert(const uint8_t* data, int size,
                                         int block_size) {
    return (g_machine && data && size > 0 && block_size >= 0 &&
            g_machine->insert_sasi_image(data, static_cast<size_t>(size),
                                         static_cast<uint32_t>(block_size))) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_create_blank(int size, int block_size) {
    return (g_machine && size > 0 && block_size > 0 &&
            g_machine->create_blank_sasi(static_cast<size_t>(size),
                                         static_cast<uint32_t>(block_size))) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_sasi_eject() {
    if (g_machine) g_machine->eject_sasi();
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_loaded() {
    return (g_machine && g_machine->sasi_loaded()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_block_size() {
    return g_machine ? static_cast<int>(g_machine->sasi_block_size()) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_size() {
    return g_machine ? static_cast<int>(g_machine->sasi_image().size()) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_snapshot() {
    if (!g_machine) return 0;
    g_sasi_out = g_machine->sasi_image();
    return static_cast<int>(g_sasi_out.size());
}

EMSCRIPTEN_KEEPALIVE const uint8_t* emu_sasi_data() {
    return g_sasi_out.data();
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_dirty() {
    return (g_machine && g_machine->sasi_dirty()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_sasi_clear_dirty() {
    if (g_machine) g_machine->clear_sasi_dirty();
}

EMSCRIPTEN_KEEPALIVE void emu_sasi_set_wp(int on) {
    if (g_machine) g_machine->set_sasi_write_protected(on != 0);
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_wp() {
    return (g_machine && g_machine->sasi_write_protected()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_sasi_set_target(int id) {
    if (g_machine) g_machine->set_sasi_target_id(static_cast<uint8_t>(id));
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_target() {
    return g_machine ? g_machine->sasi_target_id() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sasi_phase() {
    return g_machine ? g_machine->sasi_phase() : 0;
}

// Cold boot from the disk in drive 0 (the dummy IPL path)
EMSCRIPTEN_KEEPALIVE int emu_boot() {
    return (g_machine && g_machine->boot_from_disk()) ? 1 : 0;
}

// Front-panel RESET: CPU reset only. Memory mapping and peripherals remain.
EMSCRIPTEN_KEEPALIVE void emu_system_reset() {
    if (g_machine) g_machine->system_reset();
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

// ---- RS-232C / Z80 SIO --------------------------------------------------

EMSCRIPTEN_KEEPALIVE int emu_sio_tx_pop(int channel) {
    if (!g_machine) return -1;
    mz::Z80Sio::TxByte value;
    return g_machine->sio_pop_transmitted(channel, value) ? value.value : -1;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_rx_queue(int channel, int value) {
    return (g_machine && g_machine->sio_queue_receive(
                             channel, static_cast<uint8_t>(value))) ? 1 : 0;
}

// A local loopback byte has already spent one complete character time on
// TxD when it reaches this call. Deliver it to the receiver immediately so
// the browser does not add a second, artificial character-time delay.
EMSCRIPTEN_KEEPALIVE int emu_sio_rx_now(int channel, int value) {
    if (!g_machine || !g_machine->sio_rs232_connected(channel) ||
        !g_machine->sio_receiver_enabled(channel))
        return 0;
    g_machine->sio_receive(channel, static_cast<uint8_t>(value));
    return 1;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_baud(int channel) {
    return g_machine ? static_cast<int>(g_machine->sio_baud(channel)) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_rx_bits(int channel) {
    return g_machine ? g_machine->sio_receive_bits(channel) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_tx_bits(int channel) {
    return g_machine ? g_machine->sio_transmit_bits(channel) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_stop_half_bits(int channel) {
    return g_machine ? g_machine->sio_stop_half_bits(channel) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_parity(int channel) {
    return g_machine ? static_cast<int>(g_machine->sio_parity(channel)) : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_rx_enabled(int channel) {
    return (g_machine && g_machine->sio_receiver_enabled(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_tx_enabled(int channel) {
    return (g_machine && g_machine->sio_transmitter_enabled(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_rs232_connected(int channel) {
    return (g_machine && g_machine->sio_rs232_connected(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_dtr(int channel) {
    return (g_machine && g_machine->sio_dtr(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_rts(int channel) {
    return (g_machine && g_machine->sio_rts(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_sio_break(int channel) {
    return (g_machine && g_machine->sio_break_active(channel)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_sio_set_modem(int channel, int cts, int dcd) {
    if (g_machine)
        g_machine->sio_set_modem_inputs(channel, cts != 0, dcd != 0);
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

EMSCRIPTEN_KEEPALIVE int emu_has_kanji() {
    return (g_machine && g_machine->has_kanji_rom()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_set_boot_mode(int mode) {
    if (g_machine) g_machine->set_boot_mode(mode);
}

EMSCRIPTEN_KEEPALIVE int emu_boot_mode() {
    return g_machine ? g_machine->boot_mode() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_memory_compat_mode() {
    return g_machine ? g_machine->memory_compat_mode() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_display_compat_mode() {
    return g_machine ? g_machine->display_compat_mode() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_frame_cycles() {
    return g_machine ? g_machine->frame_cycles() : 0;
}

// experimental: cold boot through the user-provided IPL ROM
EMSCRIPTEN_KEEPALIVE int emu_boot_real_ipl() {
    return (g_machine && g_machine->boot_with_real_ipl()) ? 1 : 0;
}

// expansion boards: kind 0=exp RAM, 1=exp GRAM, 2=MZ-1M10 palette,
// 3=MZ-1E35, 4=MZ-1R37, 5=MZ-1E30 SASI
EMSCRIPTEN_KEEPALIVE void emu_set_hw_option(int kind, int on) {
    if (g_machine) g_machine->set_hw_option(kind, on != 0);
}

// ---- MZ-1E35 Y8950 host-side connections -------------------------------

EMSCRIPTEN_KEEPALIVE int emu_adpcm_set_ram_size(int size) {
    return (g_machine && size > 0 &&
            g_machine->set_adpcm_ram_size(static_cast<uint32_t>(size))) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_ram_size() {
    return g_machine ? static_cast<int>(g_machine->adpcm_ram_size()) : 0;
}

EMSCRIPTEN_KEEPALIVE void emu_adpcm_set_gpio_inputs(int value) {
    if (g_machine) g_machine->set_adpcm_gpio_inputs(static_cast<uint8_t>(value));
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_gpio_direction() {
    return g_machine ? g_machine->adpcm_gpio_direction() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_gpio_outputs() {
    return g_machine ? g_machine->adpcm_gpio_outputs() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_gpio_pins() {
    return g_machine ? g_machine->adpcm_gpio_pins() : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_adc_enabled() {
    return (g_machine && g_machine->adpcm_adc_enabled()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int emu_adpcm_input_samples(const float* samples, int count,
                                                 int rate) {
    if (!g_machine || !samples || count <= 0 || rate <= 0) return 0;
    return static_cast<int>(g_machine->queue_adpcm_input(
        samples, static_cast<size_t>(count), static_cast<uint32_t>(rate)));
}

EMSCRIPTEN_KEEPALIVE void emu_adpcm_clear_input() {
    if (g_machine) g_machine->clear_adpcm_input();
}

EMSCRIPTEN_KEEPALIVE void emu_adpcm_set_gain(float gain) {
    if (g_machine) g_machine->set_adpcm_mix_gain(gain);
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
