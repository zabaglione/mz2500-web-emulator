// Frame composer: text layer (PCG glyphs) over the GDE CG layer, resolved
// through the CLUT and the MZ-1M10 RGB444 palette into a 640x400 RGBA frame.
//
// Register semantics come from this repository's own documentation
// (docs/mz2500-highspeed-scroll-knowhow.md) and the game's init sequences in
// games/neko_can_run/src/neko.asm; ambiguities (plane order, glyph bit
// order, digital palette hue order) are calibrated black-box against
// EmuZ-2500 screenshots. No emulator code was consulted.
//
// Layout facts used here:
//   text VRAM (bank 38h): row stride = column count (80 or 40 bytes,
//     PIO-A bit5 selects the mode), attr plane at +0800h
//   PCG RAM  (bank 39h): PCG0 at +0000h, PCG1-3 at +0800/1000/1800h
//   attr bit3 = 1: colour glyph, 3-bit pixel colour from PCG1-3 planes
//   attr bit3 = 0: monochrome glyph from PCG0, colour = 8 + (attr & 7)
//   CG: 320x200, 4 planes (banks 20h/22h/24h/26h), ring of SAD1+1 bytes,
//       row select SAD0/SAD2 split at SLN1, HDSC 0-7 px fine scroll,
//       GDEHS/GDEHE horizontal window in 4-px units, GDEVS/GDEVE in lines
#include "core/mz2500.h"

namespace mz {

namespace {

inline uint16_t reg16(const uint8_t* regs, int lo) {
    return static_cast<uint16_t>(regs[lo] | (regs[lo + 1] << 8));
}

} // namespace

void Mz2500::render(uint8_t* rgba) const {
    // --- resolve the 16-colour output palette -------------------------------
    // OPN GPIO port A (register 0Eh) bit2: 0 selects the MZ-1M10 RGB444
    // palette, 1 the digital palette (used during FDC access windows).
    const bool rgb444 = (opn_regs_[0x0E] & 0x04) == 0;
    uint8_t pal_r[16], pal_g[16], pal_b[16];
    for (int i = 0; i < 16; i++) {
        if (rgb444) {
            // nibble << 4 expansion, matching EmuZ output byte-for-byte so
            // screenshot regressions can demand exact equality
            const uint8_t even = palette_[i * 2];     // (R<<4) | B
            const uint8_t odd = palette_[i * 2 + 1];  // G
            pal_r[i] = static_cast<uint8_t>(((even >> 4) & 0x0F) << 4);
            pal_g[i] = static_cast<uint8_t>((odd & 0x0F) << 4);
            pal_b[i] = static_cast<uint8_t>((even & 0x0F) << 4);
        } else {
            // digital palette: bit3 = intensity, bit2..0 = G/R/B
            const int hi = (i & 0x08) ? 255 : 160;
            pal_g[i] = (i & 0x04) ? hi : 0;
            pal_r[i] = (i & 0x02) ? hi : 0;
            pal_b[i] = (i & 0x01) ? hi : 0;
        }
    }

    // --- CG layer state ------------------------------------------------------
    const bool cg_on = gde_regs_[0x0E] == 0x15; // 320x200 16-colour mode
    const uint16_t gdevs = reg16(gde_regs_, 0x08);
    const uint16_t gdeve = reg16(gde_regs_, 0x0A);
    const int win_x0 = gde_regs_[0x0C] * 4; // 4-px units
    const int win_x1 = gde_regs_[0x0D] * 4;
    const int hdsc = gde_regs_[0x0F] & 7;
    const uint16_t sad0 = reg16(gde_regs_, 0x10);
    const uint16_t sad1 = reg16(gde_regs_, 0x12);
    const uint16_t sad2 = reg16(gde_regs_, 0x14);
    const uint16_t sln1 = reg16(gde_regs_, 0x16);
    const uint8_t plane_mask = gde_regs_[0x18] & 0x0F;
    const uint32_t ring = sad1 + 1u;
    const uint8_t* plane[4] = {mem_.bank_ptr(0x20), mem_.bank_ptr(0x22),
                               mem_.bank_ptr(0x24), mem_.bank_ptr(0x26)};

    // --- text layer state ----------------------------------------------------
    const bool text80 = (pio_a_ & 0x20) != 0;
    const uint8_t* tvram = mem_.bank_ptr(0x38);
    const uint8_t* pcg = mem_.bank_ptr(0x39);

    for (int y = 0; y < 400; y++) {
        const int gy = y >> 1; // 200-line space shared by CG and text
        uint8_t* row = rgba + (size_t)y * 640 * 4;

        // CG source row
        const uint8_t* cg_valid = nullptr;
        uint32_t cg_base = 0;
        if (cg_on && gy >= gdevs && gy < gdeve) {
            cg_base = ((gy < sln1 ? sad0 : sad2) + 40u * gy) % ring;
            cg_valid = reinterpret_cast<const uint8_t*>(1); // marker
        }

        const int trow = gy >> 3;
        const int gline = gy & 7;

        for (int x = 0; x < 640; x++) {
            // text pixel (0 = transparent)
            int color = 0;
            {
                const int cell = text80 ? (x >> 3) : (x >> 4);
                const int gbit = text80 ? (x & 7) : ((x >> 1) & 7);
                const int idx = trow * (text80 ? 80 : 40) + cell;
                const uint8_t code = tvram[idx];
                const uint8_t attr = tvram[0x800 + idx];
                if (code | attr) {
                    if (attr & 0x08) {
                        const int go = code * 8 + gline;
                        const int b1 = (pcg[0x0800 + go] >> (7 - gbit)) & 1;
                        const int b2 = (pcg[0x1000 + go] >> (7 - gbit)) & 1;
                        const int b3 = (pcg[0x1800 + go] >> (7 - gbit)) & 1;
                        const int c = b1 | (b2 << 1) | (b3 << 2);
                        if (c) color = 8 + c;
                    } else {
                        const uint8_t g = pcg[code * 8 + gline];
                        if ((g >> (7 - gbit)) & 1) color = 8 + (attr & 7);
                    }
                }
            }

            // CG pixel under a transparent text pixel
            if (color == 0 && cg_valid) {
                const int gx = x >> 1;
                if (gx >= win_x0 && gx < win_x1) {
                    const int px = gx - hdsc;
                    if (px >= -8) {
                        // arithmetic floor for the possibly negative left edge
                        const int cbyte = px >> 3;
                        const int bit = px & 7;
                        const uint32_t addr = (cg_base + (uint32_t)(cbyte + ring)) % ring;
                        // CG bytes are LSB-leftmost (calibrated against EmuZ)
                        int c = 0;
                        for (int p = 0; p < 4; p++) {
                            if (!(plane_mask & (1 << p))) continue;
                            c |= ((plane[p][addr] >> bit) & 1) << p;
                        }
                        color = c;
                    }
                }
            }

            const int n = crtc_regs_[0x80 + color] & 0x0F; // CLUT
            row[x * 4 + 0] = pal_r[n];
            row[x * 4 + 1] = pal_g[n];
            row[x * 4 + 2] = pal_b[n];
            row[x * 4 + 3] = 0xFF;
        }
    }
}

} // namespace mz
