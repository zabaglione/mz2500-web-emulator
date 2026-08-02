#include "core/mz2500.h"

#include <cstdio>
#include <cstring>

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

uint8_t Mz2500::cb_read(void* ud, uint16_t addr) {
    return static_cast<Mz2500*>(ud)->mem_.read(addr);
}
void Mz2500::cb_write(void* ud, uint16_t addr, uint8_t value) {
    static_cast<Mz2500*>(ud)->mem_.write(addr, value);
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
    const uint64_t in_frame = cpu_.cyc % CYCLES_PER_FRAME;
    const int line = static_cast<int>(in_frame / CYCLES_PER_LINE);
    const int in_line = static_cast<int>(in_frame % CYCLES_PER_LINE);
    uint8_t flags = 0;
    if (line >= VBLANK_START_LINE) flags |= 0x01;
    if (in_line >= CYCLES_PER_LINE - HBLANK_CYCLES) flags |= 0x02;
    return flags;
}

void Mz2500::log_port_once(uint16_t port, const char* dir) {
    const uint8_t low = port & 0xFF;
    bool* table = dir[0] == 'i' ? warned_in_ : warned_out_;
    if (!table[low]) {
        table[low] = true;
        std::fprintf(stderr, "[io] unimplemented %s port %02Xh (bus %04Xh)\n", dir, low, port);
    }
}

uint8_t Mz2500::io_in(uint16_t port) {
    switch (port & 0xFF) {
    case 0xB4: return mem_.in_b4();
    case 0xB5: return mem_.in_b5();
    case 0xBD: // GDE status: bit0 = busy (hardware clear in progress)
        return cpu_.cyc < gde_busy_until_ ? 0x01 : 0x00;
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
    case 0xE0:
        // 8255 port A = keyboard data for the firmware's scan (active low,
        // FFh when idle); the row is strobed on port C's low nibble
        return static_cast<uint8_t>(~key_rows_[ppi_[2] & 0x0F]);
    case 0xE1:
        // 8255 port B status inputs (decoded from the IPL's FD probe at its
        // routine 752Dh): bit5 = FD interface absent, bit3 = drive ready.
        // Report the interface present and ready whenever a disk is in the
        // selected drive.
        return (uint8_t)((ppi_[1] & ~0x28) |
                         (disks_[fdc_.selected_drive()].loaded() ? 0x08 : 0x00));
    case 0xE2:
        return ppi_[2];
    case 0xE4: case 0xE5: case 0xE6:
        return pit_read_counter((port & 0xFF) - 0xE4);
    case 0xE8: return pio_a_;
    case 0xEA: return static_cast<uint8_t>(~key_rows_[pio_a_ & 0x0F]);
    case 0xEF: return static_cast<uint8_t>(~joy_mask_);
    case 0xF4: return blank_flags();
    default:
        log_port_once(port, "in");
        return 0xFF;
    }
}

void Mz2500::io_out(uint16_t port, uint8_t value) {
    switch (port & 0xFF) {
    case 0xAE:
        if (mz1m10_present_) palette_[(port >> 8) & 0x1F] = value;
        else log_port_once(port, "out"); // board not installed
        return;
    case 0xB4: mem_.out_b4(value); return;
    case 0xB5: mem_.out_b5(value); return;
    case 0xB7:
        // bank mode: bit0 = 1 switches the IPL ROM overlay out, exposing the
        // RAM banks under 0000-7FFFh (the firmware relocates its vectors to
        // RAM first, then flips this)
        bank_mode_ = value;
        if (value & 1) {
            for (int block = 0; block < 4; block++) {
                if (mem_.map_of(block) == (uint8_t)(0x34 + block))
                    mem_.set_map(block, (uint8_t)block);
            }
        }
        return;
    case 0xBC: gde_index_ = value & 0x1F; return;
    case 0xBD:
        // mode register bit7 = hardware GRAM clear: wipe the graphics banks
        // and raise the busy flag (read back on BDh bit0) for a short while
        if (gde_index_ == 0x0E && (value & 0x80)) {
            for (int bank = 0x20; bank <= 0x27; bank++)
                std::memset(mem_.bank_ptr(bank), 0, BankedMemory::BANK_SIZE);
            gde_busy_until_ = cpu_.cyc + 3000; // ~0.5 ms
            gde_regs_[gde_index_] = value & 0x7F;
            return;
        }
        gde_regs_[gde_index_] = value;
        return;
    case 0xCA:
        // firmware beeper strobe (error tone): bit4 drives the speaker line
        opn_.flush_to(cpu_.cyc);
        opn_.set_beeper_level((value & 0x10) != 0);
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
    case 0xCE: mem_.set_dict_bank(value); return;
    case 0xCF: mem_.set_kanji_bank(value); return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        fdc_.write((port & 0xFF) - 0xD8, static_cast<uint8_t>(~value), cpu_.cyc);
        return;
    case 0xDC: fdc_.write_drive(value); return;
    case 0xDD: fdc_.write_side(value); return;
    case 0xDE: return; // density select: ignored
    case 0xE0: case 0xE1: case 0xE2:
        ppi_[(port & 0xFF) - 0xE0] = value; // 8255 latches (firmware pokes these)
        return;
    case 0xE3:
        if (!(value & 0x80)) { // bit set/reset mode targets port C bits
            const int bit = (value >> 1) & 7;
            ppi_[2] = (uint8_t)((ppi_[2] & ~(1 << bit)) | ((value & 1) << bit));
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
    case 0xF5: crtc_regs_[crtc_index_] = value; return;
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
    const uint64_t ticks = (cpu_.cyc - c.start_cyc) / PIT_CLOCK_DIV;
    return (uint16_t)(c.count() - 1 - (ticks % c.count()));
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

void Mz2500::service_interrupts() {
    if (pit_counting_ && cpu_.cyc >= pit_next_fire_) {
        const uint64_t period = (uint64_t)pit_[0].count() * PIT_CLOCK_DIV;
        while (cpu_.cyc >= pit_next_fire_) pit_next_fire_ += period; // merge missed ticks
        if (int_select_ & 0x04) int_pending_[2] = true;
    }
    // system tick sources 0 and 1 (identity under study; periodic delivery
    // keeps the firmware's ISR-driven state machine advancing)
    for (int src = 0; src < 2; src++) {
        if (!(int_select_ & (1 << src))) continue;
        constexpr uint64_t PERIOD = 100'000; // ~60 Hz
        if (tick_next_[src] == 0) tick_next_[src] = cpu_.cyc + PERIOD;
        if (cpu_.cyc >= tick_next_[src]) {
            while (cpu_.cyc >= tick_next_[src]) tick_next_[src] += PERIOD;
            int_pending_[src] = true;
        }
    }
    if (cpu_.iff1 && !cpu_.iff_delay) {
        for (int src = 3; src >= 0; src--) {
            if (int_pending_[src] && (int_select_ & (1 << src))) {
                int_pending_[src] = false;
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
