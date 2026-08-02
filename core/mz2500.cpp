#include "core/mz2500.h"

#include <cstdio>

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
    return static_cast<Mz2500*>(z->userdata)->io_in(port);
}
void Mz2500::cb_out(z80* z, uint16_t port, uint8_t value) {
    static_cast<Mz2500*>(z->userdata)->io_out(port, value);
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
    case 0xC8: return opn_.read_status(cpu_.cyc);
    case 0xC9: return opn_.read_data();
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        return static_cast<uint8_t>(~fdc_.read((port & 0xFF) - 0xD8, cpu_.cyc));
    case 0xE0: case 0xE1: case 0xE2:
        return ppi_[(port & 0xFF) - 0xE0];
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
    case 0xB7: bank_mode_ = value; return; // latch only (mode switching unused)
    case 0xBC: gde_index_ = value & 0x1F; return;
    case 0xBD: gde_regs_[gde_index_] = value; return;
    case 0xC6:
        int_select_ = value;
        if (!(value & 0x04)) pit_int_pending_ = false; // masking drops the latch
        return;
    case 0xC7: int_vector_ = value; return;
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
    case 0xE4: pit_write_counter(value); return;
    case 0xE5: case 0xE6:
        log_port_once(port, "out"); // PIT ch1/ch2: unused by this game
        return;
    case 0xE7:
        // control word; MZSD sends 34h = ch0, lo/hi latch, mode 2
        if ((value & 0xC0) == 0x00) {
            pit_write_phase_ = 0;
            pit_counting_ = false;
        } else {
            log_port_once(port, "out");
        }
        return;
    case 0xE8: pio_a_ = value; return;
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

void Mz2500::pit_write_counter(uint8_t value) {
    if (pit_write_phase_ == 0) {
        pit_reload_ = (pit_reload_ & 0xFF00) | value;
        pit_write_phase_ = 1;
    } else {
        pit_reload_ = static_cast<uint16_t>((pit_reload_ & 0x00FF) | (value << 8));
        pit_write_phase_ = 0;
        const uint32_t count = pit_reload_ ? pit_reload_ : 0x10000;
        pit_counting_ = true;
        pit_next_fire_ = cpu_.cyc + (uint64_t)count * PIT_CLOCK_DIV;
    }
}

void Mz2500::service_interrupts() {
    if (pit_counting_ && cpu_.cyc >= pit_next_fire_) {
        const uint32_t count = pit_reload_ ? pit_reload_ : 0x10000;
        const uint64_t period = (uint64_t)count * PIT_CLOCK_DIV;
        while (cpu_.cyc >= pit_next_fire_) pit_next_fire_ += period; // merge missed ticks
        if (int_select_ & 0x04) pit_int_pending_ = true;
    }
    if (pit_int_pending_ && cpu_.iff1 && !cpu_.iff_delay) {
        pit_int_pending_ = false;
        z80_gen_int(&cpu_, int_vector_);
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
        int_select_, int_vector_, pit_reload_, pit_counting_ ? 1 : 0,
        mem_.has_ipl_rom() ? 1 : 0);
    return written > 0 && (size_t)written < cap ? (size_t)written : 0;
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
        z80_step(&cpu_);
        service_interrupts();
    }
    opn_.flush_to(cpu_.cyc);
    frame_origin_ = end;
    frames_++;
}

} // namespace mz
