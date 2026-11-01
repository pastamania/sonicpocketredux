// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
// K2GE (Neo Geo Pocket Color) graphics, adapted from MAME's
// src/mame/snk/k1ge.{h,cpp}. Colour mode only; the monochrome K1GE
// compatibility palette is not implemented because Sonic Pocket
// Adventure is a colour-only title.
#pragma once

#include <cstdint>
#include <functional>

#include "ngpc/k2ge_render.h"

class k2ge
{
public:
	static constexpr int SCREEN_WIDTH = 160;
	static constexpr int SCREEN_HEIGHT = 152;
	static constexpr int LINES_PER_FRAME = 199;
	static constexpr int CYCLES_PER_LINE = 515;
	static constexpr int HBLANK_END_CYCLE = 480;

	void reset();

	uint8_t read(uint32_t offset) const;
	void write(uint32_t offset, uint8_t data);

	// Line timing, driven by the machine. begin_line() runs at the start of
	// every line (0..198) and end_hblank() at cycle 480 of lines that raised
	// the HBlank pin.
	void begin_line(int line);
	void end_hblank();

	// Current beam position, supplied by the machine for RAS.H/RAS.V reads.
	std::function<int()> current_cycle_in_line;
	int current_line = 0;

	std::function<void(int)> vblank_pin_w;
	std::function<void(int)> hblank_pin_w;

	// Finished frame, 0xAARRGGBB.
	uint32_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT] = {};

	uint8_t vram[0x4000] = {};

	// The last frame drawn, with the registers and palettes each line used.
	// Complete once the frame has ended (machine on_frame_end).
	const k2ge_render::Frame &frame() const { return m_capture; }

private:
	void draw(int line);

	k2ge_render::Frame m_capture;
	uint8_t m_wba_h = 0;
	uint8_t m_wba_v = 0;
	uint8_t m_wsi_h = 0xff;
	uint8_t m_wsi_v = 0xff;
};
