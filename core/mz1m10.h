// MZ-1M10 4096-colour palette board: the table the firmware leaves in its
// palette RAM before a disk's program takes over.
//
// The board holds sixteen RGB444 entries, written a nibble pair at a time
// through port AEh. MZ2500_IO_Map.pdf ("4096 color palette") gives the
// write as a 16-bit I/O address: bits 15:9 are PALNO, bit 8 picks which
// half of the entry the data byte lands in - 0 writes DATA[7:4] to red and
// DATA[3:0] to blue, 1 writes DATA[3:0] to green - so a program addresses
// entry n with B = n * 2 for its red/blue byte and B = n * 2 + 1 for its
// green. Palette number 0 is wired to black and takes no entry.
//
// Nothing in the hardware clears this RAM, and the firmware programs a full
// table before it hands the machine over, so a program that writes only the
// entries it cares about still gets a sensible screen for the rest. The
// table is the one the renderer's fixed-colour path already documents:
// component level 7 for colours 1-7, 3 for colour 8, and 15 for 9-15, with
// the IGRB code's G/R/B bits choosing which components a colour lights.
//
// Seeding it at boot is what keeps such a program off a black screen: the
// renderer switches the whole picture to the board's entries the moment any
// one of them is written, so entries left at zero would come out black -
// text included. GRAZE STORM is the case that found this: it writes two
// entries and leaves the other fourteen to the firmware.
#pragma once

#include <cstdint>

namespace mz {

// Fill a 32-byte palette RAM image: two bytes per entry, the even byte
// (R << 4) | B and the odd byte G, which is the order port AEh writes them.
inline void mz1m10_firmware_palette(uint8_t out[32]) {
    out[0] = out[1] = 0; // entry 0 is black on the board, not in the RAM
    for (int i = 1; i < 16; i++) {
        // Colour 8 is the IGRB code with intensity and no colour bits at
        // all; the firmware gives it a level of its own rather than
        // leaving it black, which is why it is not derived from i's bits.
        const int level = (i == 8) ? 3 : (i < 8 ? 7 : 15);
        const int r = (i == 8) ? level : ((i & 0x02) ? level : 0);
        const int g = (i == 8) ? level : ((i & 0x04) ? level : 0);
        const int b = (i == 8) ? level : ((i & 0x01) ? level : 0);
        out[i * 2] = static_cast<uint8_t>((r << 4) | b);
        out[i * 2 + 1] = static_cast<uint8_t>(g);
    }
}

} // namespace mz
