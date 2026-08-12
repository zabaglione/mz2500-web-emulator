#include "core/z80_sio.h"

#include <algorithm>

#include "core/timing.h"

namespace mz {

void Z80Sio::reset(uint64_t cycle) {
    (void)cycle;
    for (int level = 0; level < INTERRUPT_LEVELS; level++)
        interrupt_[level] = InterruptState{};

    for (int ch = 0; ch < 2; ch++) {
        const uint32_t clock = channel_[ch].clock_hz;
        channel_[ch] = Channel{};
        channel_[ch].clock_hz = clock;
        // A reset sets both Tx Buffer Empty (RR0 D2) and Tx Underrun/EOM
        // (RR0 D6). The former is derived from tx_buffer_full below.
        channel_[ch].regs[0] = 0;
    }
}

void Z80Sio::reset_channel(int channel) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    const uint32_t clock = c.clock_hz;

    // Completed bytes have already left TxD. Preserve the host-facing queue
    // on a channel command, but an external machine reset() clears it by
    // replacing the entire Channel before reaching this helper.
    const auto output = c.tx_output;
    const size_t output_head = c.tx_output_head;
    const size_t output_count = c.tx_output_count;
    const uint64_t output_overruns = c.tx_output_overruns;
    const bool cts = c.cts;
    const bool dcd = c.dcd;
    const bool sync = c.sync;

    c = Channel{};
    c.clock_hz = clock;
    c.tx_output = output;
    c.tx_output_head = output_head;
    c.tx_output_count = output_count;
    c.tx_output_overruns = output_overruns;
    c.cts = cts;
    c.dcd = dcd;
    c.sync = sync;

    // A WR0 channel reset affects only that channel. A hardware reset uses
    // reset(), which clears the whole package and both daisy-chain halves.
    interrupt_[rx_level(ch)] = InterruptState{};
    interrupt_[tx_level(ch)] = InterruptState{};
    interrupt_[ext_level(ch)] = InterruptState{};
}

uint8_t Z80Sio::read_data(int channel, uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (c.rx_count == 0) return 0;

    const uint8_t value = c.rx[0].value;
    for (size_t i = 1; i < c.rx_count; i++) c.rx[i - 1] = c.rx[i];
    c.rx_count--;
    c.rx_special_pending = false;
    update_receive_interrupt(ch);
    return value;
}

uint64_t Z80Sio::write_data(int channel, uint8_t value, uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];

    uint64_t waited = 0;
    if (c.tx_buffer_full && c.tx_shift_active && c.tx_shift_end > cycle) {
        // /WAIT holds the I/O write until the byte in the shifter completes
        // and the pending holding byte can move into it.
        waited = c.tx_shift_end - cycle;
        advance(c.tx_shift_end);
    }
    if (c.tx_buffer_full) return 0;

    // Only an accepted load satisfies a Tx-buffer-empty interrupt.
    interrupt_[tx_level(ch)].pending = false;
    c.tx_buffer = value;
    c.tx_buffer_full = true;
    try_start_transmit(ch, cycle + waited);
    return waited;
}

uint8_t Z80Sio::read_control(int channel, uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];
    const uint8_t selected = c.pointer;
    c.pointer = 0;

    switch (selected) {
    case 0: return rr0(ch);
    case 1: return rr1(ch);
    case 2: return ch == 1 ? rr2() : 0;
    default: return 0;
    }
}

void Z80Sio::write_control(int channel, uint8_t value, uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];

    if (c.pointer == 0) {
        c.regs[0] = value;
        execute_wr0(ch, value, cycle);
        // A channel reset owns the pointer result and leaves it at zero.
        if (((value >> 3) & 7) != 3) c.pointer = value & 7;
        return;
    }

    const uint8_t selected = c.pointer;
    c.pointer = 0;
    write_register(ch, selected, value, cycle);
}

void Z80Sio::execute_wr0(int channel, uint8_t value, uint64_t cycle) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    switch ((value >> 3) & 7) {
    case 0: break;
    case 1: // Send Abort is meaningful only to the unmodelled SDLC encoder.
        break;
    case 2:
        interrupt_[ext_level(ch)].pending = false;
        break;
    case 3:
        reset_channel(ch);
        return;
    case 4:
        c.first_rx_armed = true;
        update_receive_interrupt(ch);
        break;
    case 5:
        interrupt_[tx_level(ch)].pending = false;
        break;
    case 6:
        c.error_latch = 0;
        for (size_t i = 0; i < c.rx_count; i++) c.rx[i].errors = RX_OK;
        c.rx_special_pending = false;
        update_receive_interrupt(ch);
        break;
    case 7:
        if (ch == 0) reti();
        break;
    }

    // WR0 D7-D6 reset the CRC engines in synchronous modes. The asynchronous
    // model has no CRC stream, but code 3 still has the CPU-visible effect of
    // clearing the reset-default Tx Underrun/EOM latch.
    if ((value >> 6) == 3) c.tx_underrun_eom = false;
    (void)cycle;
}

void Z80Sio::write_register(int channel, int reg, uint8_t value,
                            uint64_t cycle) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (reg < 1 || reg > 7) return;
    c.regs[reg] = value;

    if (reg == 1) {
        if (!(value & 0x01)) interrupt_[ext_level(ch)].pending = false;
        if (!(value & 0x02)) interrupt_[tx_level(ch)].pending = false;
        if (!(value & 0x18)) interrupt_[rx_level(ch)].pending = false;
        update_receive_interrupt(ch);
    } else if (reg == 3) {
        if (!(value & 0x01)) interrupt_[rx_level(ch)].pending = false;
        update_receive_interrupt(ch);
    } else if (reg == 5) {
        if (value & 0x02) c.rts_output = true;
        else if (!c.tx_shift_active && !c.tx_buffer_full) c.rts_output = false;
        try_start_transmit(ch, cycle);
    }
}

void Z80Sio::set_clock_hz(int channel, uint32_t hz) {
    channel_[index(channel)].clock_hz = hz;
}

uint32_t Z80Sio::clock_hz(int channel) const {
    return channel_[index(channel)].clock_hz;
}

int Z80Sio::clock_multiplier(int channel) const {
    return 1 << ((channel_[index(channel)].regs[4] >> 6) * 1 +
                 ((channel_[index(channel)].regs[4] >> 6) != 0 ? 3 : 0));
}

uint32_t Z80Sio::baud(int channel) const {
    const Channel& c = channel_[index(channel)];
    const int multiplier = clock_multiplier(channel);
    return multiplier ? c.clock_hz / static_cast<uint32_t>(multiplier) : 0;
}

int Z80Sio::receive_bits(int channel) const {
    static constexpr int bits[4] = {5, 7, 6, 8};
    return bits[channel_[index(channel)].regs[3] >> 6];
}

int Z80Sio::transmit_bits(int channel) const {
    static constexpr int bits[4] = {5, 7, 6, 8};
    return bits[(channel_[index(channel)].regs[5] >> 5) & 3];
}

uint8_t Z80Sio::stop_half_bits(int channel) const {
    static constexpr uint8_t halves[4] = {0, 2, 3, 4};
    return halves[(channel_[index(channel)].regs[4] >> 2) & 3];
}

Z80Sio::Parity Z80Sio::parity(int channel) const {
    const uint8_t wr4 = channel_[index(channel)].regs[4];
    if (!(wr4 & 0x01)) return Parity::None;
    return (wr4 & 0x02) ? Parity::Even : Parity::Odd;
}

bool Z80Sio::receiver_enabled(int channel) const {
    const Channel& c = channel_[index(channel)];
    if (!(c.regs[3] & 0x01)) return false;
    return !(c.regs[3] & 0x20) || c.dcd;
}

bool Z80Sio::transmitter_enabled(int channel) const {
    const Channel& c = channel_[index(channel)];
    if (!(c.regs[5] & 0x08)) return false;
    return !(c.regs[3] & 0x20) || c.cts;
}

uint64_t Z80Sio::character_cycles(int channel) const {
    const Channel& c = channel_[index(channel)];
    if (c.clock_hz == 0) return 0;

    const int data_halves = transmit_bits(channel) * 2;
    const int parity_halves = parity(channel) == Parity::None ? 0 : 2;
    const int stop_halves = stop_half_bits(channel);
    const int start_halves = stop_halves ? 2 : 0;
    const uint64_t half_bits = static_cast<uint64_t>(start_halves + data_halves +
                                                     parity_halves + stop_halves);
    const uint64_t numerator = CPU_HZ * half_bits * clock_multiplier(channel);
    return std::max<uint64_t>(1, (numerator + 2 * c.clock_hz - 1) /
                                     (2 * c.clock_hz));
}

uint64_t Z80Sio::receive_character_cycles(int channel) const {
    const Channel& c = channel_[index(channel)];
    if (c.clock_hz == 0) return 0;

    const int data_halves = receive_bits(channel) * 2;
    const int parity_halves = parity(channel) == Parity::None ? 0 : 2;
    const int stop_halves = stop_half_bits(channel);
    const int start_halves = stop_halves ? 2 : 0;
    const uint64_t half_bits = static_cast<uint64_t>(start_halves + data_halves +
                                                     parity_halves + stop_halves);
    const uint64_t numerator = CPU_HZ * half_bits * clock_multiplier(channel);
    return std::max<uint64_t>(1, (numerator + 2 * c.clock_hz - 1) /
                                     (2 * c.clock_hz));
}

void Z80Sio::try_start_transmit(int channel, uint64_t cycle) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (c.tx_shift_active || !c.tx_buffer_full || !transmitter_enabled(ch)) return;

    const uint64_t duration = character_cycles(ch);
    if (duration == 0) return;

    c.tx_shift_active = true;
    c.tx_shift.value = static_cast<uint8_t>(
        c.tx_buffer & (transmit_bits(ch) == 8 ? 0xFF : ((1 << transmit_bits(ch)) - 1)));
    c.tx_shift.data_bits = static_cast<uint8_t>(transmit_bits(ch));
    c.tx_shift.parity = parity(ch);
    c.tx_shift.stop_half_bits = stop_half_bits(ch);
    c.tx_shift.baud = baud(ch);
    c.tx_shift.completed_cycle = cycle + duration;
    c.tx_shift_end = cycle + duration;
    c.tx_buffer_full = false;

    // The buffer becoming empty is the Tx interrupt condition.
    if (c.regs[1] & 0x02) interrupt_[tx_level(ch)].pending = true;
}

void Z80Sio::queue_transmitted(int channel, const TxByte& value) {
    Channel& c = channel_[index(channel)];
    if (c.tx_output_count == TX_OUTPUT_SIZE) {
        c.tx_output_overruns++;
        return;
    }
    const size_t tail = (c.tx_output_head + c.tx_output_count) % TX_OUTPUT_SIZE;
    c.tx_output[tail] = value;
    c.tx_output_count++;
}

void Z80Sio::complete_transmit(int channel, uint64_t cycle) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (!c.tx_shift_active) return;

    // Send Break forces TxD low in place of the serial character.
    if (!(c.regs[5] & 0x10)) queue_transmitted(ch, c.tx_shift);
    c.tx_shift_active = false;
    try_start_transmit(ch, cycle);

    if (!c.tx_shift_active && !c.tx_buffer_full && !(c.regs[5] & 0x02))
        c.rts_output = false;
}

void Z80Sio::advance(uint64_t cycle) {
    for (int ch = 0; ch < 2; ch++) {
        Channel& c = channel_[ch];
        while (c.tx_shift_active && cycle >= c.tx_shift_end) {
            const uint64_t completed = c.tx_shift_end;
            complete_transmit(ch, completed);
        }
        try_start_transmit(ch, cycle);

        while (c.rx_input_count != 0 && cycle >= c.rx_input_next) {
            const uint64_t completed = c.rx_input_next;
            const RxByte byte = c.rx_input[c.rx_input_head];
            c.rx_input_head = (c.rx_input_head + 1) % RX_INPUT_SIZE;
            c.rx_input_count--;
            accept_received(ch, byte.value, byte.errors);
            if (c.rx_input_count != 0)
                c.rx_input_next = completed + receive_character_cycles(ch);
        }
    }
}

bool Z80Sio::pop_transmitted(int channel, TxByte& value) {
    Channel& c = channel_[index(channel)];
    if (c.tx_output_count == 0) return false;
    value = c.tx_output[c.tx_output_head];
    c.tx_output_head = (c.tx_output_head + 1) % TX_OUTPUT_SIZE;
    c.tx_output_count--;
    return true;
}

size_t Z80Sio::transmitted_available(int channel) const {
    return channel_[index(channel)].tx_output_count;
}

uint64_t Z80Sio::transmitted_overruns(int channel) const {
    return channel_[index(channel)].tx_output_overruns;
}

void Z80Sio::receive_byte(int channel, uint8_t value, uint8_t errors,
                          uint64_t cycle) {
    advance(cycle);
    accept_received(index(channel), value, errors);
}

bool Z80Sio::queue_receive_byte(int channel, uint8_t value, uint8_t errors,
                                uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (!receiver_enabled(ch)) return false;
    if (c.rx_input_count == RX_INPUT_SIZE) {
        c.rx_input_overruns++;
        return false;
    }

    const size_t tail = (c.rx_input_head + c.rx_input_count) % RX_INPUT_SIZE;
    c.rx_input[tail] = {value, errors};
    if (c.rx_input_count++ == 0) {
        const uint64_t duration = receive_character_cycles(ch);
        if (duration == 0) {
            c.rx_input_count = 0;
            return false;
        }
        c.rx_input_next = cycle + duration;
    }
    return true;
}

void Z80Sio::accept_received(int channel, uint8_t value, uint8_t errors) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (!receiver_enabled(ch)) return;

    const int bits = receive_bits(ch);
    value &= static_cast<uint8_t>(bits == 8 ? 0xFF : ((1 << bits) - 1));
    errors &= static_cast<uint8_t>(RX_PARITY_ERROR | RX_OVERRUN_ERROR |
                                   RX_FRAMING_ERROR);

    if (c.rx_count < RX_FIFO_SIZE) {
        c.rx[c.rx_count++] = {value, errors};
    } else {
        // The fourth character replaces the third on a Z80 SIO and the
        // replacement is the word flagged as overrun.
        errors |= RX_OVERRUN_ERROR;
        c.rx[RX_FIFO_SIZE - 1] = {value, errors};
    }

    c.error_latch |= errors & static_cast<uint8_t>(RX_PARITY_ERROR |
                                                   RX_OVERRUN_ERROR);
    const int mode = (c.regs[1] >> 3) & 3;
    const bool parity_special = mode == 2 && (errors & RX_PARITY_ERROR);
    const bool special = parity_special ||
                         (errors & (RX_OVERRUN_ERROR | RX_FRAMING_ERROR));
    if (special) c.rx_special_pending = true;

    if (mode == 1) {
        if (c.first_rx_armed || special) {
            interrupt_[rx_level(ch)].pending = true;
            c.first_rx_armed = false;
        }
    } else if (mode >= 2) {
        interrupt_[rx_level(ch)].pending = true;
    }
}

bool Z80Sio::rx_available(int channel) const {
    return channel_[index(channel)].rx_count != 0;
}

void Z80Sio::update_receive_interrupt(int channel) {
    const int ch = index(channel);
    Channel& c = channel_[ch];
    const int level = rx_level(ch);
    const int mode = (c.regs[1] >> 3) & 3;
    if (!receiver_enabled(ch) || c.rx_count == 0 || mode == 0) {
        interrupt_[level].pending = false;
        return;
    }

    const uint8_t errors = c.rx[0].errors;
    const bool parity_special = mode == 2 && (errors & RX_PARITY_ERROR);
    c.rx_special_pending = parity_special ||
                           (errors & (RX_OVERRUN_ERROR | RX_FRAMING_ERROR));
    if (mode >= 2) interrupt_[level].pending = true;
    else if (mode == 1 && c.first_rx_armed) interrupt_[level].pending = true;
}

void Z80Sio::set_external_pending(int channel) {
    const int ch = index(channel);
    if (channel_[ch].regs[1] & 0x01) interrupt_[ext_level(ch)].pending = true;
}

void Z80Sio::set_modem_inputs(int channel, bool cts, bool dcd, bool sync,
                              uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (c.cts != cts || c.dcd != dcd || c.sync != sync) set_external_pending(ch);
    c.cts = cts;
    c.dcd = dcd;
    c.sync = sync;
    try_start_transmit(ch, cycle);
}

void Z80Sio::set_break_input(int channel, bool active, uint64_t cycle) {
    advance(cycle);
    const int ch = index(channel);
    Channel& c = channel_[ch];
    if (c.break_input != active) set_external_pending(ch);
    c.break_input = active;
}

bool Z80Sio::dtr(int channel) const {
    return (channel_[index(channel)].regs[5] & 0x80) != 0;
}

bool Z80Sio::rts(int channel) const {
    return channel_[index(channel)].rts_output;
}

bool Z80Sio::break_active(int channel) const {
    return (channel_[index(channel)].regs[5] & 0x10) != 0;
}

uint8_t Z80Sio::rr0(int channel) const {
    const int ch = index(channel);
    const Channel& c = channel_[ch];
    uint8_t value = c.tx_underrun_eom ? 0x40 : 0x00;
    if (c.rx_count != 0) value |= 0x01;
    if (ch == 0) {
        for (const auto& state : interrupt_)
            if (state.pending) value |= 0x02;
    }
    if (!c.tx_buffer_full) value |= 0x04;
    if (c.dcd) value |= 0x08;
    if (c.sync) value |= 0x10;
    if (c.cts) value |= 0x20;
    if (c.break_input) value |= 0x80;

    return value;
}

uint8_t Z80Sio::rr1(int channel) const {
    const Channel& c = channel_[index(channel)];
    uint8_t value = (!c.tx_shift_active && !c.tx_buffer_full) ? 0x01 : 0x00;
    value |= c.error_latch;
    if (c.rx_count != 0) value |= c.rx[0].errors;
    return value;
}

int Z80Sio::highest_eligible_interrupt() const {
    int service_limit = INTERRUPT_LEVELS;
    for (int level = 0; level < INTERRUPT_LEVELS; level++) {
        if (interrupt_[level].in_service) {
            service_limit = level;
            break;
        }
    }
    for (int level = 0; level < service_limit; level++)
        if (interrupt_[level].pending) return level;
    return -1;
}

uint8_t Z80Sio::vector_for_level(int level) const {
    static constexpr uint8_t normal_code[INTERRUPT_LEVELS] = {
        6, 4, 5, 2, 0, 1,
    };
    uint8_t code = normal_code[level];
    if (level == A_RX && channel_[0].rx_special_pending) code = 7;
    if (level == B_RX && channel_[1].rx_special_pending) code = 3;

    const uint8_t base = channel_[1].regs[2];
    if (!(channel_[1].regs[1] & 0x04)) return base;
    return static_cast<uint8_t>((base & 0xF1) | (code << 1));
}

uint8_t Z80Sio::rr2() const {
    const int level = highest_eligible_interrupt();
    return level < 0 ? channel_[1].regs[2] : vector_for_level(level);
}

bool Z80Sio::interrupt_pending(uint64_t cycle) {
    advance(cycle);
    return highest_eligible_interrupt() >= 0;
}

bool Z80Sio::acknowledge_interrupt(uint64_t cycle, uint8_t& vector) {
    advance(cycle);
    const int level = highest_eligible_interrupt();
    if (level < 0) return false;
    vector = vector_for_level(level);
    interrupt_[level].pending = false;
    interrupt_[level].in_service = true;
    return true;
}

void Z80Sio::reti() {
    for (int level = 0; level < INTERRUPT_LEVELS; level++) {
        if (!interrupt_[level].in_service) continue;
        interrupt_[level].in_service = false;
        if (level == A_RX) update_receive_interrupt(0);
        if (level == B_RX) update_receive_interrupt(1);
        return;
    }
}

} // namespace mz
