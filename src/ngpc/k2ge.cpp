// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
// Adapted from MAME's src/mame/snk/k1ge.cpp (K2GE colour path).

#include "k2ge.h"

#include <cstring>

namespace {

constexpr bool bit(uint32_t value, int n) { return (value >> n) & 1; }

} // namespace


void k2ge::reset()
{
	std::memset(vram, 0, sizeof(vram));
	write(0x000, 0x00);   /* Interrupt enable */
	write(0x002, 0x00);   /* WBA.H */
	write(0x003, 0x00);   /* WVA.V */
	write(0x004, 0xff);   /* WSI.H */
	write(0x005, 0xff);   /* WSI.V */
	write(0x007, 0xc6);   /* REF */
	write(0x012, 0x00);   /* 2D control */
	write(0x020, 0x00);   /* PO.H */
	write(0x021, 0x00);   /* PO.V */
	write(0x030, 0x00);   /* PF */
	write(0x032, 0x00);   /* S1SO.H */
	write(0x033, 0x00);   /* S1SO.V */
	write(0x034, 0x00);   /* S2SO.H */
	write(0x035, 0x00);   /* S2SO.V */
	for (uint32_t offset : { 0x101u, 0x102u, 0x103u, 0x105u, 0x106u, 0x107u,
			0x109u, 0x10au, 0x10bu, 0x10du, 0x10eu, 0x10fu,
			0x111u, 0x112u, 0x113u, 0x115u, 0x116u, 0x117u, 0x118u })
		write(offset, 0x07);
	write(0x400, 0xff);   /* LED control */
	write(0x402, 0x80);   /* LEDFREG */
	write(0x7e0, 0x52);   /* RESET */
	write(0x7e2, 0x00);   /* MODE */
	m_wba_h = vram[0x002];
	m_wba_v = vram[0x003];
	m_wsi_h = vram[0x004];
	m_wsi_v = vram[0x005];
}


uint8_t k2ge::read(uint32_t offset) const
{
	offset &= 0x3fff;
	uint8_t data = vram[offset];

	switch (offset)
	{
	case 0x008:     /* RAS.H */
		data = (current_cycle_in_line ? current_cycle_in_line() : 0) >> 2;
		break;
	case 0x009:     /* RAS.V */
		data = current_line;
		break;
	}
	return data;
}


void k2ge::write(uint32_t offset, uint8_t data)
{
	offset &= 0x3fff;

	switch (offset)
	{
	case 0x000:
		if (vblank_pin_w)
			vblank_pin_w(bit(data, 7) ? bit(vram[0x010], 6) : 0);
		break;
	case 0x030:
		data &= 0x80;
		break;
	case 0x100: case 0x101: case 0x102: case 0x103:
	case 0x104: case 0x105: case 0x106: case 0x107:
	case 0x108: case 0x109: case 0x10a: case 0x10b:
	case 0x10c: case 0x10d: case 0x10e: case 0x10f:
	case 0x110: case 0x111: case 0x112: case 0x113:
	case 0x114: case 0x115: case 0x116: case 0x117:
		data &= 0x07;
		break;
	case 0x7e2:
		if (vram[0x7f0] != 0xaa)
			return;
		data &= 0x80;
		break;
	}

	/* Only the lower 4 bits of the palette entry high bytes can be written */
	if (offset >= 0x0200 && offset < 0x0400 && bit(offset, 0))
		data &= 0x0f;

	vram[offset] = data;
}


void k2ge::draw(int line)
{
	using namespace k2ge_render;

	LineRegs regs;
	regs.po_h = vram[0x020];
	regs.po_v = vram[0x021];
	regs.s1x = vram[0x032];
	regs.s1y = vram[0x033];
	regs.s2x = vram[0x034];
	regs.s2y = vram[0x035];
	regs.pf = vram[0x030];
	regs.bg = vram[0x118];
	regs.oow = vram[0x012];
	regs.wba_h = m_wba_h;
	regs.wba_v = m_wba_v;
	regs.wsi_h = m_wsi_h;
	regs.wsi_v = m_wsi_v;

	// Record the line for the host; the palette is kept once per change.
	if (line == 0)
		m_capture.palettes.clear();
	m_capture.line[line] = regs;
	Palette palette;
	std::memcpy(palette.data(), &vram[0x200], palette.size());
	if (m_capture.palettes.empty() || m_capture.palettes.back() != palette)
		m_capture.palettes.push_back(palette);
	m_capture.palette_index[line] = static_cast<uint8_t>(m_capture.palettes.size() - 1);
	if (line == SCREEN_HEIGHT - 1)
		std::memcpy(m_capture.vram.data(), vram, sizeof(vram));

	View view;
	view.width = SCREEN_WIDTH;
	render_line(vram, &vram[0x200], regs, line, view, nullptr, &framebuffer[line * SCREEN_WIDTH]);
}


void k2ge::begin_line(int y)
{
	current_line = y;

	/* Check for start of VBlank */
	if (y >= SCREEN_HEIGHT)
	{
		vram[0x010] |= 0x40;
		if (bit(vram[0x000], 7) && vblank_pin_w)
			vblank_pin_w(1);
	}

	/* Check for end of VBlank */
	if (y == 0)
	{
		m_wba_h = vram[0x002];
		m_wba_v = vram[0x003];
		m_wsi_h = vram[0x004];
		m_wsi_v = vram[0x005];
		vram[0x010] &= ~0x40;
		if (bit(vram[0x000], 7) && vblank_pin_w)
			vblank_pin_w(0);
	}

	/* Check if Hint should be triggered */
	if (y == LINES_PER_FRAME - 1 || y < 151)
	{
		if (bit(vram[0x000], 6) && hblank_pin_w)
			hblank_pin_w(1);
	}

	/* Draw a line when inside visible area */
	if (y && y < SCREEN_HEIGHT + 1)
		draw(y - 1);
}


void k2ge::end_hblank()
{
	if (hblank_pin_w)
		hblank_pin_w(0);
}
