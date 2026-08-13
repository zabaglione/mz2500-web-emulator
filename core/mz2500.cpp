#include "core/mz2500.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mz {

Mz2500::Mz2500() {
    z80_init(&cpu_);
    cpu_.read_byte = cb_read;
    cpu_.write_byte = cb_write;
    cpu_.port_in = cb_in;
    cpu_.port_out = cb_out;
    cpu_.reti = cb_reti;
    cpu_.userdata = this;
    for (int i = 0; i < FdcMb8877::NUM_DRIVES; i++) fdc_.attach(i, &disks_[i]);
    update_boot_sense_inputs();
}

void Mz2500::system_reset() {
    // RESET is not the front-panel IPL button. The CPU clock keeps running
    // and every external latch remains untouched; only the Z80 reset state
    // is established before execution resumes at the currently mapped 0000h.
    const unsigned long machine_cycles = cpu_.cyc;
    z80_init(&cpu_);
    cpu_.read_byte = cb_read;
    cpu_.write_byte = cb_write;
    cpu_.port_in = cb_in;
    cpu_.port_out = cb_out;
    cpu_.reti = cb_reti;
    cpu_.userdata = this;
    cpu_.cyc = machine_cycles;
    cpu_half_cycle_ = 0;
    step_external_wait_ = 0;
    cpu_step_active_ = false;
    real_ipl_ram_init_pending_ = false;
    idle_frames_remaining_ = 0;
}

bool Mz2500::insert_disk(int drive, const std::string& path) {
    return disks_[drive & 1].load_file(path);
}

void Mz2500::set_rom(int kind, const uint8_t* data, size_t size) {
    switch (kind) {
    case 0: mem_.load_ipl_rom(data, size); break;
    case 1: break; // retired CG slot; compatibility text uses MZ-2500 kanji ROM
    case 2: mem_.load_kanji_rom(data, size); break;
    case 3: mem_.load_dict_rom(data, size); break;
    case 4: sasi_.load_bios_rom(data, size); break;
    }
}

void Mz2500::set_boot_mode(int mode) {
    boot_mode_ = std::max(0, std::min(mode, 2));
    update_boot_sense_inputs();
}

int Mz2500::memory_compat_mode() const {
    if ((bank_mode_ & 2) == 0) return 0;
    return (bank_mode_ & 1) ? 1 : 2;
}

int Mz2500::decode_display_compat_mode(uint8_t mode_register) const {
    const int mode = mode_register & 3;
    if (mode == 1 || mode == 2) return mode;
    // The real IPL's no-media path writes 08h: 200-line timing with MOD=00.
    // In that state the rear MODE selector still determines which legacy
    // character output is visible. MOD=11 explicitly forces MZ-2500 mode.
    if (mode == 0 && (mode_register & 0x08) && boot_mode_ != 0)
        return boot_mode_;
    return 0;
}

int Mz2500::display_compat_mode() const {
    return decode_display_compat_mode(crtc_regs_[0x0F]);
}

int Mz2500::effective_display_mode() const {
    return display_compat_mode();
}

int Mz2500::frame_cycles() const {
    return (boot_mode_ != 0 || (crtc_regs_[0x0F] & 0x08))
        ? CYCLES_PER_FRAME_15KHZ : CYCLES_PER_FRAME;
}

int Mz2500::frame_lines() const {
    return frame_cycles() == CYCLES_PER_FRAME_15KHZ
        ? LINES_PER_FRAME_15KHZ : LINES_PER_FRAME;
}

int Mz2500::visible_lines() const {
    return frame_cycles() == CYCLES_PER_FRAME_15KHZ
        ? VBLANK_START_LINE_15KHZ : VBLANK_START_LINE;
}

void Mz2500::update_boot_sense_inputs() {
    // OPN port B is the IPL's view of the rear-panel selectors. Mode lines
    // are active low; legacy boot also selects the 200-line (15 kHz) timing.
    uint8_t pins = 0x3F;
    if (boot_mode_ == 1) pins &= static_cast<uint8_t>(~0x10);
    if (boot_mode_ == 2) pins &= static_cast<uint8_t>(~0x20);
    if (boot_mode_ != 0) pins |= 0x40;
    opn_.set_port_b_input(pins);
    cmt_.set_mz80b_mode(boot_mode_ == 2);
}

bool Mz2500::compat_window(uint16_t addr, int& bank, uint16_t& offset) const {
    const int mode = memory_compat_mode();
    const uint8_t select = pio_a_ & 0xC0;
    if (mode == 1) {
        // MZ-2000: DISP=1/HCLG=0 maps one selected 16KB colour plane at
        // C000h-FFFFh. DISP=1/HCLG=1 exposes the 4KB character RAM at D000h.
        if (select == 0x80 && addr >= 0xC000) {
            const uint16_t rel = static_cast<uint16_t>(addr - 0xC000);
            bank = 0x20 + (compat_vram_control_ & 3) * 2 + (rel >> 13);
            offset = rel & 0x1FFF;
            return true;
        }
        if (select == 0xC0 && addr >= 0xD000 && addr < 0xE000) {
            bank = 0x38;
            offset = static_cast<uint16_t>(addr - 0xD000);
            return true;
        }
    } else if (mode == 2) {
        // MZ-80B has two alternative legacy layouts. HCLG chooses the
        // original low (5000h/6000h) or high (D000h/E000h) VRAM window;
        // F4h-F7h bit0 selects which of the two monochrome graphic pages is
        // CPU-visible.
        const uint16_t text_base = select == 0x80 ? 0xD000 : 0x5000;
        const uint16_t graph_base = select == 0x80 ? 0xE000 : 0x6000;
        if ((select == 0x80 || select == 0xC0) &&
            addr >= text_base && addr < static_cast<uint16_t>(text_base + 0x1000)) {
            bank = 0x38;
            offset = static_cast<uint16_t>(addr - text_base);
            return true;
        }
        if ((select == 0x80 || select == 0xC0) &&
            addr >= graph_base && addr < static_cast<uint32_t>(graph_base) + 0x2000) {
            bank = 0x20 + (compat_vram_control_ & 1) * 2;
            offset = static_cast<uint16_t>(addr - graph_base);
            return true;
        }
    }
    return false;
}

void Mz2500::charge_compat_vram_wait() {
    // The 4 MHz column in the official wait table assigns one wait to the
    // legacy VRAM windows. It is already expressed on the common 6 MHz
    // machine-time axis used by the rest of this core.
    cpu_.cyc++;
    if (cpu_step_active_) step_external_wait_++;
}

uint8_t Mz2500::test_compat_memory_read(uint16_t address) {
    int bank = 0;
    uint16_t offset = 0;
    if (!compat_window(address, bank, offset)) return 0xFF;
    return mem_.bank_present(bank) ? mem_.bank_ptr(bank)[offset] : 0xFF;
}

void Mz2500::test_compat_memory_write(uint16_t address, uint8_t value) {
    int bank = 0;
    uint16_t offset = 0;
    if (compat_window(address, bank, offset) && mem_.bank_present(bank))
        mem_.bank_ptr(bank)[offset] = value;
}

// Is the G-CRTC fetching graphics VRAM on `line`? Only inside its own
// vertical display window (GDEVS/GDEVE, registers 08h-0Bh) and only while
// the mode register has the layer on at all. GDEVS/GDEVE count display
// lines, so a 200-line mode covers two rasters per line - the same mapping
// the frame composer uses.
bool Mz2500::graphics_scanning(int line) const {
    const uint8_t gmode = gde_regs_[0x0E];
    if (!gde_cg_on(gmode)) return false;
    const int gy = gde_v200(gmode) ? (line >> 1) : line;
    const int gdevs = gde_regs_[0x08] | (gde_regs_[0x09] << 8);
    const int gdeve = gde_regs_[0x0A] | (gde_regs_[0x0B] << 8);
    return gy >= gdevs && gy < gdeve;
}

// Is the CRTC fetching the text planes on `line`? Its vertical text window
// is registers 03h/05h, in two-raster lines from TEXT_WIN_LINE0 - the same
// window BASIC's console@ programs, and the same one the frame composer
// reads.
bool Mz2500::text_scanning(int line) const {
    const int y0 = (crtc_regs_[3] - TEXT_WIN_LINE0) * 2;
    const int y1 = (crtc_regs_[5] - TEXT_WIN_LINE0) * 2;
    return in_win(text_vwin_kind(), line, y0, y1);
}

// Which layer a memory block feeds, and whether that layer is being drawn
// on this raster.
//
// The period magazine article behind this model gives the rule as an
// optimisation: restrict the displayed area in y with the hardware viewport
// and the wait comes off the rasters that are not shown, because a raster
// the controller is not displaying is a raster it is not fetching VRAM for.
// Restricting in x buys nothing - the controller still owns the bus for the
// whole line - which is why GDEHS/GDEHE are not consulted here.
//
// The two layers are gated separately, and the article's own sample program
// depends on it: it masks the graphics away completely and then says so in
// as many words - with the graphics fully masked no wait is charged, so the
// program takes its wait off the text screen instead, and warns that
// masking the text with console@ breaks it.
bool Mz2500::layer_scanning(int bank, int line) const {
    if (line >= visible_lines()) return false; // nothing is scanned at all
    if (bank >= 0x20 && bank <= 0x33) return graphics_scanning(line);
    if (bank == 0x38 || bank == 0x39) return text_scanning(line);
    return false;
}

int Mz2500::display_stall_cycles_current() const {
    const int cycles = frame_cycles();
    const int lines = frame_lines();
    // A Z80 instruction may cross the nominal run_frame() boundary before
    // the caller advances frame_origin_. Bus timing still belongs to the
    // new raster in that case, so wrap instead of pinning to the old frame's
    // final HBLANK cycle.
    const int in_frame = cycle_in_frame(cpu_.cyc, frame_origin_, cycles);
    const int line = line_of_cycle_for(in_frame, cycles, lines);
    if (line >= visible_lines()) return 0;
    const int hblank = cycles == CYCLES_PER_FRAME_15KHZ
        ? HBLANK_CYCLES_15KHZ : HBLANK_CYCLES;
    const int display_end = line_start_cycle_for(line + 1, cycles, lines) - hblank;
    return in_frame < display_end ? display_end - in_frame : 0;
}

// The weight the port B5h bank table charges for touching this block, plus
// the stall to the blanking period if the controller has the bus. Both are
// paid before the access completes, so the byte lands at the end of the
// wait, which is where the bus puts it.
void Mz2500::charge_access_wait(int bank) {
    // At 4 MHz, direct GRAM/text/PCG mappings carry no extra wait; only the
    // read-modify-write window keeps one. The compatibility overlay handled
    // by compat_window() is a separate path and always carries one wait.
    const int weight = boot_mode_ != 0
        ? ((bank >= 0x30 && bank <= 0x33) ? 1 : 0)
        : bank_access_wait(bank);
    if (!weight) return;
    const int stall = boot_mode_ == 0 && layer_scanning(bank, current_line())
        ? display_stall_cycles_current() : 0;
    const uint64_t charged = static_cast<uint64_t>(stall + weight);
    cpu_.cyc += static_cast<unsigned long>(charged);
    if (cpu_step_active_) step_external_wait_ += charged;
    if (stall_counters_) {
        stall_counters_->hits[insn_pc_]++;
        stall_counters_->stall[insn_pc_] += (uint64_t)stall;
        stall_counters_->weight[insn_pc_] += (uint64_t)weight;
    }
}

void Mz2500::enable_stall_profile(bool on) {
    if (on == (bool)stall_counters_) return;
    stall_counters_ = on ? std::make_unique<StallCounters>() : nullptr;
    stall_origin_frame_ = frames_;
}

void Mz2500::reset_stall_profile() {
    if (stall_counters_) *stall_counters_ = StallCounters{};
    stall_origin_frame_ = frames_;
}

std::vector<Mz2500::StallSite> Mz2500::stall_sites() const {
    std::vector<StallSite> out;
    if (!stall_counters_) return out;
    for (int pc = 0; pc < 0x10000; pc++) {
        if (!stall_counters_->hits[pc]) continue;
        out.push_back(StallSite{(uint16_t)pc, stall_counters_->hits[pc],
                                stall_counters_->stall[pc], stall_counters_->weight[pc]});
    }
    std::sort(out.begin(), out.end(), [](const StallSite& a, const StallSite& b) {
        if (a.stall != b.stall) return a.stall > b.stall;
        return a.hits > b.hits;
    });
    return out;
}

uint8_t Mz2500::cb_read(void* ud, uint16_t addr) {
    auto* m = static_cast<Mz2500*>(ud);
    int compat_bank = 0;
    uint16_t compat_offset = 0;
    if (m->compat_window(addr, compat_bank, compat_offset)) {
        m->charge_compat_vram_wait();
        return m->mem_.bank_present(compat_bank)
            ? m->mem_.bank_ptr(compat_bank)[compat_offset] : 0xFF;
    }
    const int bank = m->mem_.map_of(addr >> 13);
    m->charge_access_wait(bank);
    if (bank >= 0x20 && bank <= 0x33 && m->cpu_.cyc < m->gde_busy_until_)
        return 0xFF;
    if (bank >= 0x30 && bank <= 0x33)
        return m->gvram_rmw_read(bank, addr & 0x1FFF);
    return m->mem_.read(addr);
}
void Mz2500::cb_write(void* ud, uint16_t addr, uint8_t value) {
    auto* m = static_cast<Mz2500*>(ud);
    int compat_bank = 0;
    uint16_t compat_offset = 0;
    if (m->compat_window(addr, compat_bank, compat_offset)) {
        m->charge_compat_vram_wait();
        if (m->mem_.bank_present(compat_bank))
            m->mem_.bank_ptr(compat_bank)[compat_offset] = value;
        return;
    }
    const int bank = m->mem_.map_of(addr >> 13);
    m->charge_access_wait(bank);
    if (bank >= 0x20 && bank <= 0x33 && m->cpu_.cyc < m->gde_busy_until_)
        return;
    if (bank >= 0x30 && bank <= 0x33) {
        m->gvram_rmw_write(bank, addr & 0x1FFF, value);
        return;
    }
    m->mem_.write(addr, value);
}

// GVRAM read-modify-write windows (banks 30h/31h = standard screen,
// 32h/33h = extended screen; each pair is the 16KB linear plane space).
// A CPU write lands on every plane selected by the function register,
// combined with the pattern/colour/bit-mask registers (GDE regs 00h-06h,
// I/O map "Graphic controller internal register").
void Mz2500::gvram_rmw_write(int bank, uint16_t off, uint8_t value) {
    const uint32_t lin = ((uint32_t)(bank & 1) << 13) | off;
    const int base = (bank >= 0x32) ? 0x28 : 0x20;
    const uint8_t fn = gde_regs_[5];
    const uint8_t planes = fn & 0x0F;       // plane select (I G R B)
    const uint8_t colour = gde_regs_[4] & 0x0F;
    const uint8_t mask = gde_regs_[6];
    for (int p = 0; p < 4; p++) {
        if (!(planes & (1 << p))) continue;
        const int dst_bank = base + p * 2 + (int)(lin >> 13);
        if (!mem_.bank_present(dst_bank)) continue;
        uint8_t* dst = mem_.bank_ptr(dst_bank) + (lin & 0x1FFF);
        const uint8_t pat = gde_regs_[p];
        uint8_t d = *dst;
        switch ((fn >> 6) & 3) {
        case 0: // REPLACE: punch the mask, fit the patterned data
            d = (uint8_t)((d & ~mask) |
                          ((colour & (1 << p)) ? (value & pat & mask) : 0));
            break;
        case 1: // PSET: set/reset the written pixels to the requested colour
            d = (uint8_t)((d & ~value) |
                          ((colour & (1 << p)) ? (value & pat) : 0));
            break;
        default: // 10 is the screen-clear command; 11 is not a drawing mode
            break;
        }
        *dst = d;
    }
}

uint8_t Mz2500::gvram_rmw_read(int bank, uint16_t off) {
    const uint32_t lin = ((uint32_t)(bank & 1) << 13) | off;
    const int base = (bank >= 0x32) ? 0x28 : 0x20;
    const uint8_t sel = gde_regs_[7];
    auto plane_byte = [&](int p) {
        const int src_bank = base + p * 2 + (int)(lin >> 13);
        return mem_.bank_present(src_bank)
            ? mem_.bank_ptr(src_bank)[lin & 0x1FFF]
            : (uint8_t)0xFF;
    };
    // Whatever the window returns, the four planes' raw bytes latch into
    // ports BCh-BFh, which is how a paint routine gets at the individual
    // planes after one read (I/O map, "the raw data at that point is
    // latched into BCh-BFh").
    for (int p = 0; p < 4; p++) gvram_latch_[p] = plane_byte(p);
    if (!(sel & 0x10)) return plane_byte(sel & 3); // direct read of one plane
    // select read: a bit is 1 where the pixel colour matches I/G/R/B exactly
    uint8_t out = 0xFF;
    for (int p = 0; p < 4; p++) {
        const uint8_t b = plane_byte(p);
        out &= (sel & (1 << p)) ? b : (uint8_t)~b;
    }
    return out;
}

void Mz2500::clear_gvram_window() {
    const uint8_t mode = gde_regs_[0x0E];
    const uint8_t planes = gde_regs_[0x05] & 0x0F;
    const uint8_t mask = gde_regs_[0x06];
    const bool h640 = (mode & 0x02) != 0;
    const bool v200 = gde_v200(mode);
    const int stride = h640 ? 80 : 40;
    const int max_lines = v200 ? 200 : 400;
    const int first = std::min<int>(gde_regs_[0x08] | (gde_regs_[0x09] << 8),
                                    max_lines);
    const int last = std::min<int>(gde_regs_[0x0A] | (gde_regs_[0x0B] << 8),
                                   max_lines);
    const uint32_t sad0 = gde_regs_[0x10] | (gde_regs_[0x11] << 8);
    const uint32_t sad1 = gde_regs_[0x12] | (gde_regs_[0x13] << 8);
    const uint32_t sad2 = gde_regs_[0x14] | (gde_regs_[0x15] << 8);
    const uint32_t sln1 = gde_regs_[0x16] | (gde_regs_[0x17] << 8);
    const uint32_t ring = sad1 + 1u;

    if (first < last && planes != 0) {
        for (int y = first; y < last; y++) {
            const uint32_t row = gde_row_address(
                (uint32_t)y, (uint32_t)stride, sad0, sad1, sad2, sln1);
            for (int x = 0; x < stride; x++) {
                const uint32_t address = (row + (uint32_t)x) % ring;
                for (int plane = 0; plane < 4; plane++) {
                    if (!(planes & (1 << plane))) continue;
                    const int bank = gde_plane_bank(plane, address, mode);
                    if (mem_.bank_present(bank))
                        mem_.bank_ptr(bank)[address & 0x1FFF] &=
                            (uint8_t)~mask;
                }
            }
        }
    }

    // The period source specifies a maximum of 32 ms for screen clear but
    // gives no per-byte timing. Scale linearly with the address count so a
    // full 640x400 window reaches that documented upper bound (192,000 CPU
    // cycles at 6 MHz); the four planes are operated in parallel.
    const uint64_t addresses =
        (uint64_t)std::max(0, last - first) * (uint64_t)stride;
    gde_busy_until_ = cpu_.cyc + std::max<uint64_t>(6, addresses * 6);
}

uint8_t Mz2500::cb_in(z80* z, uint16_t port) {
    auto* m = static_cast<Mz2500*>(z->userdata);
    const uint8_t value = m->io_in(port);
    m->io_ring_[m->io_ring_pos_] = {m->cpu_.cyc, port, value, false};
    m->io_ring_pos_ = (m->io_ring_pos_ + 1) % IO_RING;
    return value;
}
void Mz2500::cb_out(z80* z, uint16_t port, uint8_t value) {
    auto* m = static_cast<Mz2500*>(z->userdata);
    m->io_ring_[m->io_ring_pos_] = {m->cpu_.cyc, port, value, true};
    m->io_ring_pos_ = (m->io_ring_pos_ + 1) % IO_RING;
    m->io_out(port, value);
}

uint8_t Mz2500::blank_flags() const {
    const int cycles = frame_cycles();
    const int lines = frame_lines();
    const int in_frame = cycle_in_frame(cpu_.cyc, frame_origin_, cycles);
    const int line = line_of_cycle_for(in_frame, cycles, lines);
    uint8_t flags = 0;
    if (line >= visible_lines()) flags |= 0x01;
    const int hblank = cycles == CYCLES_PER_FRAME_15KHZ
        ? HBLANK_CYCLES_15KHZ : HBLANK_CYCLES;
    if (in_frame >= line_start_cycle_for(line + 1, cycles, lines) - hblank)
        flags |= 0x02;
    return flags;
}

int Mz2500::frame_line_start(int line) const {
    return line_start_cycle_for(line, frame_cycles(), frame_lines());
}

// Which raster of the current frame the CPU is on. Same frame origin as
// blank_flags(), so the two never disagree about where vertical blanking is.
int Mz2500::current_line() const {
    const int cycles = frame_cycles();
    const int lines = frame_lines();
    const int in_frame = cycle_in_frame(cpu_.cyc, frame_origin_, cycles);
    const int line = line_of_cycle_for(in_frame, cycles, lines);
    return line < lines ? line : lines - 1;
}

int Mz2500::raster_write_start_line() const {
    // render() still composes the frame whose origin is frame_origin_. An
    // instruction that overruns its end has already entered the following
    // frame, so its writes must not repaint the completed frame. The next
    // seed_raster_lines() call will pick up the resulting register state.
    if (cpu_.cyc >= frame_origin_ + static_cast<uint64_t>(frame_cycles()))
        return VBLANK_START_LINE;
    const int scale = frame_cycles() == CYCLES_PER_FRAME_15KHZ ? 2 : 1;
    return std::min(current_line() * scale, VBLANK_START_LINE);
}

// Start of frame: every visible line inherits the complete display-facing
// state. I/O writes overwrite only the current and later lines below.
void Mz2500::seed_raster_lines() {
    for (int line = 0; line < VBLANK_START_LINE; line++) {
        RasterLineState& state = raster_line_[line];
        std::memcpy(state.crtc, crtc_regs_, sizeof(state.crtc));
        std::memcpy(state.gde, gde_regs_, sizeof(state.gde));
        std::memcpy(state.palette, palette_, sizeof(state.palette));
        state.cg_mask = cg_mask_;
        state.font_size = font_size_;
        state.pio_a = pio_a_;
        state.ppi_a = ppi_[0];
        state.ppi_control = ppi_control_;
        state.ppi_c = ppi_[2];
        state.opn_port_a = opn_.port_a_pins();
        state.compat_vram_control = compat_vram_control_;
        state.compat_background = compat_background_;
        state.compat_text_colour = compat_text_colour_;
        state.compat_graphics_mask = compat_graphics_mask_;
        state.crtc_vwin_written = crtc_vwin_written_;
        state.crtc_hwin_written = crtc_hwin_written_;
        state.palette_written = palette_written_;
        state.opn_port_a_output = opn_.port_a_is_output();
    }
}

void Mz2500::log_port_once(uint16_t port, const char* dir) {
    const uint8_t low = port & 0xFF;
    bool* table = dir[0] == 'i' ? warned_in_ : warned_out_;
    if (!table[low]) {
        table[low] = true;
        std::fprintf(stderr, "[io] unimplemented %s port %02Xh (bus %04Xh)\n", dir, low, port);
    }
}

static bool traced_port(uint8_t p) {
    // MZ_TRACE_ALL=1 widens --trace-io to every port, for hunting
    // down which one a driver is really talking to.
    static const bool all = std::getenv("MZ_TRACE_ALL") != nullptr;
    if (all) return true;
    return (p >= 0xD8 && p <= 0xDF) || (p >= 0xE0 && p <= 0xE3) ||
           (p >= 0xA0 && p <= 0xA3) || (p >= 0xB0 && p <= 0xB3) || p == 0xCD ||
           (p >= 0xA4 && p <= 0xA5) || (p >= 0xA8 && p <= 0xA9) ||
           p == 0xAE || p == 0xB4 || p == 0xB5 || p == 0xB7 || p == 0xCA ||
           p == 0xF4 || p == 0xF5 || p == 0xF6 || p == 0xC6 || p == 0xC7 || p == 0xCC ||
           p == 0xFE || p == 0xFF ||
           (p >= 0xE4 && p <= 0xE7) || p == 0xEF || p == 0xF0 || p == 0xCF ||
           p == 0xBC || p == 0xBD || p == 0xF7;
}

void Mz2500::sio_receive(int channel, uint8_t value) {
    if (!sio_rs232_connected(channel)) return;
    sio_.receive_byte(channel, value, Z80Sio::RX_OK, cpu_.cyc);
}

bool Mz2500::sio_queue_receive(int channel, uint8_t value) {
    if (!sio_rs232_connected(channel)) return false;
    return sio_.queue_receive_byte(channel, value, Z80Sio::RX_OK, cpu_.cyc);
}

void Mz2500::sio_write_control(int ch, uint8_t value) {
    const bool old_dtr = sio_.dtr(ch);
    sio_.write_control(ch, value, cpu_.cyc);
    const bool new_dtr = sio_.dtr(ch);
    if (old_dtr != new_dtr) sio_dtr_changed(ch, new_dtr);
}

bool Mz2500::decode_sio_port(uint8_t port, int& channel, bool& control) const {
    const uint8_t base = (sio_clock_control_ & 0x80) ? 0xB0 : 0xA0;
    if (port < base || port > base + 3) return false;
    const uint8_t offset = port - base;
    channel = (offset >> 1) & 1;
    control = (offset & 1) != 0;
    return true;
}

void Mz2500::write_sio_clock_control(uint8_t value) {
    // I/O map and real-IPL order agree that bits5-3 select channel A and
    // bits2-0 channel B. The book's printed A/B headings are reversed.
    static constexpr uint32_t clocks[8] = {
        307'200, 153'600, 76'800, 38'400,
         19'200,   9'600,  4'800,  2'400,
    };
    sio_clock_control_ = value;
    sio_.set_clock_hz(0, clocks[(value >> 3) & 7]);
    sio_.set_clock_hz(1, clocks[value & 7]);
}

void Mz2500::test_set_sio_dtr(int channel, bool high) {
    const int ch = channel & 1;
    sio_write_control(ch, 5);
    sio_write_control(ch, high ? 0x80 : 0x00);
}

void Mz2500::test_enable_sio_receiver(int channel) {
    const int ch = channel & 1;
    sio_write_control(ch, 3);
    sio_write_control(ch, 0xC1);
}

// A rising edge on DTR is the mouse's strobe: one three-byte packet goes
// into the receiver, exactly as the measured protocol describes.
void Mz2500::sio_dtr_changed(int ch, bool dtr) {
    const bool rising = dtr && !sio_dtr_[ch];
    sio_dtr_[ch] = dtr;
    if (!rising || ch != 1 || !mouse_connected()) return;
    uint8_t packet[3];
    mouse_.take_packet(packet);
    for (uint8_t b : packet) sio_.receive_byte(1, b, Z80Sio::RX_OK, cpu_.cyc);
}

uint8_t Mz2500::io_in(uint16_t port) {
    if (trace_io_ && traced_port(port & 0xFF)) {
        const uint8_t v = io_in_raw(port);
        std::fprintf(stderr, "[iot] %llu f%llu pc=%04X IN  %02X -> %02X\n",
                     (unsigned long long)cpu_.cyc, (unsigned long long)frames_,
                     cpu_.pc, port & 0xFF, v);
        return v;
    }
    return io_in_raw(port);
}

uint8_t Mz2500::io_in_raw(uint16_t port) {
    switch (port & 0xFF) {
    case 0xB4: return mem_.in_b4();
    case 0xB5: return mem_.in_b5();
    // BCh-BFh hand back the four graphics planes' raw bytes latched by the
    // last read-modify-write window access - plane 0 (blue) through plane 3
    // (intensity). BDh doubles as the status byte when register 07h bit4
    // selects the search read: bit7 = vertical sync, bit0 = clear in
    // progress.
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: {
        int ch = 0;
        bool control = false;
        if (!decode_sio_port(port & 0xFF, ch, control)) return 0xFF;
        return control ? sio_.read_control(ch, cpu_.cyc)
                       : sio_.read_data(ch, cpu_.cyc);
    }
    case 0xA4:
        if (sasi_present_) return sasi_.read_data();
        log_port_once(port, "in");
        return 0xFF;
    case 0xA5:
        if (sasi_present_) return sasi_.read_status();
        log_port_once(port, "in");
        return 0xFF;
    case 0xA9:
        if (sasi_present_) return sasi_.read_bios((port >> 8) & 0xFF);
        log_port_once(port, "in");
        return 0xFF;
    case 0xBC: return gvram_latch_[0];
    case 0xBD:
        if (gde_regs_[7] & 0x10)
            return (uint8_t)((cpu_.cyc < gde_busy_until_ ? 0x01 : 0x00) |
                             (blank_flags() & 0x01 ? 0x00 : 0x80));
        return gvram_latch_[1];
    case 0xBE: return gvram_latch_[2];
    case 0xBF: return gvram_latch_[3];
    case 0xC8: return opn_.read_status(cpu_.cyc);
    case 0xC9: {
        const uint8_t v = opn_.read_data();
        if (trace_boot_)
            std::fprintf(stderr, "[opn] read C9 addr=%02X -> %02X (pc=%04X)\n",
                         opn_addr_, v, cpu_.pc);
        return v;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        return static_cast<uint8_t>(~fdc_.read((port & 0xFF) - 0xD8, cpu_.cyc));
    case 0xCA:
        // telephone unit status (MZ-1X09 class option, not installed):
        // bit7 = POWER "woken by the phone line". The IPL branches to the
        // communication-ROM boot when it reads 1 here, so an absent unit
        // must read 0 or every boot detours away from the floppy.
        return 0x00;
    case 0xCC:
        // RP5C15 RTC: bus address bits A11-A8 select the register (16-bit
        // I/O decode, like the palette port). Registers are 4 bits wide.
        return rtc_read((port >> 8) & 0x0F);
    case 0x98:
        if (adpcm_present_) return adpcm_.read_status(cpu_.cyc);
        log_port_once(port, "in");
        return 0xFF;
    case 0x99:
        if (adpcm_present_) return adpcm_.read_data(cpu_.cyc);
        log_port_once(port, "in");
        return 0xFF;
    case 0xAD:
        // MZ-1R37 EMM data window: bus A15-A8 supplies address bits 7:0
        if (emm_present_) return emm_.read((port >> 8) & 0xFF);
        log_port_once(port, "in");
        return 0xFF;
    case 0xAF:
        // TV tuner control board (MZ-1T01 class option, not installed):
        // bit0 = TVPOWER input. An absent tuner reads 0, the same trap as
        // the telephone unit on CAh - open bus would say "tuner powered".
        return 0x00;
    case 0xE0:
        // 8255 port A controls the cassette deck. The native 82h mode makes
        // it an output, whose readback is the output latch. On RESET all
        // ports are inputs; no input source is wired here, so the pins are
        // pulled high. Keyboard rows are on the Z80 PIO at E8h/EAh.
        return ppi_port_a_output() ? ppi_[0] : 0xFF;
    case 0xE1: {
        if (ppi_port_b_output()) return ppi_[1];
        // Port B is the data-recorder sensor input in native mode:
        // READ/TREADY/WREADY/TEND are bits 6..3. The latter three are
        // active low; an empty deck therefore retains the old B8h result.
        uint8_t v = 0x80;
        if (cmt_.read_data(cpu_.cyc)) v |= 0x40;
        if (!cmt_.tape_loaded()) v |= 0x20;
        if (!cmt_.tape_loaded() || cmt_.write_protected()) v |= 0x10;
        if (!cmt_.running(cpu_.cyc)) v |= 0x08;
        if (key_rows_[3] & 0x80) v &= (uint8_t)~0x80;
        if (!(blank_flags() & 0x01)) v |= 0x01;
        return v;
    }
    case 0xE2: {
        // Mixed-direction port C: output bits read their latches; input bits
        // see pulled-high pins. Mode 1/2 handshake inputs are not used by
        // the MZ-2500's normal 82h configuration.
        uint8_t v = 0xFF;
        for (int bit = 0; bit < 8; bit++) {
            if (!ppi_port_c_output(bit)) continue;
            v = (uint8_t)((v & ~(1 << bit)) | (ppi_[2] & (1 << bit)));
        }
        return v;
    }
    case 0xE4: case 0xE5: case 0xE6:
        return pit_read_counter((port & 0xFF) - 0xE4);
    case 0xE8: return pio_a_;
    case 0xEA: {
        // PIO port B = key columns. E8h STB[4]=1 reads the row addressed by
        // STB[3:0]; STB[4]=0 reads the AND of every column (any-key sense)
        if (pio_a_ & 0x10)
            return static_cast<uint8_t>(~key_rows_[pio_a_ & 0x0F]);
        uint8_t any = 0;
        for (int r = 0; r < 16; r++) any |= key_rows_[r];
        return static_cast<uint8_t>(~any);
    }
    case 0xEF: return static_cast<uint8_t>(~joy_mask_);
    case 0xFE: return printer_.read_control(cpu_.cyc);
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        // All four ports return the same H/V display-period pins in native
        // mode. Their read value is explicitly undefined after the
        // character controller enters either compatibility mode.
        if (display_compat_mode() != 0) return 0xFF;
        return static_cast<uint8_t>(~blank_flags() & 0x03);
    default:
        log_port_once(port, "in");
        return 0xFF;
    }
}

void Mz2500::io_out(uint16_t port, uint8_t value) {
    if (trace_io_ && traced_port(port & 0xFF))
        std::fprintf(stderr, "[iot] %llu f%llu pc=%04X OUT %02X <- %02X\n",
                     (unsigned long long)cpu_.cyc, (unsigned long long)frames_,
                     cpu_.pc, port & 0xFF, value);
    switch (port & 0xFF) {
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: {
        int ch = 0;
        bool control = false;
        if (!decode_sio_port(port & 0xFF, ch, control)) return;
        if (control) sio_write_control(ch, value);
        else {
            const uint64_t wait = sio_.write_data(ch, value, cpu_.cyc);
            cpu_.cyc += static_cast<unsigned long>(wait);
            if (cpu_step_active_) step_external_wait_ += wait;
        }
        return;
    }
    case 0x98:
        if (adpcm_present_) adpcm_.write_address(value, cpu_.cyc);
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xA4:
        if (sasi_present_) sasi_.write_data(value);
        else log_port_once(port, "out");
        return;
    case 0xA5:
        if (sasi_present_) sasi_.write_selection(value);
        else log_port_once(port, "out");
        return;
    case 0xA8:
        if (sasi_present_) sasi_.write_bios_latch((port >> 8) & 0xFF, value);
        else log_port_once(port, "out");
        return;
    case 0x99:
        if (adpcm_present_) adpcm_.write_data(value, cpu_.cyc);
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xAC:
        // MZ-1R37 EMM address latch: bits 19:16 ride the bus's high byte,
        // bits 15:8 are the written value. No auto-increment.
        if (emm_present_) emm_.latch((port >> 8) & 0xFF, value);
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xAD:
        if (emm_present_) emm_.write((port >> 8) & 0xFF, value);
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xAE:
        if (mz1m10_present_) {
            const int entry = (port >> 8) & 0x1F;
            palette_[entry] = value;
            palette_written_ = true;
            for (int line = raster_write_start_line();
                 line < VBLANK_START_LINE; line++) {
                raster_line_[line].palette[entry] = value;
                raster_line_[line].palette_written = true;
            }
        }
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xB4: mem_.out_b4(value); return;
    case 0xB5: mem_.out_b5(value); return;
    case 0xB7:
        // B7h is a two-bit memory-mode latch: 0/1 are the two native IPL/run
        // states, 2 selects MZ-80B and 3 selects MZ-2000. Entering either
        // compatibility state fixes all eight CPU blocks to RAM 00h-07h.
        // The display mode is a separate latch in CRTC register 0Fh.
        if ((bank_mode_ & 2) == 0 && (value & 2)) {
            for (int block = 0; block < 8; block++)
                mem_.set_map(block, static_cast<uint8_t>(block));
        } else if ((value & 3) == 1) {
            // Native run state drops the reset overlay while keeping the
            // upper half of the IPL visible at 4000h-7FFFh.
            static const uint8_t run_map[4] = {0x00, 0x01, 0x36, 0x37};
            for (int block = 0; block < 4; block++) {
                if (mem_.map_of(block) == (uint8_t)(0x34 + block))
                    mem_.set_map(block, run_map[block]);
            }
        }
        bank_mode_ = value & 3;
        return;
    case 0xBC:
        // G-CRTC register select. Bit7 = auto-increment: each following BDh
        // write bumps the low two bits of the register number, so a driver
        // can pour the four plane pattern registers (00h-03h), or the colour
        // and function pair (04h, 05h), through the port back to back.
        gde_index_ = value & 0x7F;
        gde_autoinc_ = (value & 0x80) != 0;
        return;
    case 0xBD: {
        const uint8_t reg = gde_index_;
        if (gde_autoinc_)
            gde_index_ = (uint8_t)((gde_index_ & ~3) | ((gde_index_ + 1) & 3));
        if (reg > 0x1F) return;
        gde_regs_[reg] = value;
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++)
            raster_line_[line].gde[reg] = value;
        if (reg == 0x05 && ((value >> 6) & 3) == 2) clear_gvram_window();
        return;
    }
    case 0xCA:
        // telephone unit control (TOFF power-off pulse). Not a sound source:
        // with no phone unit installed the write does nothing. (The BEEP
        // speaker is 8255 port C bit2 / SOUND on ports E2h/E3h below.)
        return;
    case 0xC6:
        int_select_ = value;
        for (int src = 0; src < 4; src++) {
            if (!(value & (1 << src))) int_pending_[src] = false;
        }
        return;
    case 0xC7:
        for (int src = 0; src < 4; src++) {
            if (int_select_ & (0x10 << src)) int_vectors_[src] = value;
        }
        return;
    case 0xC8:
        opn_addr_ = value;
        opn_.write_address(value, cpu_.cyc);
        return;
    case 0xC9:
        opn_regs_[opn_addr_] = value; // raw register shadow for debug tooling
        // Reg 28h is a command, not a latch: bits 1-0 pick the FM channel
        // and bits 7-4 carry the slot key-on mask, so the shadow alone
        // cannot answer "which channels are sounding". Keep that here.
        if (opn_addr_ == 0x28 && (value & 3) != 3) fm_keyon_[value & 3] = value >> 4;
        opn_.write_data(value, cpu_.cyc);
        if (opn_addr_ == 0x07 || opn_addr_ == 0x0E) {
            for (int line = raster_write_start_line();
                 line < VBLANK_START_LINE; line++) {
                raster_line_[line].opn_port_a = opn_.port_a_pins();
                raster_line_[line].opn_port_a_output = opn_.port_a_is_output();
            }
        }
        return;
    case 0xCC:
        rtc_write((port >> 8) & 0x0F, value & 0x0F);
        return;
    case 0xCD:
        write_sio_clock_control(value);
        return;
    case 0xCE: mem_.set_dict_bank(value); return;
    case 0xCF: mem_.set_kanji_bank(value); return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        fdc_.write((port & 0xFF) - 0xD8, static_cast<uint8_t>(~value), cpu_.cyc);
        return;
    case 0xDC: fdc_.write_drive(value, cpu_.cyc); return;
    case 0xDD: fdc_.write_side(value); return;
    case 0xDE:
        fdc_.set_single_density((value & 0x01) != 0);
        return;
    case 0xE0:
        ppi_[0] = value;
        update_ppi_outputs();
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++)
            raster_line_[line].ppi_a = ppi_[0];
        return;
    case 0xE1:
        // MIC/REC1 control the analogue audio track. Keep the latch for
        // readback; the browser CMT device currently exposes the data track.
        ppi_[1] = value;
        return;
    case 0xE2:
        // 8255 port C: bit2 = SOUND drives the BEEP speaker (BASIC's startup
        // pip toggles it at audio rate)
        ppi_[2] = value;
        update_ppi_outputs();
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++)
            raster_line_[line].ppi_c = ppi_[2];
        return;
    case 0xE3:
        ppi_write_control(value);
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++) {
            raster_line_[line].ppi_a = ppi_[0];
            raster_line_[line].ppi_control = ppi_control_;
            raster_line_[line].ppi_c = ppi_[2];
        }
        return;
    case 0xE4: case 0xE5: case 0xE6:
        pit_write_counter((port & 0xFF) - 0xE4, value);
        return;
    case 0xE7: pit_write_control(value); return;
    case 0xE8:
        pio_a_ = value;
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++)
            raster_line_[line].pio_a = value;
        return;
    case 0xE9: case 0xEB:
        pio_ctrl_[((port & 0xFF) - 0xE9) >> 1] = value; // PIO mode words: latch
        return;
    case 0xEF: joy_enable_ = value; return;
    case 0xF0: case 0xF1: case 0xF2: case 0xF3:
        pit_gate_strobe();
        return;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: {
        const uint8_t p = port & 0xFF;
        const int memory_mode = memory_compat_mode();
        const int display_mode = display_compat_mode();
        const int first_line = raster_write_start_line();

        auto sync_compat_rasters = [&]() {
            for (int line = first_line; line < VBLANK_START_LINE; line++) {
                raster_line_[line].compat_vram_control = compat_vram_control_;
                raster_line_[line].compat_background = compat_background_;
                raster_line_[line].compat_text_colour = compat_text_colour_;
                raster_line_[line].compat_graphics_mask = compat_graphics_mask_;
            }
        };

        // The memory-controller half of the shared decode remains separate
        // from the character controller. This also models the short period
        // during IPL where firmware has switched one latch but not the other.
        if (memory_mode == 1 && p == 0xF7) {
            compat_vram_control_ = static_cast<uint8_t>(
                (compat_vram_control_ & ~3) | (value & 3));
        } else if (memory_mode == 2) {
            compat_vram_control_ = static_cast<uint8_t>(
                (compat_vram_control_ & ~1) | (value & 1));
        }

        if (display_mode == 1) {
            // MZ-2000: background, text colour/priority and graphic output
            // mask occupy F4h, F5h and F6h respectively. F7h belongs only
            // to the memory controller above.
            if (p == 0xF4) compat_background_ = value & 7;
            else if (p == 0xF5) compat_text_colour_ = value & 0x0F;
            else if (p == 0xF6) compat_graphics_mask_ = value & 0x0F;
            sync_compat_rasters();
            return;
        }
        if (display_mode == 2) {
            // In MZ-80B mode every one of F4h-F7h drives the same three
            // lines: access page on D0 and display enables on D1/D2.
            compat_vram_control_ = static_cast<uint8_t>(
                (compat_vram_control_ & ~6) | (value & 6));
            sync_compat_rasters();
            return;
        }

        // The shared memory-controller decode above can change a legacy
        // page latch before the display controller enters compatibility
        // mode. Preserve that state for any later raster-mode transition.
        sync_compat_rasters();

        // Native character-controller decode.
        if (p == 0xF4) {
            crtc_index_ = value;
            return;
        }
        if (p == 0xF5) {
            // Capture the old timing before register 0Fh can switch the
            // controller to its 15 kHz clock and change current_line().
            const int crtc_first_line = first_line;
            crtc_regs_[crtc_index_] = value;
            // Remember that a program has taken charge of a text display
            // window. A programmed-shut window and an untouched reset
            // register pair otherwise have the same numeric contents.
            if (crtc_index_ == 0x03 || crtc_index_ == 0x05)
                crtc_vwin_written_ = true;
            if (crtc_index_ == 0x07 || crtc_index_ == 0x08)
                crtc_hwin_written_ = true;
            for (int line = crtc_first_line;
                 line < VBLANK_START_LINE; line++) {
                raster_line_[line].crtc[crtc_index_] = value;
                raster_line_[line].crtc_vwin_written = crtc_vwin_written_;
                raster_line_[line].crtc_hwin_written = crtc_hwin_written_;
            }
            return;
        }
        if (p == 0xF6) {
            cg_mask_ = value;
            for (int line = raster_write_start_line();
                 line < VBLANK_START_LINE; line++)
                raster_line_[line].cg_mask = value;
            return;
        }
        font_size_ = value;
        for (int line = raster_write_start_line();
             line < VBLANK_START_LINE; line++)
            raster_line_[line].font_size = value;
        return;
    }
    case 0xFE:
        printer_.write_control(value, cpu_.cyc);
        return;
    case 0xFF:
        printer_.write_data(value);
        return;
    default:
        log_port_once(port, "out");
        return;
    }
}

uint16_t Mz2500::pit_current(int ch) const {
    const PitChannel& c = pit_[ch];
    if (!c.counting) return c.terminal ? 0 : (uint16_t)c.reload;
    // The three channels are chained (I/O map): CLK0 = 31.25kHz, CLK1 = OUT0,
    // CLK2 = OUT1. So each channel counts at the rate its predecessor
    // overflows - BASIC-M25 reads ch2 as its wall clock and only does timer
    // work when that reading changes, which is what paces the cursor blink.
    uint64_t divisor = PIT_CLOCK_DIV;
    for (int up = 0; up < ch; up++) divisor *= (uint64_t)pit_[up].count();
    const uint64_t ticks = (cpu_.cyc - c.start_cyc) / divisor;
    // A freshly written counter is not loaded into the counting element
    // until the next clock edge; until then the read returns the reload
    // value itself. The IPL depends on this: it writes ch2 = 0 and spins
    // until the reading is zero, which ends on the very first read.
    if (ticks == 0) return (uint16_t)c.reload;
    int mode = (c.control >> 1) & 7;
    if (mode >= 6) mode -= 4;
    if (mode != 2 && mode != 3 && ticks >= c.count()) return 0;
    return (uint16_t)(c.count() - 1 - ((ticks - 1) % c.count()));
}

void Mz2500::pit_start_counter(int ch) {
    PitChannel& c = pit_[ch];
    if (!c.loaded) return;
    c.counting = true;
    c.terminal = false;
    c.start_cyc = cpu_.cyc;
    if (ch == 0) {
        pit_counting_ = true;
        pit_next_fire_ = cpu_.cyc + (uint64_t)c.count() * PIT_CLOCK_DIV;
    }
}

void Mz2500::pit_gate_strobe() {
    // Any write to F0h-F3h produces one low pulse on GATE0 and GATE1. A
    // rising edge retriggers modes 1/5 and restarts the rate/square-wave
    // generators (2/3); a zero-time pulse merely pauses modes 0/4.
    for (int ch = 0; ch < 2; ch++) {
        int mode = (pit_[ch].control >> 1) & 7;
        if (mode >= 6) mode -= 4;
        if (mode == 1 || mode == 2 || mode == 3 || mode == 5)
            pit_start_counter(ch);
    }
}

void Mz2500::pit_write_control(uint8_t value) {
    const int ch = value >> 6;
    if (ch == 3) return; // 8254 read-back: not on a 8253
    PitChannel& c = pit_[ch];
    const int rw = (value >> 4) & 3;
    if (rw == 0) { // counter latch command
        c.latch = pit_current(ch);
        c.latched = true;
        c.rd_phase = 0;
        return;
    }
    c.control = value;
    c.wr_phase = 0;
    c.rd_phase = 0;
    c.loaded = false;
    c.counting = false;
    c.terminal = false;
    if (ch == 0) pit_counting_ = false;
}

void Mz2500::pit_write_counter(int ch, uint8_t value) {
    PitChannel& c = pit_[ch];
    const int rw = (c.control >> 4) & 3;
    bool complete = false;
    if (rw == 1) { // low byte only
        c.reload = (uint16_t)((c.reload & 0xFF00) | value);
        complete = true;
    } else if (rw == 2) { // high byte only
        c.reload = (uint16_t)((c.reload & 0x00FF) | (value << 8));
        complete = true;
    } else { // low then high
        if (c.wr_phase == 0) {
            c.reload = (uint16_t)((c.reload & 0xFF00) | value);
            c.wr_phase = 1;
        } else {
            c.reload = (uint16_t)((c.reload & 0x00FF) | (value << 8));
            c.wr_phase = 0;
            complete = true;
        }
    }
    if (complete) {
        c.loaded = true;
        c.terminal = false;
        int mode = (c.control >> 1) & 7;
        if (mode >= 6) mode -= 4;
        // Hardware-triggered one-shot/strobe modes wait for GATE. The other
        // modes load from the next clock after the count write completes.
        if (mode == 1 || mode == 5) {
            c.counting = false;
            if (ch == 0) pit_counting_ = false;
        } else {
            pit_start_counter(ch);
        }
    }
}

void Mz2500::update_ppi_outputs() {
    opn_.flush_to(cpu_.cyc);
    opn_.set_beeper_level(ppi_port_c_high(2));
    cmt_.set_port_a(ppi_port_a_output(), ppi_[0], cpu_.cyc);
    cmt_.set_port_c(ppi_port_c_output(7), ppi_[2], cpu_.cyc);
}

void Mz2500::ppi_write_control(uint8_t value) {
    if (value & 0x80) {
        ppi_control_ = value;
        // Intel 8255 mode-set clears the three output latches. This is also
        // what makes a mode reconfiguration reliably drop SOUND and VGATE.
        ppi_[0] = ppi_[1] = ppi_[2] = 0;
        update_ppi_outputs();
        return;
    }

    const int bit = (value >> 1) & 7;
    ppi_[2] = (uint8_t)((ppi_[2] & ~(1 << bit)) | ((value & 1) << bit));
    update_ppi_outputs();
}

uint8_t Mz2500::pit_read_counter(int ch) {
    PitChannel& c = pit_[ch];
    const uint16_t value = c.latched ? c.latch : pit_current(ch);
    const int rw = (c.control >> 4) & 3;
    if (rw == 1) {
        c.latched = false;
        return (uint8_t)value;
    }
    if (rw == 2) {
        c.latched = false;
        return (uint8_t)(value >> 8);
    }
    if (c.rd_phase == 0) {
        c.rd_phase = 1;
        return (uint8_t)value;
    }
    c.rd_phase = 0;
    c.latched = false;
    return (uint8_t)(value >> 8);
}

// RP5C15 real-time clock: 4-bit registers, bank 0 = current time (served
// from the host clock so TIME$/date are right), bank 1 = alarm and mode
// latches (register Dh bit0 selects the bank). BASIC-M25 probes the chip at
// boot and refuses to arm its clock framework when the reads are garbage.
uint8_t Mz2500::rtc_read(int reg) {
    if (reg == 0x0D) return rtc_mode_;
    if (reg == 0x0E || reg == 0x0F) return 0;
    if (rtc_mode_ & 1) return rtc_bank1_[reg];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    switch (reg) {
    case 0x0: return (uint8_t)(tm.tm_sec % 10);
    case 0x1: return (uint8_t)(tm.tm_sec / 10);
    case 0x2: return (uint8_t)(tm.tm_min % 10);
    case 0x3: return (uint8_t)(tm.tm_min / 10);
    case 0x4: return (uint8_t)(tm.tm_hour % 10);
    case 0x5: return (uint8_t)(tm.tm_hour / 10);
    case 0x6: return (uint8_t)tm.tm_wday;
    case 0x7: return (uint8_t)(tm.tm_mday % 10);
    case 0x8: return (uint8_t)(tm.tm_mday / 10);
    case 0x9: return (uint8_t)((tm.tm_mon + 1) % 10);
    case 0xA: return (uint8_t)((tm.tm_mon + 1) / 10);
    case 0xB: return (uint8_t)(tm.tm_year % 10);
    case 0xC: return (uint8_t)((tm.tm_year / 10) % 10);
    default: return 0;
    }
}

void Mz2500::rtc_write(int reg, uint8_t value) {
    if (reg == 0x0D) { rtc_mode_ = value; return; }
    if (reg == 0x0E || reg == 0x0F) return; // test / reset latches
    if (rtc_mode_ & 1) rtc_bank1_[reg] = value;
    // bank-0 time writes are accepted silently; the host clock stays the
    // source of truth for reads
}

void Mz2500::service_interrupts() {
    sio_.advance(cpu_.cyc);
    printer_.advance(cpu_.cyc);
    // I/O controller sources (port C6h, I/O map): bit3 = VBLANK (CRTC),
    // bit2 = 8253 timer, bit1 = printer, bit0 = RTC (RP5C15).
    if (pit_counting_ && cpu_.cyc >= pit_next_fire_) {
        const uint64_t period = (uint64_t)pit_[0].count() * PIT_CLOCK_DIV;
        int mode = (pit_[0].control >> 1) & 7;
        if (mode >= 6) mode -= 4;
        if (mode == 2 || mode == 3) {
            while (cpu_.cyc >= pit_next_fire_)
                pit_next_fire_ += period; // merge missed ticks
        } else {
            pit_counting_ = false;
            pit_[0].counting = false;
            pit_[0].terminal = true;
        }
        if (int_select_ & 0x04) int_pending_[2] = true;
    }
    // VBLANK: one interrupt per frame at the start of vertical blanking.
    // BASIC-M25's console scroll parks on a flag its VBLANK handler clears.
    {
        const uint64_t vbl = frame_origin_ + frame_line_start(visible_lines());
        if (cpu_.cyc >= vbl && last_vblank_frame_ != frames_) {
            last_vblank_frame_ = frames_;
            if (int_select_ & 0x08) int_pending_[3] = true;
        }
    }
    // RTC (RP5C15) periodic interrupt: ~16 Hz (the chip's 16 Hz pulse
    // output). Calibrated as one interrupt per 4 frames (13.87 Hz) to match
    // EmuZ's cursor-blink cadence exactly - BASIC divides this tick down for
    // the cursor.
    if (int_select_ & 0x01) {
        constexpr uint64_t PERIOD = (uint64_t)CYCLES_PER_FRAME * 4;
        if (tick_next_[0] == 0) tick_next_[0] = cpu_.cyc + PERIOD;
        if (cpu_.cyc >= tick_next_[0]) {
            while (cpu_.cyc >= tick_next_[0]) tick_next_[0] += PERIOD;
            int_pending_[0] = true;
        }
    }
    // The printer interface requests source 1 when the active-low STA/ACK
    // pulse begins. A pulse that happened while PRTIE was disabled is not
    // held for a later enable.
    if (printer_.consume_interrupt() && (int_select_ & 0x02))
        int_pending_[1] = true;
    // The SIO is a daisy-chained Z80 peripheral and supplies its own WR2
    // vector, including the status modification bits when enabled.
    if (cpu_.iff1 && !cpu_.iff_delay) {
        uint8_t vector = 0;
        if (sio_.acknowledge_interrupt(cpu_.cyc, vector)) {
            if (trace_io_)
                std::fprintf(stderr, "[int] f%llu sio vec=%02X\n",
                             (unsigned long long)frames_, vector);
            z80_gen_int(&cpu_, vector);
            return;
        }
    }
    if (cpu_.iff1 && !cpu_.iff_delay) {
        for (int src = 3; src >= 0; src--) {
            if (int_pending_[src] && (int_select_ & (1 << src))) {
                int_pending_[src] = false;
                if (trace_io_)
                    std::fprintf(stderr, "[int] f%llu src%d vec=%02X\n",
                                 (unsigned long long)frames_, src, int_vectors_[src]);
                z80_gen_int(&cpu_, int_vectors_[src]);
                break;
            }
        }
    }
}

void Mz2500::cb_reti(z80* z) {
    static_cast<Mz2500*>(z->userdata)->sio_.reti();
}

size_t Mz2500::debug_json(char* buf, size_t cap) {
    const uint16_t sad0 = (uint16_t)(gde_regs_[0x10] | (gde_regs_[0x11] << 8));
    const int written = std::snprintf(
        buf, cap,
        "{\"frames\":%llu,\"cycles\":%llu,"
        "\"cpu\":{\"pc\":%u,\"sp\":%u,\"a\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,"
        "\"im\":%u,\"iff1\":%u,\"halted\":%u},"
        "\"bank\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"fdc\":{\"motor\":%u,\"drive\":%u,\"cyl\":%u,\"reads\":%llu,\"seeks\":%llu},"
        "\"gde\":{\"mode\":%u,\"sad0\":%u,\"hdsc\":%u},"
        "\"video\":{\"crtc0\":%u,\"cgmask\":%u,\"font\":%u},"
        "\"compat\":{\"boot\":%u,\"memory\":%u,\"display\":%u,\"frame_cycles\":%u},"
        "\"text80\":%u,\"kanji\":%u,"
        "\"int\":{\"select\":%u,\"vector\":%u,\"pit_reload\":%u,\"pit_on\":%u},"
        "\"ipl_rom\":%u}",
        (unsigned long long)frames_, (unsigned long long)cpu_.cyc,
        cpu_.pc, cpu_.sp, cpu_.a, (cpu_.b << 8) | cpu_.c, (cpu_.d << 8) | cpu_.e,
        (cpu_.h << 8) | cpu_.l, cpu_.interrupt_mode, cpu_.iff1 ? 1 : 0,
        cpu_.halted ? 1 : 0,
        mem_.map_of(0), mem_.map_of(1), mem_.map_of(2), mem_.map_of(3),
        mem_.map_of(4), mem_.map_of(5), mem_.map_of(6), mem_.map_of(7),
        fdc_.motor_on() ? 1 : 0, (unsigned)fdc_.selected_drive(),
        (unsigned)fdc_.physical_cylinder(), (unsigned long long)fdc_.stat_reads,
        (unsigned long long)fdc_.stat_seeks,
        gde_regs_[0x0E], sad0, gde_regs_[0x0F] & 7,
        crtc_regs_[0x00], cg_mask_, font_size_,
        boot_mode_, memory_compat_mode(), display_compat_mode(), frame_cycles(),
        (pio_a_ & 0x20) ? 1 : 0, mem_.kanji_bank(),
        int_select_, int_vectors_[2], pit_[0].reload, pit_counting_ ? 1 : 0,
        mem_.has_ipl_rom() ? 1 : 0);
    return written > 0 && (size_t)written < cap ? (size_t)written : 0;
}

void Mz2500::dump_forensics(const char* why) {
    std::fprintf(stderr, "[trap] %s at frame %llu\n", why, (unsigned long long)frames_);
    std::fprintf(stderr, "[trap] recent PCs:");
    for (int i = 0; i < PC_RING; i++) {
        const uint16_t pc = pc_ring_[(pc_ring_pos_ + i) % PC_RING];
        if (i % 16 == 0) std::fprintf(stderr, "\n[trap]   ");
        std::fprintf(stderr, "%04X ", pc);
    }
    std::fprintf(stderr, "\n[trap] recent I/O:\n");
    for (int i = 0; i < IO_RING; i++) {
        const IoEvent& e = io_ring_[(io_ring_pos_ + i) % IO_RING];
        if (e.cyc == 0) continue;
        std::fprintf(stderr, "[trap]   %llu %s %04X = %02X\n", (unsigned long long)e.cyc,
                     e.out ? "OUT" : "IN ", e.port, e.value);
    }
    char json[1024];
    debug_json(json, sizeof(json));
    std::fprintf(stderr, "[trap] state: %s\n", json);
}

void Mz2500::run_frame() {
    const int this_frame_cycles = frame_cycles();
    const uint64_t end = frame_origin_ + this_frame_cycles;
    const auto sync_frame_devices = [this]() {
        opn_.flush_to(cpu_.cyc);
        // Flush unconditionally so chip time stays synchronized even with the
        // board pulled. Drop samples produced while nobody drains the ring.
        adpcm_.flush_to(cpu_.cyc);
        cmt_.sync(cpu_.cyc);
        if (!adpcm_present_) adpcm_.discard_audio();
    };
    seed_raster_lines();
    if (real_ipl_ram_init_pending_) {
        // The physical IPL performs its RAM test before it loads and starts
        // the disk program. Applying it at the first frame preserves the
        // reset boundary while preventing stale program RAM from leaking
        // into a repeated IPL.
        mem_.clear_main_ram();
        real_ipl_ram_init_pending_ = false;
    }
    if (idle_frames_remaining_ > 0) {
        // The real IPL ROM is "running" - burn the frame without touching RAM.
        // Audio time must still advance because the browser uses produced
        // samples to pace emulation while the dummy IPL holds the Z80 payload.
        idle_frames_remaining_--;
        cpu_.cyc = end;
        sync_frame_devices();
        frame_origin_ = end;
        frames_++;
        return;
    }
    while (cpu_.cyc < end) {
        pc_ring_[pc_ring_pos_] = cpu_.pc;
        pc_ring_pos_ = (pc_ring_pos_ + 1) % PC_RING;
        insn_pc_ = cpu_.pc;
        if (loop_start_ >= 0) {
            if (loop_last_ && cpu_.pc == (uint16_t)loop_end_) {
                const uint64_t d = cpu_.cyc - loop_last_;
                loop_.passes++;
                loop_.total += d;
                if (d > loop_.worst) loop_.worst = d;
                if (d > static_cast<uint64_t>(this_frame_cycles)) {
                    loop_.over++;
                    loop_.over_total += d;
                }
                loop_last_ = 0;
            }
            if (!loop_last_ && cpu_.pc == (uint16_t)loop_start_) loop_last_ = cpu_.cyc;
        }
        if (trap_watch_ >= 0 && !trap_hit_ && cpu_.pc == (uint16_t)trap_watch_) {
            trap_hit_ = true;
            dump_forensics("watch PC reached");
        }
        const uint64_t step_start = cpu_.cyc;
        step_external_wait_ = 0;
        cpu_step_active_ = true;
        z80_step(&cpu_);
        cpu_step_active_ = false;
        if (boot_mode_ != 0) {
            const uint64_t elapsed = cpu_.cyc - step_start;
            const uint64_t internal = elapsed >= step_external_wait_
                ? elapsed - step_external_wait_ : 0;
            const uint64_t half_ticks = internal * 3 + cpu_half_cycle_;
            cpu_.cyc = step_start + step_external_wait_ + half_ticks / 2;
            cpu_half_cycle_ = static_cast<uint8_t>(half_ticks & 1);
        } else {
            cpu_half_cycle_ = 0;
        }
        service_interrupts();
    }
    sync_frame_devices();
    frame_origin_ = end;
    frames_++;
}

size_t Mz2500::read_audio(float* out, size_t max_samples) {
    const size_t n = opn_.read_audio(out, max_samples);
    if (adpcm_present_ && n > 0) {
        float tmp[512];
        size_t added = 0;
        while (added < n) {
            const size_t want = std::min(n - added, sizeof(tmp) / sizeof(tmp[0]));
            const size_t m = adpcm_.read_audio(tmp, want);
            if (m == 0) break;
            for (size_t i = 0; i < m; i++) out[added + i] += tmp[i];
            added += m;
        }
    }
    return n;
}

} // namespace mz
