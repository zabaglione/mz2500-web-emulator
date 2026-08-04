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
    cpu_.userdata = this;
    for (int i = 0; i < FdcMb8877::NUM_DRIVES; i++) fdc_.attach(i, &disks_[i]);
}

bool Mz2500::insert_disk(int drive, const std::string& path) {
    return disks_[drive & 1].load_file(path);
}

void Mz2500::set_rom(int kind, const uint8_t* data, size_t size) {
    switch (kind) {
    case 0: mem_.load_ipl_rom(data, size); break;
    case 1: break; // cg.rom: stored by the frontend, not wired yet (text uses PCG)
    case 2: mem_.load_kanji_rom(data, size); break;
    case 3: mem_.load_dict_rom(data, size); break;
    }
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
    if (line >= VBLANK_START_LINE) return false; // nothing is scanned at all
    if (bank >= 0x20 && bank <= 0x33) return graphics_scanning(line);
    if (bank == 0x38 || bank == 0x39) return text_scanning(line);
    return false;
}

// The weight the port B5h bank table charges for touching this block, plus
// the stall to the blanking period if the controller has the bus. Both are
// paid before the access completes, so the byte lands at the end of the
// wait, which is where the bus puts it.
void Mz2500::charge_access_wait(int bank) {
    const int weight = bank_access_wait(bank);
    if (!weight) return;
    const int stall =
        layer_scanning(bank, current_line()) ? display_stall_cycles(cpu_.cyc) : 0;
    cpu_.cyc += (unsigned long)(stall + weight);
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
    const int bank = m->mem_.map_of(addr >> 13);
    m->charge_access_wait(bank);
    if (bank >= 0x30 && bank <= 0x33)
        return m->gvram_rmw_read(bank, addr & 0x1FFF);
    return m->mem_.read(addr);
}
void Mz2500::cb_write(void* ud, uint16_t addr, uint8_t value) {
    auto* m = static_cast<Mz2500*>(ud);
    const int bank = m->mem_.map_of(addr >> 13);
    m->charge_access_wait(bank);
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
        uint8_t* dst = mem_.bank_ptr(base + p * 2 + (int)(lin >> 13)) + (lin & 0x1FFF);
        const uint8_t pat = gde_regs_[p];
        uint8_t d = *dst;
        switch ((fn >> 6) & 3) {
        case 0: // REPLACE: punch the mask, fit the patterned data
            d = (uint8_t)((d & ~mask) |
                          ((colour & (1 << p)) ? (value & pat & mask) : 0));
            break;
        case 1: // PSET: punch the written bits, fit the patterned data
            d = (uint8_t)((d & ~value) |
                          ((colour & (1 << p)) ? (value & pat) : 0));
            break;
        default: // clear: the written bits are knocked out of every plane
            d = (uint8_t)(d & ~value);
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
        return mem_.bank_ptr(base + p * 2 + (int)(lin >> 13))[lin & 0x1FFF];
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
    const int in_frame = static_cast<int>(cpu_.cyc % CYCLES_PER_FRAME);
    const int line = line_of_cycle(in_frame);
    uint8_t flags = 0;
    if (line >= VBLANK_START_LINE) flags |= 0x01;
    if (in_frame >= line_start_cycle(line + 1) - HBLANK_CYCLES) flags |= 0x02;
    return flags;
}

// Which raster of the current frame the CPU is on. Same frame origin as
// blank_flags(), so the two never disagree about where vertical blanking is.
int Mz2500::current_line() const {
    const int line = line_of_cycle(static_cast<int>(cpu_.cyc % CYCLES_PER_FRAME));
    return line < LINES_PER_FRAME ? line : LINES_PER_FRAME - 1;
}

// Start of frame: every line inherits the window the registers hold now.
void Mz2500::seed_hwin_lines() {
    for (int line = 0; line < LINES_PER_FRAME; line++) {
        hwin_line_[line][0] = gde_regs_[0x0C];
        hwin_line_[line][1] = gde_regs_[0x0D];
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
           (p >= 0xA0 && p <= 0xA3) ||
           p == 0xAE || p == 0xB4 || p == 0xB5 || p == 0xB7 || p == 0xCA ||
           p == 0xF4 || p == 0xF5 || p == 0xF6 || p == 0xC6 || p == 0xC7 || p == 0xCC ||
           (p >= 0xE4 && p <= 0xE7) || p == 0xEF || p == 0xF0 || p == 0xCF ||
           p == 0xBC || p == 0xBD || p == 0xF7;
}

// Read register 0 of a SIO channel. Bit0 = a character is waiting, bit2 =
// the transmit buffer is empty. With no device on the line the first is
// never true and the second always is.
uint8_t Mz2500::sio_status(int ch) const {
    const SioChannel& c = sio_[ch & 1];
    switch (c.pointer) {
    case 0: return (uint8_t)(0x04 | (c.rx_empty() ? 0x00 : 0x01));
    case 1: return 0x01; // all sent
    case 2: return sio_[1].regs[2]; // interrupt vector, channel B only
    default: return 0x00;
    }
}

// A rising edge on DTR is the mouse's strobe: one three-byte packet goes
// into the receiver, exactly as the measured protocol describes.
void Mz2500::sio_dtr_changed(int ch, bool dtr) {
    const bool rising = dtr && !sio_dtr_[ch];
    sio_dtr_[ch] = dtr;
    if (!rising || ch != 1 || !mouse_connected()) return;
    uint8_t packet[3];
    mouse_.take_packet(packet);
    for (uint8_t b : packet) sio_[1].push(b);
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
    case 0xA0: case 0xA2:
        return sio_[((port & 0xFF) == 0xA2) ? 1 : 0].pop();
    case 0xA1: case 0xA3: {
        const int ch = ((port & 0xFF) == 0xA3) ? 1 : 0;
        const uint8_t v = sio_status(ch);
        sio_[ch].pointer = 0;
        return v;
    }
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
    case 0xAF:
        // TV tuner control board (MZ-1T01 class option, not installed):
        // bit0 = TVPOWER input. An absent tuner reads 0, the same trap as
        // the telephone unit on CAh - open bus would say "tuner powered".
        return 0x00;
    case 0xE0:
        // 8255 port A = keyboard data for the firmware's scan (active low,
        // FFh when idle); the row is strobed on port C's low nibble
        return static_cast<uint8_t>(~key_rows_[ppi_[2] & 0x0F]);
    case 0xE1: {
        // 8255 port B = cassette-deck status (I/O map E1h). No data recorder
        // is emulated: TREADY(bit5)=1 and WREADY(bit4)=1 say "no tape",
        // TEND(bit3)=1 says the deck is stopped - the firmware's deck
        // commands then complete immediately. bit0 mirrors VBLANK
        // (0 = blanking, 1 = display), which boot code uses for frame sync.
        uint8_t v = 0x38;
        if (!(blank_flags() & 0x01)) v |= 0x01;
        return v;
    }
    case 0xE2:
        return ppi_[2];
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
    case 0xF4:
        // The I/O map has both bits the other way round from blank_flags():
        // "0: Horizontal blanking period / 1: Horizontal display period",
        // and the same for VB. Report the display period, not the blanking.
        return (uint8_t)(~blank_flags() & 0x03);
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
    case 0xA0: case 0xA2: return; // SIO transmit: nothing on the line
    case 0xA1: case 0xA3: {
        // Writing register 0 both selects the next register and carries the
        // command bits; every other register is a plain store and hands the
        // port back to register 0.
        SioChannel& c = sio_[((port & 0xFF) == 0xA3) ? 1 : 0];
        const int ch = ((port & 0xFF) == 0xA3) ? 1 : 0;
        if (c.pointer == 0) {
            c.regs[0] = value;
            c.pointer = value & 0x07;
        } else {
            const uint8_t reg = c.pointer;
            c.regs[reg] = value;
            c.pointer = 0;
            // WR5 bit7 is DTR. The mouse driver polls nothing: it toggles
            // this line twice a frame and takes the answer as a receive
            // interrupt, so a rising edge is what asks for a packet.
            if (reg == 5) sio_dtr_changed(ch, (value & 0x80) != 0);
        }
        return;
    }
    case 0xAE:
        if (mz1m10_present_) {
            palette_[(port >> 8) & 0x1F] = value;
            palette_written_ = true;
        }
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xB4: mem_.out_b4(value); return;
    case 0xB5: mem_.out_b5(value); return;
    case 0xB7:
        // bank mode: bit0 = 1 drops the reset overlay (IPL ROM at 0000-7FFF)
        // and loads the run map: RAM banks 00/01 under 0000-3FFF and the
        // second ROM half (banks 36/37) at 4000-7FFF. The firmware depends
        // on this exact map - after the flip the CPU slides over zeroed RAM
        // into 4000h and re-enters the ROM there (its 0000-3FFF half is
        // reached later through RST 18h syscalls that map banks 34/35).
        bank_mode_ = value;
        if (value & 1) {
            static const uint8_t run_map[4] = {0x00, 0x01, 0x36, 0x37};
            for (int block = 0; block < 4; block++) {
                if (mem_.map_of(block) == (uint8_t)(0x34 + block))
                    mem_.set_map(block, run_map[block]);
            }
        }
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
        // mode register bit7 = hardware GRAM clear: wipe the graphics banks
        // and raise the busy flag (read back on BDh bit0) for a short while
        if (reg == 0x0E && (value & 0x80)) {
            for (int bank = 0x20; bank <= 0x27; bank++)
                std::memset(mem_.bank_ptr(bank), 0, BankedMemory::BANK_SIZE);
            gde_busy_until_ = cpu_.cyc + 3000; // ~0.5 ms
            gde_regs_[reg] = value & 0x7F;
            return;
        }
        gde_regs_[reg] = value;
        // The horizontal window takes effect from the raster being scanned
        // and holds until it is written again.
        if (reg == 0x0C || reg == 0x0D) {
            for (int line = current_line(); line < LINES_PER_FRAME; line++)
                hwin_line_[line][reg - 0x0C] = value;
        }
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
        opn_regs_[opn_addr_] = value; // renderer reads the GPIO latch (reg 0Eh)
        opn_.write_data(value, cpu_.cyc);
        return;
    case 0xCC:
        rtc_write((port >> 8) & 0x0F, value & 0x0F);
        return;
    case 0xCE: mem_.set_dict_bank(value); return;
    case 0xCF: mem_.set_kanji_bank(value); return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        fdc_.write((port & 0xFF) - 0xD8, static_cast<uint8_t>(~value), cpu_.cyc);
        return;
    case 0xDC: fdc_.write_drive(value); return;
    case 0xDD: fdc_.write_side(value); return;
    case 0xDE: return; // density select: ignored
    case 0xE0: case 0xE1:
        ppi_[(port & 0xFF) - 0xE0] = value; // 8255 latches (firmware pokes these)
        return;
    case 0xE2:
        // 8255 port C: bit2 = SOUND drives the BEEP speaker (BASIC's startup
        // pip toggles it at audio rate)
        ppi_[2] = value;
        opn_.flush_to(cpu_.cyc);
        opn_.set_beeper_level((ppi_[2] & 0x04) != 0);
        return;
    case 0xE3:
        if (!(value & 0x80)) { // bit set/reset mode targets port C bits
            const int bit = (value >> 1) & 7;
            ppi_[2] = (uint8_t)((ppi_[2] & ~(1 << bit)) | ((value & 1) << bit));
            if (bit == 2) {
                opn_.flush_to(cpu_.cyc);
                opn_.set_beeper_level((ppi_[2] & 0x04) != 0);
            }
        }
        return;
    case 0xE4: case 0xE5: case 0xE6:
        pit_write_counter((port & 0xFF) - 0xE4, value);
        return;
    case 0xE7: pit_write_control(value); return;
    case 0xE8: pio_a_ = value; return;
    case 0xE9: case 0xEB:
        pio_ctrl_[((port & 0xFF) - 0xE9) >> 1] = value; // PIO mode words: latch
        return;
    case 0xEF: joy_enable_ = value; return;
    case 0xF0: return; // PIT gate strobe: counting starts when the reload lands
    case 0xF4: crtc_index_ = value; return;
    case 0xF5:
        crtc_regs_[crtc_index_] = value;
        // Remember that a program has taken charge of a text display
        // window. The CRTC's power-on register contents are undocumented
        // and the native IPL replacement does not model the firmware's
        // CRTC set-up, so the renderer needs to tell "no window has ever
        // been programmed" from "a window is programmed shut" - and the
        // register values alone cannot say which, because a shut window
        // is exactly SL = EL / SC = EC (see core/renderer.cpp).
        if (crtc_index_ == 0x03 || crtc_index_ == 0x05) crtc_vwin_written_ = true;
        if (crtc_index_ == 0x07 || crtc_index_ == 0x08) crtc_hwin_written_ = true;
        return;
    case 0xF6: cg_mask_ = value; return;
    case 0xF7: font_size_ = value; return;
    default:
        log_port_once(port, "out");
        return;
    }
}

uint16_t Mz2500::pit_current(int ch) const {
    const PitChannel& c = pit_[ch];
    if (!c.counting) return (uint16_t)c.reload;
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
    return (uint16_t)(c.count() - 1 - ((ticks - 1) % c.count()));
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
    c.counting = false;
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
        c.counting = true;
        c.start_cyc = cpu_.cyc;
        if (ch == 0) {
            pit_counting_ = true;
            pit_next_fire_ = cpu_.cyc + (uint64_t)c.count() * PIT_CLOCK_DIV;
        }
    }
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
    // I/O controller sources (port C6h, I/O map): bit3 = VBLANK (CRTC),
    // bit2 = 8253 timer, bit1 = printer, bit0 = RTC (RP5C15).
    if (pit_counting_ && cpu_.cyc >= pit_next_fire_) {
        const uint64_t period = (uint64_t)pit_[0].count() * PIT_CLOCK_DIV;
        while (cpu_.cyc >= pit_next_fire_) pit_next_fire_ += period; // merge missed ticks
        if (int_select_ & 0x04) int_pending_[2] = true;
    }
    // VBLANK: one interrupt per frame at the start of vertical blanking.
    // BASIC-M25's console scroll parks on a flag its VBLANK handler clears.
    {
        const uint64_t vbl = frame_origin_ + line_start_cycle(VBLANK_START_LINE);
        if (cpu_.cyc >= vbl && last_vblank_frame_ != frames_) {
            last_vblank_frame_ = frames_;
            if (int_select_ & 0x08) int_pending_[3] = true;
        }
    }
    // RTC (RP5C15) periodic interrupt: ~16 Hz (the chip's 16 Hz pulse
    // output). Calibrated as one interrupt per 4 frames (13.87 Hz) to match
    // EmuZ's cursor-blink cadence exactly - BASIC divides this tick down for
    // the cursor. The printer (bit1) never interrupts - no printer attached.
    if (int_select_ & 0x01) {
        constexpr uint64_t PERIOD = (uint64_t)CYCLES_PER_FRAME * 4;
        if (tick_next_[0] == 0) tick_next_[0] = cpu_.cyc + PERIOD;
        if (cpu_.cyc >= tick_next_[0]) {
            while (cpu_.cyc >= tick_next_[0]) tick_next_[0] += PERIOD;
            int_pending_[0] = true;
        }
    }
    // Z80 SIO channel B receive. The SIO is a daisy-chained Z80 peripheral,
    // so it puts its own vector on the bus (WR2) rather than going through
    // the C6h controller. It interrupts while a character is waiting and the
    // receiver and its interrupts are enabled; reading the data port clears
    // the condition.
    if (cpu_.iff1 && !cpu_.iff_delay) {
        SioChannel& b = sio_[1];
        if (!b.rx_empty() && (b.regs[3] & 0x01) && (b.regs[1] & 0x18)) {
            if (trace_io_)
                std::fprintf(stderr, "[int] f%llu sio-b vec=%02X\n",
                             (unsigned long long)frames_, b.regs[2]);
            z80_gen_int(&cpu_, b.regs[2]);
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
    const uint64_t end = frame_origin_ + CYCLES_PER_FRAME;
    seed_hwin_lines();
    if (idle_frames_remaining_ > 0) {
        // the real IPL ROM is "running" - burn the frame without touching RAM
        idle_frames_remaining_--;
        cpu_.cyc = end;
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
                if (d > (uint64_t)CYCLES_PER_FRAME) {
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
        z80_step(&cpu_);
        service_interrupts();
    }
    opn_.flush_to(cpu_.cyc);
    frame_origin_ = end;
    frames_++;
}

} // namespace mz
