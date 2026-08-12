// Display-window vocabulary shared by the frame composer and the VRAM wait
// model. Both have to answer the same question - is this layer being drawn
// on this raster? - and they have to answer it the same way: the composer
// decides what a screenshot shows, and the wait model decides whether the
// controller is holding the VRAM bus while the CPU asks for it.
#pragma once

#include <cstdint>

namespace mz {

// The CRTC's two text display windows - vertical (03h SL / 05h EL) and
// horizontal (07h SC / 08h EC) - follow one rule, spelled out in Oh!MZ's
// hardware analysis (tmp/MZ2500_SuperMZ_Magazine.pdf p203, the CRTC
// internal-register table). Its remark against 03h/05h reads: with
// register 3 < register 5 a blank forms at the top and bottom of the
// screen and the picture in the middle; the reverse puts the blank in the
// middle and the picture at the top and bottom; with register 3 =
// register 5 the whole screen goes blank. The blanked part is transparent
// whatever CRTC 00h bit1 says. The remark against 07h/08h then says the
// blank forms the same way as for registers 3 and 5, that the blanked
// part counts as palette 0, and that writing an out-of-range value has no
// effect and never enlarges the display period.
//
// So the pair is a start/end pair on a counter that wraps once per frame
// (or per line): "inside" when start < end, "outside" when start > end,
// and nothing at all when they are equal. Reading the equal case as a
// window nobody programmed - which is what comparing the two values
// alone amounts to - flashes a whole hidden text screen for one frame
// every time a program shuts the window before filling the text planes.
enum class WinKind { All, None, Inside, Outside };

// `written` distinguishes a window programmed shut from one no program
// ever touched: the CRTC's power-on register contents are documented
// nowhere, and core/dummy_ipl.cpp stands in for the firmware without
// modelling its CRTC set-up, so rather than invent a start/end pair the
// renderer leaves the screen whole until a program writes one.
inline WinKind win_kind(bool written, uint8_t start, uint8_t end) {
    if (!written) return WinKind::All;
    if (start == end) return WinKind::None;
    return start < end ? WinKind::Inside : WinKind::Outside;
}

// Membership in screen coordinates. `lo`/`hi` are the start and end
// registers mapped onto the framebuffer, so a bound that falls outside
// the picture simply never matches and the display period cannot grow.
inline bool in_win(WinKind kind, int v, int lo, int hi) {
    switch (kind) {
    case WinKind::All: return true;
    case WinKind::None: return false;
    case WinKind::Inside: return v >= lo && v < hi;
    default: return v >= lo || v < hi;
    }
}

// CRTC registers 03h/05h count in lines of the raw video timing, two
// rasters to a line, with line 17 (11h) the first one displayed - the I/O
// map's standard value for the 400-raster timing. So SL puts the text
// layer's top edge at (SL - 17) * 2 rasters.
constexpr int TEXT_WIN_LINE0 = 17;

// G-CRTC mode register 0Eh: bit7 EX, bit4 4C (clear = 4 colours), bit3
// 256C, bit2 V200, bit1 H640, bit0 PRI.
inline bool gde_v200(uint8_t gmode) { return (gmode & 0x04) != 0; }

// Resolve a G-CRTC plane address to one of the physical 8KB GRAM banks.
// Keeping this mapping shared is important: display fetches, hardware clear,
// and CPU-visible effects must agree on EX and 640x400 bank selection.
inline int gde_plane_bank(int plane, uint32_t address, uint8_t mode) {
    const int quarter = (address >> 13) & 3;
    const bool v200 = gde_v200(mode);
    const bool h640 = (mode & 0x02) != 0;
    const bool cg4 = !(mode & 0x10) && !v200 && h640;

    if (v200 && (mode & 0x80)) return 0x28 + plane * 2 + (quarter & 1);
    if (quarter < 2) return 0x20 + plane * 2 + quarter;
    if (cg4) return 0x24 + plane * 2 + (quarter - 2);
    return 0x28 + plane * 2 + (quarter - 2);
}

// SAD0 addresses the first raster of the scrolling area. After SLN1
// rasters, the controller starts a separate fixed area at SAD2, so its line
// offset restarts at zero rather than continuing to count from the top of
// the screen. SAD1 is the last address in the scroll ring.
inline uint32_t gde_row_address(uint32_t display_line, uint32_t stride,
                                uint32_t sad0, uint32_t sad1, uint32_t sad2,
                                uint32_t sln1) {
    const uint32_t ring = sad1 + 1u;
    if (display_line < sln1)
        return (sad0 + stride * display_line) % ring;
    return (sad2 + stride * (display_line - sln1)) % ring;
}

// Is the graphics layer being displayed at all? Oh!MZ's hardware analysis
// lists the mode-bit combinations that are real modes (15h/14h 320x200x16,
// 1Dh 320x200x256, 17h 640x200x16, 03h 640x400x4, 93h 640x400x16, and the
// EX variants); anything else leaves the layer off.
inline bool gde_cg_on(uint8_t gmode) {
    const bool h640 = (gmode & 0x02) != 0;
    const bool v200 = gde_v200(gmode);
    // 4-colour mode exists so that a machine with no MZ-1R27 can still run
    // 640x400 with two bits per pixel.
    const bool cg4 = !(gmode & 0x10) && !v200 && h640;
    // 256 colours is a 320-wide, 200-line geometry only (modes 1Dh/9Dh).
    const bool cg256 = (gmode & 0x18) == 0x18 && v200 && !h640;
    return (v200 || h640) && ((gmode & 0x10) != 0 || cg4) &&
           (!(gmode & 0x08) || cg256);
}

} // namespace mz
