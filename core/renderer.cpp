// Frame composer: text layer (PCG glyphs) over the GDE CG layer, resolved
// through the CLUT and the MZ-1M10 RGB444 palette into a 640x400 RGBA frame.
//
// Register semantics come from this repository's own documentation
// (docs/mz2500-highspeed-scroll-knowhow.md) and the game's init sequences in
// games/neko_can_run/src/neko.asm; ambiguities (plane order, glyph bit
// order, digital palette hue order) are calibrated black-box against
// EmuZ-2500 screenshots. The legacy compatibility paths are based on Sharp
// documentation and are checked only through external, black-box EmuZ runs.
// No CSCP internal implementation, source code, or data is used here.
//
// Layout facts used here:
//   text VRAM (bank 38h): row stride = column count (80 or 40 bytes,
//     PIO-A bit5 selects the mode), attr plane at +0800h
//   PCG RAM  (bank 39h): PCG0 at +0000h, PCG1-3 at +0800/1000/1800h
//   attr bit7 = blink, bit6 = reverse, bit3 = colour glyph (3-bit pixel
//     colour from PCG1-3), bits 5-4 = mono PCG set, bits 2-0 = colour
//   CG: 4 planes, each a linear 32KB display space (0000-3FFF in the
//       standard VRAM banks 20h-27h, 4000-7FFF in the expansion banks
//       28h-2Fh); 320 or 640 wide by 200 or 400 tall, ring of SAD1+1 bytes,
//       row select SAD0/SAD2 split at SLN1, HDSC 0-7 px fine scroll,
//       GDEHS/GDEHE horizontal window in 4-px units, GDEVS/GDEVE in lines
//   a 320x200 screen is 8000 bytes, so each plane holds two of them one
//       8KB apart; register 18h enables the two screens' planes separately
//       and both are fetched from the one display address counter
#include "core/mz2500.h"

namespace mz {

namespace {

inline uint16_t reg16(const uint8_t* regs, int lo) {
    return static_cast<uint16_t>(regs[lo] | (regs[lo + 1] << 8));
}

// 256-colour mode drives each gun with 3 bits through a straight
// digital-to-analogue ladder, so the eight steps are n x 255 / 7.
constexpr uint8_t LEVEL9[8] = {0, 36, 73, 109, 146, 182, 219, 255};

// Compose one 256-colour pixel into a packed 9-bit R<<6|G<<3|B. The top bit
// of each gun comes from page 0 after the palette, the middle bit from page
// 1 raw, and the bottom bit from whatever CRTC register 0Ah selects: 00 the
// palette's own I bit, 01 page 1's I plane, 10 always set, 11 always clear.
// Register 0Ah holds blue in b1:0, red in b3:2 and green in b5:4.
inline int compose256(int pal0, int c1, uint8_t reg0a) {
    int out = 0;
    const int bit_of[3] = {1, 2, 0}; // R, G, B within the IGRB code
    const int field[3] = {1, 2, 0};  // R, G, B within register 0Ah
    for (int i = 0; i < 3; i++) {
        const int b = bit_of[i];
        int lo = 0;
        switch ((reg0a >> (2 * field[i])) & 3) {
        case 0: lo = (pal0 >> 3) & 1; break;
        case 1: lo = (c1 >> 3) & 1; break;
        case 2: lo = 1; break;
        default: lo = 0; break;
        }
        const int level = 4 * ((pal0 >> b) & 1) + 2 * ((c1 >> b) & 1) + lo;
        out |= level << (6 - i * 3);
    }
    return out;
}

} // namespace

void Mz2500::render_compat_line(uint8_t* row, int y, int mode) const {
    const RasterLineState& state = raster_line_[y];
    const uint8_t* tvram = mem_.bank_ptr(0x38);
    const std::vector<uint8_t>& kanji = mem_.kanji_rom();
    const bool text80 = (state.pio_a & 0x20) != 0;
    const int columns = text80 ? 80 : 40;
    const bool force_blank = (state.ppi_control & 0x01) == 0 &&
                             (state.ppi_c & 0x01) != 0;
    const bool reverse_mono = mode == 2 &&
                              (state.ppi_control & 0x10) == 0 &&
                              (state.ppi_a & 0x10) == 0;

    auto font_byte = [&](uint8_t code, int line) -> uint8_t {
        // Compatibility text uses the MZ-2500 kanji ROM. Black-box EmuZ
        // runs produce identical frames with and without a separate CG ROM,
        // while removing the kanji ROM changes both legacy-mode outputs.
        const size_t embedded = 0x6018 + static_cast<size_t>(code) * 32 + line;
        return embedded < kanji.size() ? kanji[embedded] : 0;
    };
    auto plane_byte = [&](int plane, uint16_t address) -> uint8_t {
        const int bank = 0x20 + plane * 2 + (address >> 13);
        return mem_.bank_present(bank)
            ? mem_.bank_ptr(bank)[address & 0x1FFF] : 0;
    };

    const int sy = y >> 1;
    const int text_line = sy & 7;
    const int text_row = sy >> 3;
    for (int x = 0; x < 640; x++) {
        const int tx = text80 ? x : (x >> 1);
        const int cell = tx >> 3;
        bool text_on = false;
        if (cell < columns && text_row < 25) {
            const uint8_t code = tvram[text_row * columns + cell];
            const uint8_t glyph = font_byte(code, text_line);
            text_on = ((glyph >> (7 - (tx & 7))) & 1) != 0;
        }

        int colour = 0;
        if (mode == 1) {
            // MZ-2000 has 640x200 B/R/G graphics. Its compatibility
            // pages 1,2,3 occupy the native R/G/I plane storage; F6h
            // bits 0,1,2 gate those pages as blue, red and green.
            const uint16_t address = static_cast<uint16_t>(sy * 80 + (x >> 3));
            const int bit = x & 7; // legacy graphics are LSB-leftmost
            int graphic = 0;
            if (state.compat_graphics_mask & 0x01)
                graphic |= ((plane_byte(1, address) >> bit) & 1) << 0;
            if (state.compat_graphics_mask & 0x02)
                graphic |= ((plane_byte(2, address) >> bit) & 1) << 1;
            if (state.compat_graphics_mask & 0x04)
                graphic |= ((plane_byte(3, address) >> bit) & 1) << 2;
            const int text_colour = state.compat_text_colour & 7;
            const bool graphics_first = (state.compat_text_colour & 8) != 0;
            colour = state.compat_background & 7;
            if (graphics_first) {
                if (text_on) colour = text_colour;
                if (graphic) colour = graphic;
            } else {
                if (graphic) colour = graphic;
                if (text_on) colour = text_colour;
            }
        } else {
            // MZ-80B displays two 320x200 monochrome pages. D1/D2 of
            // any F4h-F7h write enable page 1/page 2; text and graphics
            // are ORed, then the result is doubled horizontally.
            const int gx = x >> 1;
            const uint16_t address = static_cast<uint16_t>(sy * 40 + (gx >> 3));
            const int bit = gx & 7;
            bool graphic = false;
            if (state.compat_vram_control & 0x02)
                graphic |= ((plane_byte(0, address) >> bit) & 1) != 0;
            if (state.compat_vram_control & 0x04)
                graphic |= ((plane_byte(1, address) >> bit) & 1) != 0;
            bool mono = text_on || graphic;
            if (reverse_mono) mono = !mono;
            colour = mono ? 4 : 0; // original MZ-80B green phosphor
        }

        uint8_t r = 0, g = 0, b = 0;
        if (!force_blank) {
            r = (colour & 2) ? 255 : 0;
            g = (colour & 4) ? 255 : 0;
            b = (colour & 1) ? 255 : 0;
        }
        uint8_t* pixel = row + x * 4;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = 0xFF;
    }
}

void Mz2500::render(uint8_t* rgba) const {
    static const uint8_t absent_bank[BankedMemory::BANK_SIZE] = {};
    auto graphics_bank = [&](int bank) -> const uint8_t* {
        return mem_.bank_present(bank) ? mem_.bank_ptr(bank) : absent_bank;
    };
    const uint8_t* tvram = mem_.bank_ptr(0x38);
    const uint8_t* pcg = mem_.bank_ptr(0x39);
    // Text2 plane (tvram +1000h) K=1 routes a cell to the kanji ROM font:
    // glyph byte address = L<<17 | A[16:11]<<11 | Text1<<3, one byte per
    // scanline, 16 lines per cell, address bit3 ignored (I/O map, block 38h)
    const std::vector<uint8_t>& krom = mem_.kanji_rom();
    const uint32_t kmask = krom.empty() ? 0 : (uint32_t)krom.size() - 1;
    const bool blink_phase = ((frames_ / 28) & 1) == 0;
    static const int PAGE_SELECT[4] = {3, 1, 2, 3}; // both, first, second, both
    for (int y = 0; y < 400; y++) {
        const RasterLineState& state = raster_line_[y];
        const uint8_t* crtc = state.crtc;
        const uint8_t* gde = state.gde;
        uint8_t* row = rgba + (size_t)y * 640 * 4;

        const int line_mode = crtc[0x0F] & 3;
        if (line_mode == 1 || line_mode == 2) {
            render_compat_line(row, y, line_mode);
            continue;
        }

        // VGATE is a raster signal too. A write during the picture blanks
        // only the lines that have not yet been scanned.
        if ((state.ppi_control & 0x01) == 0 && (state.ppi_c & 0x01)) {
            for (int x = 0; x < 640; x++) {
                row[x * 4 + 0] = 0;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
                row[x * 4 + 3] = 255;
            }
            continue;
        }

        // Resolve the palette from the values that existed on this raster.
        const bool rgb444 = mz1m10_present_ && state.palette_written &&
                            state.opn_port_a_output &&
                            (state.opn_port_a & 0x04) == 0;
        uint8_t pal_r[16], pal_g[16], pal_b[16];
        for (int i = 0; i < 16; i++) {
            if (rgb444 && i != 0) {
                const uint8_t even = state.palette[i * 2];
                const uint8_t odd = state.palette[i * 2 + 1];
                pal_r[i] = static_cast<uint8_t>(((even >> 4) & 0x0F) << 4);
                pal_g[i] = static_cast<uint8_t>((odd & 0x0F) << 4);
                pal_b[i] = static_cast<uint8_t>((even & 0x0F) << 4);
            } else if (rgb444) {
                pal_r[i] = pal_g[i] = pal_b[i] = 0;
            } else if (i == 8) {
                pal_r[i] = pal_g[i] = pal_b[i] = 95;
            } else {
                const int hi = (i & 0x08) ? 255 : 127;
                pal_g[i] = (i & 0x04) ? hi : 0;
                pal_r[i] = (i & 0x02) ? hi : 0;
                pal_b[i] = (i & 0x01) ? hi : 0;
            }
        }

        const uint8_t gmode = gde[0x0E];
        const bool h640 = (gmode & 0x02) != 0;
        const bool v200 = gde_v200(gmode);
        const bool cg4 = !(gmode & 0x10) && !v200 && h640;
        const bool cg256 = (gmode & 0x18) == 0x18 && v200 && !h640;
        const bool cg2page = !h640 && v200 && !cg256 && (gmode & 0x10) != 0;
        const bool page0_front = (gmode & 0x01) != 0;
        const bool cg_on = gde_cg_on(gmode);
        const uint16_t gdevs = reg16(gde, 0x08);
        const uint16_t gdeve = reg16(gde, 0x0A);
        const int win_unit = h640 ? 8 : 4;
        const int cg_stride = h640 ? 80 : 40;
        const int hdsc = gde[0x0F] & 7;
        const uint16_t sad0 = reg16(gde, 0x10);
        const uint16_t sad1 = reg16(gde, 0x12);
        const uint16_t sad2 = reg16(gde, 0x14);
        const uint16_t sln1 = reg16(gde, 0x16);
        const uint8_t plane_mask = gde[0x18] & (cg4 ? 0x03 : 0x0F);
        const uint8_t plane_mask2 = (uint8_t)((gde[0x18] >> 4) & 0x0F);
        const uint32_t ring = sad1 + 1u;
        const uint8_t* plane_bank[4][4];
        for (int p = 0; p < 4; p++)
            for (int q = 0; q < 4; q++)
                plane_bank[p][q] = graphics_bank(
                    gde_plane_bank(p, (uint32_t)q << 13, gmode));

        const bool text80 = (state.pio_a & 0x20) != 0;
        const int text_cols = text80 ? 80 : 40;
        const int text_sa = ((crtc[2] << 8) | crtc[1]) & 0x7FF;
        const int text_vd = (crtc[9] & 0x0F) * 2;
        const bool gun_g = (state.cg_mask & 0x04) != 0;
        const bool gun_r = (state.cg_mask & 0x02) != 0;
        const bool gun_b = (state.cg_mask & 0x01) != 0;
        const bool text_black_bg = (crtc[0x00] & 0x02) != 0;
        const int text_pages = text80 ? 1 : PAGE_SELECT[(crtc[0x00] >> 2) & 3];
        const bool text64 = (crtc[0x00] & 0x01) == 0;
        const WinKind vwin = win_kind(state.crtc_vwin_written,
                                      crtc[3], crtc[5]);
        const int text_y0 = (crtc[3] - TEXT_WIN_LINE0) * 2;
        const int text_y1 = (crtc[5] - TEXT_WIN_LINE0) * 2;
        const int text_digit0 = text80 ? 9 : 8;
        const WinKind hwin = win_kind(state.crtc_hwin_written,
                                      crtc[7], crtc[8]);
        const int text_x0 = (crtc[7] - text_digit0) * 8;
        const int text_x1 = (crtc[8] - text_digit0) * 8;
        const int bg_colour = ((crtc[0x0B] & 0x01) << 3) |
                              ((crtc[0x0C] & 0x01) << 2) |
                              (((crtc[0x0B] >> 5) & 1) << 1) |
                              ((crtc[0x0B] >> 2) & 1);
        const int bg_rgb = (((crtc[0x0B] >> 3) & 0x07) << 6) |
                           ((((crtc[0x0C] & 1) << 2) |
                             ((crtc[0x0B] >> 6) & 0x03)) << 3) |
                           (crtc[0x0B] & 0x07);

        // GDEVS/GDEVE/SLN1 count display lines, so a 400-line mode addresses
        // every raster and a 200-line mode doubles each one.
        const int gy = v200 ? (y >> 1) : y;

        // GDEHS/GDEHE as they stood while this raster was scanned. The
        // 24 kHz frame displays VBLANK_START_LINE = 400 of its 448 lines and
        // this framebuffer is 400 rasters, so the two are the same raster:
        // one RasterLineState entry per row, which is the only ratio at which
        // a program that rewrites the window every raster keeps its shape.
        static_assert(VBLANK_START_LINE == 400,
                      "the framebuffer is one raster per displayed line");
        const int win_x0 = gde[0x0C] * win_unit;
        const int win_x1 = gde[0x0D] * win_unit;

        // CG source row
        const uint8_t* cg_valid = nullptr;
        uint32_t cg_base = 0;
        if (cg_on && gy >= gdevs && gy < gdeve) {
            cg_base = gde_row_address((uint32_t)gy, (uint32_t)cg_stride,
                                      sad0, sad1, sad2, sln1);
            cg_valid = reinterpret_cast<const uint8_t*>(1); // marker
        }

        // Font height (port F7h bit0). In 16-line mode a cell is a full 16
        // rasters of glyph: Text1 bit0 = address A[3] is ignored, so a cell
        // reads 16 consecutive bytes from the even code - which is exactly
        // how BASIC-M25 lays out its logo and text (even codes only). In
        // 8-line mode a cell carries 8 glyph lines, double-scanned, and A[3]
        // selects the half - the layout NEKO CAN RUN's PCG art uses.
        const bool text16 = (state.font_size & 1) == 0;
        const int ty = y + text_vd; // text raster after the fine roll
        // CRTC reg 00h bit4 selects 20 rows instead of 25: the cell grows to
        // 20 rasters, the extra 4 below the glyph carrying the attribute but
        // no pixels.
        const bool rows20 = (crtc[0x00] & 0x10) != 0;
        const int cell_h = rows20 ? 20 : 16;
        const int trow = (ty / cell_h) % (rows20 ? 20 : 25);
        const int cell_line = ty % cell_h;
        const int gline = text16 ? cell_line : (cell_line >> 1);

        const bool text_row_visible = in_win(vwin, y, text_y0, text_y1);

        // One text cell at `idx`, sampled at glyph column `gbit`. Returns
        // whether the cell paints there, with its 3-bit GRB colour in `out`
        // - and colour 0 with a true return is an opaque black.
        auto text_cell = [&](int idx, int gbit, int& out) -> bool {
            out = 0;
            const uint8_t code = tvram[idx];
            const uint8_t attr = tvram[0x800 + idx];
            const uint8_t t2 = tvram[0x1000 + idx];
            if (!(code | attr | t2)) return false;
            // Attribute bit6 reverses the cell and bit7 blinks it. Both work
            // by way of the glyph bit: a blinked-off cell, and the spare
            // rasters under a 20-row cell, simply have no dots, which
            // reverse then paints solid.
            const bool reverse = (attr & 0x40) != 0;
            const bool dots = cell_line < 16 && !((attr & 0x80) && !blink_phase);
            // glyph byte offset: 16-line cells ignore address A[3] (Text1
            // bit0) and span 16 bytes from the even code
            const int go = text16 ? ((code & ~1) * 8 + gline) : (code * 8 + gline);
            if (attr & 0x08) {
                // colour PCG: PCG1-3 give each dot its own colour, and
                // reverse swaps in the complementary one - which is the
                // 3-bit complement, so 000 and 111 trade places and
                // whichever lands on 000 is transparent
                int c = 0;
                if (dots) {
                    c = ((pcg[0x0800 + go] >> (7 - gbit)) & 1) |
                        (((pcg[0x1000 + go] >> (7 - gbit)) & 1) << 1) |
                        (((pcg[0x1800 + go] >> (7 - gbit)) & 1) << 2);
                }
                if (reverse) c ^= 7;
                out = c;
                return c != 0;
            }
            // monochrome glyph: attribute bits 5-4 pick the PCG set, and
            // set 0 defers to Text2 bit7 for the kanji ROM
            int dot = 0;
            if (dots) {
                const int set = (attr >> 4) & 3;
                if (set == 0 && (t2 & 0x80) && kmask) {
                    uint32_t base = ((uint32_t)(t2 & 0x40) << 11) |
                                    ((uint32_t)(t2 & 0x3F) << 11) |
                                    ((uint32_t)code << 3);
                    if (text16) base &= ~0x08u;
                    dot = (krom[(base + (uint32_t)gline) & kmask] >> (7 - gbit)) & 1;
                } else {
                    dot = (pcg[set * 0x800 + go] >> (7 - gbit)) & 1;
                }
            }
            if (!(dot ^ (reverse ? 1 : 0))) return false;
            out = attr & 7;
            return true;
        };

        for (int x = 0; x < 640; x++) {
            // text pixel. `color` is a CLUT index and text_on says the cell
            // paints here at all - the two are not the same, because text
            // colour 000 is an opaque black rather than a transparent hole.
            int color = 0;
            bool from_graphics = false;
            bool text_on = false;
            // 256-colour graphics, and 64-colour text, bypass the 16-entry
            // CLUT: >= 0 is a packed 9-bit R<<6|G<<3|B.
            int rgb = -1;
            if (text_row_visible && in_win(hwin, x, text_x0, text_x1)) {
                const int cell = text80 ? (x >> 3) : (x >> 4);
                const int gbit = text80 ? (x & 7) : ((x >> 1) & 7);
                const int idx = (text_sa + trow * text_cols + cell) & 0x7FF;
                int c0 = 0, c1 = 0;
                const bool on0 = text_pages != 2 && text_cell(idx, gbit, c0);
                const bool on1 = text_pages != 1 &&
                                 text_cell((idx ^ 0x400) & 0x7FF, gbit, c1);
                text_on = on0 || on1;
                if (!on0) c0 = 0;
                if (!on1) c1 = 0;
                if (text64) {
                    // 64 colours: page 1 drives bit2 of each gun, page 2
                    // bit1, and bit0 is wired low - which is why the mode
                    // cannot reach full brightness.
                    if (text_on)
                        rgb = (((c0 >> 1) & 1) << 8) | (((c1 >> 1) & 1) << 7) |
                              (((c0 >> 2) & 1) << 5) | (((c1 >> 2) & 1) << 4) |
                              ((c0 & 1) << 2) | ((c1 & 1) << 1);
                } else if (text_on) {
                    // 8 colours: the first page wins wherever it paints
                    const int c = on0 ? c0 : c1;
                    color = c ? 8 + c : 0;
                }
                // CRTC reg 00h bit1 makes the transparent parts of the text
                // layer opaque black, hiding lower-priority graphics.
                if (!text_on && text_black_bg) text_on = true;
            }

            // CG pixel: shows through a transparent text pixel, and also
            // covers an opaque one when its graphic palette entry has the
            // priority bit (80h-8Fh bit4) set.
            if (cg_valid) {
                const int gx = h640 ? x : (x >> 1);
                if (gx >= win_x0 && gx < win_x1) {
                    const int px = gx - hdsc;
                    if (px < 0) {
                        // Fine scroll pushes the picture right by inserting
                        // 0-7 white pixels at the left edge; programs hide
                        // them by blanking the leftmost cell. This filler is
                        // a graphics-layer artefact of HDSC, not a text
                        // pixel, so port F6h's mask must reach it too.
                        if (!text_on) {
                            color = 15;
                            text_on = true;
                            from_graphics = true;
                        }
                    } else {
                        const int cbyte = px >> 3;
                        const int bit = px & 7;
                        const uint32_t addr = (cg_base + (uint32_t)(cbyte + ring)) % ring;
                        // CG bytes are LSB-leftmost (calibrated against EmuZ)
                        int c = 0;
                        for (int p = 0; p < 4; p++) {
                            if (!(plane_mask & (1 << p))) continue;
                            const uint8_t byte =
                                plane_bank[p][(addr >> 13) & 3][addr & 0x1FFF];
                            c |= ((byte >> bit) & 1) << p;
                        }
                        if (cg2page) {
                            // The second screen rides one 8KB screen above
                            // the first and shares its address counter, so
                            // it scrolls and wraps with it. Colour 0 is the
                            // hole through which the screen behind shows.
                            const uint32_t a1 = addr ^ 0x2000u;
                            int c2 = 0;
                            for (int p = 0; p < 4; p++) {
                                if (!(plane_mask2 & (1 << p))) continue;
                                const uint8_t byte =
                                    plane_bank[p][(a1 >> 13) & 3][a1 & 0x1FFF];
                                c2 |= ((byte >> bit) & 1) << p;
                            }
                            const int front = page0_front ? c : c2;
                            const int back = page0_front ? c2 : c;
                            c = front ? front : back;
                        }
                        // CRTC regs 80h-8Fh are the graphic palette: bit4 =
                        // this colour draws in front of the text layer.
                        const bool wins = !text_on || (crtc[0x80 + c] & 0x10);
                        if (cg256) {
                            // The second page rides at display address xor
                            // 2000h and takes no palette; together the two
                            // pages give each gun 3 bits.
                            const uint32_t a1 = addr ^ 0x2000u;
                            int c1 = 0;
                            for (int p = 0; p < 4; p++) {
                                if (!(plane_mask2 & (1 << p))) continue;
                                const uint8_t byte =
                                    plane_bank[p][(a1 >> 13) & 3][a1 & 0x1FFF];
                                c1 |= ((byte >> bit) & 1) << p;
                            }
                            const int pal0 = crtc[0x80 + c] & 0x0F;
                            if (wins) {
                                rgb = (pal0 == 0 && c1 == 0)
                                          ? bg_rgb
                                          : compose256(pal0, c1, crtc[0x0A]);
                                // Port F6h gates the final 256-colour output.
                                if (!gun_r) rgb &= ~0x1C0;
                                if (!gun_g) rgb &= ~0x038;
                                if (!gun_b) rgb &= ~0x007;
                            }
                        } else {
                            // A graphic pixel that lands on palette code 0
                            // shows the background colour, not plain black.
                            const int mapped =
                                (crtc[0x80 + c] & 0x0F) ? c : bg_colour;
                            if (wins) { color = mapped; from_graphics = true; }
                        }
                    }
                }
            }

            if (rgb >= 0) {
                row[x * 4 + 0] = LEVEL9[(rgb >> 6) & 7];
                row[x * 4 + 1] = LEVEL9[(rgb >> 3) & 7];
                row[x * 4 + 2] = LEVEL9[rgb & 7];
                row[x * 4 + 3] = 255;
                continue;
            }
            // CRTC registers 80h-8Fh are the GRAPHIC layer's palette, not a
            // text palette: bit4 arbitrates priority against the text
            // layer (see the `wins` check above), and a register whose
            // documented job is to arbitrate against text is by
            // definition not something text's own colours pass through.
            // `color` for a text-resolved pixel is already the code the
            // hardware displays, so it goes to the RGB lookup as-is;
            // only a graphics-sourced pixel (from_graphics) still needs
            // the 80h-8Fh translation.
            int n = from_graphics ? (crtc[0x80 + color] & 0x0F) : color;
            if (from_graphics) {
                // Port F6h gates the graphic layer's guns. Only BE gets a
                // pre-lookup clear of the CLUT index: BE is the sole bit
                // that also suppresses the I plane (I/O map), and dropping
                // the I bit changes the shared brightness level the CLUT
                // encodes for the other channels too - only a change to the
                // index *before* the lookup can do that.
                //
                // GE and RE must NOT clear index bits pre-lookup. pal_r/
                // pal_g/pal_b is a real lookup table, not a per-bit function
                // of the code, and colour 8 proves it: it is a hardcoded
                // grey (pal_r[8]=pal_g[8]=pal_b[8]=95, calibrated against
                // the real machine, see above) rather than a derivation from
                // its individual bits. Clearing bit2 (G) or bit1 (R) on a
                // code that is not already 8 can alias it onto index 8 -
                // e.g. code 10 (I,R) loses its R bit under RE=0 and becomes
                // 8 - and index 8 then returns 95 on every channel, not just
                // the one whose gun was actually turned off. The post-lookup
                // gate below already forces green/red to 0 whenever their
                // gun is off, whatever `n` resolves to, so a pre-lookup
                // clear for GE/RE would only create that aliasing and never
                // do anything the gate doesn't already do. This is a no-op
                // at the default mask 07h, so it changes no existing golden
                // screenshot.
                if (!gun_b) n &= ~0x09; // blue takes the intensity with it
                row[x * 4 + 0] = gun_r ? pal_r[n] : 0;
                row[x * 4 + 1] = gun_g ? pal_g[n] : 0;
                row[x * 4 + 2] = gun_b ? pal_b[n] : 0;
            } else {
                row[x * 4 + 0] = pal_r[n];
                row[x * 4 + 1] = pal_g[n];
                row[x * 4 + 2] = pal_b[n];
            }
            row[x * 4 + 3] = 0xFF;
        }
    }
}

namespace {

// JIS X 0201 byte to UTF-8: ASCII through 7Eh (5Ch is the yen key, kept as
// a backslash so what type_text sent comes back unchanged), A1h-DFh the
// half-width katakana block. Everything else is a middle dot placeholder.
size_t jisx0201_utf8(uint8_t c, char* out) {
    if (c == 0x00 || c == 0x20) { out[0] = ' '; return 1; }
    if (c >= 0x21 && c <= 0x7E) { out[0] = (char)c; return 1; }
    if (c >= 0xA1 && c <= 0xDF) {
        const uint32_t u = 0xFF61 + (c - 0xA1); // U+FF61..FF9F
        out[0] = (char)(0xE0 | (u >> 12));
        out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[2] = (char)(0x80 | (u & 0x3F));
        return 3;
    }
    out[0] = '.';
    return 1;
}

} // namespace

size_t Mz2500::screen_text(char* buf, size_t cap) const {
    // Same layout state as render() above: mode from PIO-A bit5, start
    // address from CRTC regs 01h/02h (11-bit counter, wraps at the end of
    // the Text1 plane), 20-row mode from CRTC reg 00h bit4, and the
    // 40-column page pair from reg 00h bits 3-2. The fine roll (reg 09h)
    // moves rasters, not cells, so a text dump ignores it.
    const bool text80 = (pio_a_ & 0x20) != 0;
    const int cols = text80 ? 80 : 40;
    const bool rows20 = (crtc_regs_[0x00] & 0x10) != 0;
    const int rows = rows20 ? 20 : 25;
    const uint8_t* tvram = mem_.bank_ptr(0x38);
    const int text_sa = ((crtc_regs_[2] << 8) | crtc_regs_[1]) & 0x7FF;
    static const int PAGE_SELECT[4] = {3, 1, 2, 3}; // both, first, second, both
    const int pages = text80 ? 1 : PAGE_SELECT[(crtc_regs_[0x00] >> 2) & 3];

    // One cell to UTF-8. Kanji-ROM ANK cells decode to their character
    // (glyph address 6000h + code*32, the inverse of the address formula in
    // render()); kanji cells are a geta mark per half, PCG art cells a hash.
    auto decode_cell = [&](int idx, char* out) -> size_t {
        const uint8_t code = tvram[idx];
        const uint8_t attr = tvram[0x800 + idx];
        const uint8_t t2 = tvram[0x1000 + idx];
        if (!(code | attr | t2)) { out[0] = ' '; return 1; }
        if (attr & 0x08) { out[0] = '#'; return 1; } // colour PCG art
        const int set = (attr >> 4) & 3;
        if (set == 0 && (t2 & 0x80)) {
            const uint32_t addr = ((uint32_t)(t2 & 0x40) << 11) |
                                  ((uint32_t)(t2 & 0x3F) << 11) |
                                  ((uint32_t)code << 3);
            if (addr >= 0x6000 && addr < 0x8000 && (addr & 0x1F) == 0)
                return jisx0201_utf8((uint8_t)((addr - 0x6000) >> 5), out);
            // a kanji: one geta mark per half-width cell
            out[0] = (char)0xE3; out[1] = (char)0x80; out[2] = (char)0x93; // 〓
            return 3;
        }
        // mono PCG glyph: the code byte is whatever font the program loaded;
        // JIS X 0201 is the best available guess
        return jisx0201_utf8(code, out);
    };

    size_t n = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = (text_sa + r * cols + c) & 0x7FF;
            if (pages == 2) idx = (idx ^ 0x400) & 0x7FF;
            if (pages == 3) {
                // both pages composed: dump the first unless it is empty
                if (!(tvram[idx] | tvram[0x800 + idx] | tvram[0x1000 + idx]))
                    idx = (idx ^ 0x400) & 0x7FF;
            }
            char tmp[4];
            const size_t len = decode_cell(idx, tmp);
            if (n + len + 2 > cap) { buf[n] = '\0'; return n; }
            for (size_t i = 0; i < len; i++) buf[n++] = tmp[i];
        }
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return n;
}

} // namespace mz
