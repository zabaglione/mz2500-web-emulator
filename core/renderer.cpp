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

void Mz2500::render(uint8_t* rgba) const {
    // --- resolve the 16-colour output palette -------------------------------
    // OPN GPIO port A (register 0Eh) bit2: 0 selects the MZ-1M10 RGB444
    // palette, 1 the digital palette (used during FDC access windows).
    // A machine with no palette board - and one whose board has never been
    // programmed, like BASIC-M25, which drives the digital palette only -
    // shows the fixed IGRB colours.
    const bool rgb444 =
        mz1m10_present_ && palette_written_ && (opn_regs_[0x0E] & 0x04) == 0;
    uint8_t pal_r[16], pal_g[16], pal_b[16];
    for (int i = 0; i < 16; i++) {
        if (rgb444 && i != 0) {
            // nibble << 4 expansion, matching EmuZ output byte-for-byte so
            // screenshot regressions can demand exact equality. Colour 0 is
            // wired to black on the MZ-1M10 and takes no palette entry.
            const uint8_t even = palette_[i * 2];     // (R<<4) | B
            const uint8_t odd = palette_[i * 2 + 1];  // G
            pal_r[i] = static_cast<uint8_t>(((even >> 4) & 0x0F) << 4);
            pal_g[i] = static_cast<uint8_t>((odd & 0x0F) << 4);
            pal_b[i] = static_cast<uint8_t>((even & 0x0F) << 4);
        } else if (rgb444) {
            pal_r[i] = pal_g[i] = pal_b[i] = 0;
        } else if (i == 8) {
            // The fixed 16 colours are 0 and 9-15 at full level, 1-7 at a
            // half level, and colour 8 alone a darker grey - not, as bit3
            // alone would suggest, an "intense black". The firmware's own
            // MZ-1M10 table settles the ordering: it writes component level
            // 7 for colours 1-7, 3 for colour 8 and 15 for 9-15, so 8 sits
            // below the half level. (EmuZ-2500 renders it at 152, above the
            // half level, which contradicts that table.)
            pal_r[i] = pal_g[i] = pal_b[i] = 95;
        } else {
            // digital palette: bit3 = intensity, bit2..0 = G/R/B.
            // Half level measured off EmuZ's BASIC-M25 screen (127, not 160).
            const int hi = (i & 0x08) ? 255 : 127;
            pal_g[i] = (i & 0x04) ? hi : 0;
            pal_r[i] = (i & 0x02) ? hi : 0;
            pal_b[i] = (i & 0x01) ? hi : 0;
        }
    }

    // --- CG layer state ------------------------------------------------------
    // Mode register 0Eh. The I/O map names the bits: bit7 EX (fetch the
    // display from the expansion VRAM, ignored in 640x400), bit4 4C (clear
    // = 4 colours), bit3 256C, bit2 V200, bit1 H640, bit0 PRI (which of two
    // superimposed screens is in front). Oh!MZ's hardware analysis lists
    // the combinations that are real modes:
    //   15h/14h 320x200x16   1Dh 320x200x256   17h 640x200x16
    //   03h 640x400x4        93h 640x400x16    (95h/94h/9Dh/97h: EX set)
    // No listed mode is 400 lines at 320 dots, and EmuZ blanks the screen
    // for one, so 400 lines implies 640 dots here too.
    const uint8_t gmode = gde_regs_[0x0E];
    const bool h640 = (gmode & 0x02) != 0;
    const bool v200 = gde_v200(gmode);
    // 4-colour mode exists so that a machine with no MZ-1R27 can still run
    // 640x400: the missing expansion B and R planes are replaced by the
    // standard G and I planes, and the picture keeps only 2 bits per pixel.
    const bool cg4 = !(gmode & 0x10) && !v200 && h640;
    // 256 colours is a 320-wide, 200-line geometry only (modes 1Dh/9Dh).
    const bool cg256 = (gmode & 0x18) == 0x18 && v200 && !h640;
    // 320x200 16 colours is the one mode that carries two whole screens in
    // the standard VRAM and can show them at once: Oh!MZ's capability table
    // lists it as "2 pages (superimposable)" where 640x200 and 640x400 get
    // one, and the I/O map's PRI bit (0Eh bit0) is documented as the
    // priority "when superimposing two screens (320x200 16-colour mode
    // only)". The second screen sits one 8KB screen above the first, which
    // is the same pairing 256-colour mode uses for its extra four planes.
    // PRI 1 puts the lower-numbered screen in front, 0 puts it behind.
    const bool cg2page = !h640 && v200 && !cg256 && (gmode & 0x10) != 0;
    const bool page0_front = (gmode & 0x01) != 0;
    const bool cg_on = gde_cg_on(gmode); // core/gcrtc.h - the wait model agrees
    const uint16_t gdevs = reg16(gde_regs_, 0x08);
    const uint16_t gdeve = reg16(gde_regs_, 0x0A);
    const int win_unit = h640 ? 8 : 4; // window regs step 8 dots at 640 wide
    const int cg_stride = h640 ? 80 : 40;
    const int hdsc = gde_regs_[0x0F] & 7;
    const uint16_t sad0 = reg16(gde_regs_, 0x10);
    const uint16_t sad1 = reg16(gde_regs_, 0x12);
    const uint16_t sad2 = reg16(gde_regs_, 0x14);
    const uint16_t sln1 = reg16(gde_regs_, 0x16);
    const uint8_t plane_mask = gde_regs_[0x18] & (cg4 ? 0x03 : 0x0F);
    // Register 18h is "I1 G1 R1 B1 I0 G0 R0 B0", one enable per displayed
    // plane: the lower nibble is the first screen's four planes and the
    // upper nibble the second screen's. 256-colour mode spends the second
    // screen's planes on the extra bit per gun; 320x200 16-colour shows it
    // as a second picture behind or in front of the first.
    const uint8_t plane_mask2 = (uint8_t)((gde_regs_[0x18] >> 4) & 0x0F);
    const uint32_t ring = sad1 + 1u;
    // The controller sees one linear 32KB space per plane: 0000-3FFF in the
    // standard VRAM (banks 20h-27h, two per plane) and 4000-7FFF in the
    // expansion VRAM (28h-2Fh). 400-line modes reach into the second half.
    // In 4-colour mode there is no expansion VRAM to reach, so the standard
    // G and I banks stand in as the upper halves of the B and R planes.
    // EmuZ-2500 reads the expansion banks here instead, which would leave a
    // program written for an unexpanded machine with a blank lower half -
    // and that machine is the only reason the mode exists.
    const uint8_t* plane_bank[4][4];
    for (int p = 0; p < 4; p++) {
        const int hi = cg4 ? (0x24 + p * 2) : (0x28 + p * 2);
        plane_bank[p][0] = mem_.bank_ptr(0x20 + p * 2);
        plane_bank[p][1] = mem_.bank_ptr(0x21 + p * 2);
        plane_bank[p][2] = mem_.bank_ptr(hi);
        plane_bank[p][3] = mem_.bank_ptr(hi + 1);
    }

    // --- text layer state ----------------------------------------------------
    const bool text80 = (pio_a_ & 0x20) != 0;
    const uint8_t* tvram = mem_.bank_ptr(0x38);
    const uint8_t* pcg = mem_.bank_ptr(0x39);
    // Text2 plane (tvram +1000h) K=1 routes a cell to the kanji ROM font:
    // glyph byte address = L<<17 | A[16:11]<<11 | Text1<<3, one byte per
    // scanline, 16 lines per cell, address bit3 ignored (I/O map, block 38h)
    const std::vector<uint8_t>& krom = mem_.kanji_rom();
    const uint32_t kmask = krom.empty() ? 0 : (uint32_t)krom.size() - 1;
    // CRTC text roll: regs 01h/02h = display start address in cells, reg 09h
    // = display start line (2-raster units). BASIC's console scrolls with a
    // 7-step fine scroll and then advances the start address one row. The
    // address counter is 11 bits and wraps at the end of the Text1 plane
    // (2048 cells), not at the end of the 25 displayed rows - so a rolled
    // screen runs through the 48 cells past row 24 before coming back to
    // zero, which is exactly where BASIC writes its scrolled rows.
    const int text_cols = (pio_a_ & 0x20) ? 80 : 40;
    const int text_sa = ((crtc_regs_[2] << 8) | crtc_regs_[1]) & 0x7FF;
    const int text_vd = (crtc_regs_[9] & 0x0F) * 2;
    // Vertical text window: reg 03h = first displayed line, reg 05h = first
    // blanked line, both in 2-raster units. BASIC-M25 sets 11h/D9h for its
    // 25 rows and drops reg 05h to D1h while smooth-scrolling, which hides
    // the row the fine scroll is feeding in. Outside the window the text
    // layer is blank and the graphics layer shows through.
    // Attribute bit7 blinks a cell at about 1Hz; bit1 of CRTC reg 00h turns
    // the text layer's transparent parts into opaque black.
    const bool blink_phase = ((frames_ / 28) & 1) == 0;
    // Port F6h gates the graphic layer's guns. In 16-colour mode the mask
    // lands on the IGRB the palette produced - bit0 (BE) takes the I plane
    // with the blue - and in 256-colour mode on the final output.
    const bool gun_g = (cg_mask_ & 0x04) != 0;
    const bool gun_r = (cg_mask_ & 0x02) != 0;
    const bool gun_b = (cg_mask_ & 0x01) != 0;
    const bool text_black_bg = (crtc_regs_[0x00] & 0x02) != 0;
    // 40-column mode carries two text screens, the second at display address
    // xor 400h. CRTC reg 00h bits 3-2 pick between them: 00 composes their
    // colours into 64, 01 shows the first alone (and is what 80 columns
    // uses), 10 the second alone, 11 lays one over the other in 8 colours.
    // Reg 00h bit0 clear is the 64-colour text / 256-colour graphics pairing;
    // with it set the text layer keeps its 8 colours through the CLUT.
    static const int PAGE_SELECT[4] = {3, 1, 2, 3}; // both, first, second, both
    const int text_pages =
        text80 ? 1 : PAGE_SELECT[(crtc_regs_[0x00] >> 2) & 3];
    const bool text64 = (crtc_regs_[0x00] & 0x01) == 0;
    // Vertical text window: reg 03h (SL) is the first displayed line and reg
    // 05h (EL) the first blanked one, both counted in lines of the raw video
    // timing - the same "from the start of the raw frame, not from the top of
    // the picture" convention as the horizontal pair below. The I/O map's
    // standard values for the 400-raster timing this renderer produces are
    // 17 (11h) and 217 (D9h) (38h/FEh belong to the 200-raster timing), and
    // one line of that timing is two rasters here, so a programmed SL moves
    // the text layer's top edge down by (SL - 17) * 2 rasters and EL blanks
    // it again at (EL - 17) * 2. Anchoring the window at SL is what lets a
    // title park a text panel partway down the screen; EL at SL closes the
    // window completely, which is how the same title keeps its message
    // hidden while it fills the text planes. See win_kind() in core/gcrtc.h
    // for the three cases and for what an unwritten pair means - the VRAM
    // wait model reads the same window off the same registers.
    const WinKind vwin = text_vwin_kind();
    const int text_y0 = (crtc_regs_[3] - TEXT_WIN_LINE0) * 2;
    const int text_y1 = (crtc_regs_[5] - TEXT_WIN_LINE0) * 2;
    // Horizontal text window: reg 07h (SC) is the first displayed digit and
    // reg 08h (EC) the first blanked one, both counted in 8-dot digits of
    // the raw video timing rather than from the left edge of the picture.
    // The I/O map's standard values are the ones that put text column 0 at
    // the left edge of the active area - 9 at 80 digits and 8 at 40, for the
    // 400-raster timing this renderer produces (11 and 10 belong to the
    // 200-raster timing, which never reaches the framebuffer here) - so a
    // programmed SC displaces the text layer's left edge by (SC - standard)
    // digits and EC blanks it again (EC - standard) digits in. A title that
    // narrows the window this way is reserving the digits it gives up for
    // the graphics layer, which keeps its own window in GDEHS/GDEHE.
    const int text_digit0 = text80 ? 9 : 8;
    const WinKind hwin =
        win_kind(crtc_hwin_written_, crtc_regs_[7], crtc_regs_[8]);
    const int text_x0 = (crtc_regs_[7] - text_digit0) * 8;
    const int text_x1 = (crtc_regs_[8] - text_digit0) * 8;
    // Background colour (CRTC regs 0Bh/0Ch): in 16-colour mode one bit per
    // component - I from 0Bh bit0, B from 0Bh bit2, R from 0Bh bit5, G from
    // 0Ch bit0. A graphic pixel whose palette entry is 0 shows this instead.
    const int bg_colour = ((crtc_regs_[0x0B] & 0x01) << 3) |
                          ((crtc_regs_[0x0C] & 0x01) << 2) |
                          (((crtc_regs_[0x0B] >> 5) & 1) << 1) |
                          ((crtc_regs_[0x0B] >> 2) & 1);
    // The same two registers carry a full 9-bit colour for 256-colour mode:
    // 0Bh is G1 G0 R2 R1 R0 B2 B1 B0 and 0Ch bit0 is G2.
    const int bg_rgb = (((crtc_regs_[0x0B] >> 3) & 0x07) << 6) |
                       ((((crtc_regs_[0x0C] & 1) << 2) |
                         ((crtc_regs_[0x0B] >> 6) & 0x03)) << 3) |
                       (crtc_regs_[0x0B] & 0x07);

    for (int y = 0; y < 400; y++) {
        // GDEVS/GDEVE/SLN1 count display lines, so a 400-line mode addresses
        // every raster and a 200-line mode doubles each one.
        const int gy = v200 ? (y >> 1) : y;
        uint8_t* row = rgba + (size_t)y * 640 * 4;

        // GDEHS/GDEHE as they stood while this raster was scanned. The
        // 24 kHz frame displays VBLANK_START_LINE = 400 of its 448 lines and
        // this framebuffer is 400 rasters, so the two are the same raster:
        // one entry of hwin_line_ per row, which is the only ratio at which
        // a program that rewrites the window every raster keeps its shape.
        static_assert(VBLANK_START_LINE == 400,
                      "the framebuffer is one raster per displayed line");
        const int win_x0 = hwin_line_[y][0] * win_unit;
        const int win_x1 = hwin_line_[y][1] * win_unit;

        // CG source row
        const uint8_t* cg_valid = nullptr;
        uint32_t cg_base = 0;
        if (cg_on && gy >= gdevs && gy < gdeve) {
            cg_base = ((gy < sln1 ? sad0 : sad2) + (uint32_t)cg_stride * gy) % ring;
            cg_valid = reinterpret_cast<const uint8_t*>(1); // marker
        }

        // Font height (port F7h bit0). In 16-line mode a cell is a full 16
        // rasters of glyph: Text1 bit0 = address A[3] is ignored, so a cell
        // reads 16 consecutive bytes from the even code - which is exactly
        // how BASIC-M25 lays out its logo and text (even codes only). In
        // 8-line mode a cell carries 8 glyph lines, double-scanned, and A[3]
        // selects the half - the layout NEKO CAN RUN's PCG art uses.
        const bool text16 = (font_size_ & 1) == 0;
        const int ty = y + text_vd; // text raster after the fine roll
        // CRTC reg 00h bit4 selects 20 rows instead of 25: the cell grows to
        // 20 rasters, the extra 4 below the glyph carrying the attribute but
        // no pixels.
        const bool rows20 = (crtc_regs_[0x00] & 0x10) != 0;
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
                        const bool wins = !text_on || (crtc_regs_[0x80 + c] & 0x10);
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
                            const int pal0 = crtc_regs_[0x80 + c] & 0x0F;
                            if (wins) {
                                rgb = (pal0 == 0 && c1 == 0)
                                          ? bg_rgb
                                          : compose256(pal0, c1, crtc_regs_[0x0A]);
                                // Port F6h gates the final 256-colour output.
                                if (!gun_r) rgb &= ~0x1C0;
                                if (!gun_g) rgb &= ~0x038;
                                if (!gun_b) rgb &= ~0x007;
                            }
                        } else {
                            // A graphic pixel that lands on palette code 0
                            // shows the background colour, not plain black.
                            const int mapped =
                                (crtc_regs_[0x80 + c] & 0x0F) ? c : bg_colour;
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
            int n = from_graphics ? (crtc_regs_[0x80 + color] & 0x0F) : color;
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

} // namespace mz
