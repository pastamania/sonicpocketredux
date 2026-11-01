// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
// Adapted from MAME's src/mame/snk/k1ge.cpp (K2GE colour path).

#include "k2ge_render.h"

#include <cstring>

namespace k2ge_render {

const char *aspect_name(Aspect aspect)
{
	return aspect == Aspect::Wide16x10 ? "16:10" : aspect == Aspect::Wide16x9 ? "16:9" : "original";
}

bool parse_aspect(const char *text, Aspect &aspect)
{
	if (!std::strcmp(text, "original") || !std::strcmp(text, "stock"))
		aspect = Aspect::Original;
	else if (!std::strcmp(text, "16:10") || !std::strcmp(text, "16x10"))
		aspect = Aspect::Wide16x10;
	else if (!std::strcmp(text, "16:9") || !std::strcmp(text, "16x9"))
		aspect = Aspect::Wide16x9;
	else
		return false;
	return true;
}

Aspect next_aspect(Aspect aspect)
{
	return static_cast<Aspect>((static_cast<int>(aspect) + 1) % ASPECT_COUNT);
}

View aspect_view(Aspect aspect)
{
	View view;
	view.width = aspect_width(aspect);
	view.origin = -((view.width - 160) / 2);
	view.card_origin = view.origin;
	view.screen_only = view.width > 160;
	return view;
}

namespace {

constexpr bool bit(uint32_t value, int n) { return (value >> n) & 1; }

inline void get_tile_addr(uint16_t data, bool &hflip, uint16_t &tile_addr)
{
	hflip = bit(data, 15);
	tile_addr = 0x2000 + ((data & 0x1ff) * 16);
}

inline uint16_t get_pixel(bool hflip, uint16_t &tile_data)
{
	uint16_t col;
	if (hflip)
	{
		col = tile_data & 0x0003;
		tile_data >>= 2;
	}
	else
	{
		col = tile_data >> 14;
		tile_data <<= 2;
	}
	return col;
}

inline uint16_t word(const uint8_t *vram, int offset)
{
	return vram[offset] | (vram[offset + 1] << 8);
}

// Pens 0..191 index palette RAM directly (sprites, plane 1, plane 2);
// 0xf0..0xf7 are background colours and 0xf8..0xff window colours.
inline uint32_t pen_to_argb(const uint8_t *palette, uint16_t pen)
{
	const uint16_t value = palette[pen * 2] | (palette[pen * 2 + 1] << 8);
	const uint32_t r = (value & 0xf) * 0x11;
	const uint32_t g = ((value >> 4) & 0xf) * 0x11;
	const uint32_t b = ((value >> 8) & 0xf) * 0x11;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

void get_tile_data_for(const uint8_t *vram, uint16_t map_data, int line, int scroll_y, int pal_base,
		uint16_t &pcode, bool &hflip, uint16_t &tile_data)
{
	pcode = pal_base + ((map_data & 0x1e00) >> 7);
	uint16_t tile_addr;
	get_tile_addr(map_data, hflip, tile_addr);
	if (bit(map_data, 14))
		tile_addr += (7 - ((scroll_y + line) & 0x07)) * 2;
	else
		tile_addr += ((scroll_y + line) & 0x07) * 2;
	tile_data = word(vram, tile_addr);
}

void get_tile_data(const uint8_t *vram, int offset_x, uint16_t base, int line, int scroll_y, int pal_base,
		uint16_t &pcode, bool &hflip, uint16_t &tile_data)
{
	get_tile_data_for(vram, word(vram, base + offset_x), line, scroll_y, pal_base, pcode, hflip, tile_data);
}

void draw_scroll_plane(const uint8_t *vram, uint16_t *p, const View &view, uint16_t base, int line,
		int scroll_x, int scroll_y, int pal_base, int clip_lo = 0, int clip_hi = MAX_WIDTH)
{
	const int start_x = (scroll_x + view.origin) & 0xff;
	int offset_x = (start_x >> 3) * 2;
	int px = start_x & 0x07;

	base += (((scroll_y + line) >> 3) & 0x1f) * 0x0040;

	uint16_t pcode, tile_data;
	bool hflip;
	get_tile_data(vram, offset_x, base, line, scroll_y, pal_base, pcode, hflip, tile_data);
	if (hflip)
		tile_data >>= 2 * px;
	else
		tile_data <<= 2 * px;

	for (int i = 0; i < view.width; i++)
	{
		const uint16_t col = get_pixel(hflip, tile_data);
		if (col && i >= clip_lo && i < clip_hi)
			p[i] = pcode + col;

		px++;
		if (px >= 8)
		{
			offset_x = (offset_x + 2) & 0x3f;
			get_tile_data(vram, offset_x, base, line, scroll_y, pal_base, pcode, hflip, tile_data);
			px = 0;
		}
	}
}

// The same plane, drawn from a wide scene: `world_x` is the world x of screen
// x 0, and the tile for each column comes from the scene, not the hardware map.
void draw_wide_plane(const uint8_t *vram, uint16_t *p, const View &view, const WideScene &wide, int plane,
		int line, int world_x, int scroll_y, int pal_base)
{
	const uint16_t *row = wide.entry[plane][((scroll_y + line) >> 3) & 0x1f];
	const int start = world_x + view.origin;         // world x of output column 0
	int col = start >> 3;                            // floor, whatever the sign
	int px = start & 7;

	uint16_t pcode = 0, tile_data = 0;
	bool hflip = false, have = false;
	auto fetch = [&]() {
		const int index = col - wide.first_col;
		have = index >= 0 && index < WIDE_COLS;
		if (have)
			get_tile_data_for(vram, row[index], line, scroll_y, pal_base, pcode, hflip, tile_data);
	};
	fetch();
	if (have)
	{
		if (hflip)
			tile_data >>= 2 * px;
		else
			tile_data <<= 2 * px;
	}

	for (int i = 0; i < view.width; i++)
	{
		if (have)
		{
			const uint16_t pixel = get_pixel(hflip, tile_data);
			if (pixel)
				p[i] = pcode + pixel;
		}
		if (++px >= 8)
		{
			++col;
			fetch();
			px = 0;
		}
	}
}

void draw_sprite_plane(const uint8_t *vram, uint16_t *p, const View &view, uint16_t priority, int line,
		const LineRegs &regs, const SpriteShift *shift)
{
	struct sprite_t
	{
		uint16_t spr_data;
		int x;
		uint8_t y;
		uint8_t index;
	};
	sprite_t spr[SPRITES];
	int num_sprites = 0;
	uint8_t spr_y = 0;
	uint8_t spr_x = 0;

	priority <<= 11;

	/* Select sprites */
	int ref[SPRITES];     // unwrapped x of each selected sprite, or NO_REF
	for (int i = 0; i < 256; i += 4)
	{
		const uint16_t spr_data = word(vram, 0x800 + i);
		const uint8_t x = vram[0x802 + i];
		const uint8_t y = vram[0x803 + i];

		spr_x = bit(spr_data, 10) ? (spr_x + x) : (regs.po_h + x);
		spr_y = bit(spr_data, 9) ? (spr_y + y) : (regs.po_v + y);

		if ((spr_data & 0x1800) == priority)
		{
			const int index = i >> 2;
			const int sx = spr_x + (shift ? shift->dx[index] : 0);
			const uint8_t sy = static_cast<uint8_t>(spr_y + (shift ? shift->dy[index] : 0));
			if ((line >= sy || sy > 0xf8) && line < ((sy + 8) & 0xff))
			{
				spr[num_sprites].spr_data = spr_data;
				spr[num_sprites].y = sy;
				spr[num_sprites].x = sx;
				spr[num_sprites].index = index;
				// Placed relative to its object, if it has one: the object's x
				// plus how far the sprite is from it, whichever lap of 256 that is.
				ref[num_sprites] = SpriteShift::NO_REF;
				if (shift && shift->ref_x[index] != SpriteShift::NO_REF)
				{
					const int object_x = shift->ref_x[index];
					int offset = (spr_x - object_x) & 0xff;
					if (offset >= 128)
						offset -= 256;
					ref[num_sprites] = object_x + shift->dx[index] + offset;
				}
				num_sprites++;
			}
		}
	}

	/* Draw sprites */
	for (int i = num_sprites - 1; i >= 0; i--)
	{
		const uint16_t pcode = (vram[0x0c00 + spr[i].index] & 0x0f) << 2;
		const int origin = shift && shift->card[spr[i].index] ? view.card_origin : view.origin;

		bool hflip;
		uint16_t tile_addr;
		get_tile_addr(spr[i].spr_data, hflip, tile_addr);
		if (bit(spr[i].spr_data, 14))
			tile_addr += (7 - ((line - spr[i].y) & 0x07)) * 2;
		else
			tile_addr += ((line - spr[i].y) & 0x07) * 2;
		uint16_t tile_data = word(vram, tile_addr);

		for (int j = 0; j < 8; j++)
		{
			// A sprite of a game object is placed by its object. Any other is
			// where the stock screen would show it, and nowhere else: its own x
			// wraps at 256, which in a wide view would put it somewhere else.
			int out_x;
			if (ref[i] != SpriteShift::NO_REF)
				out_x = ref[i] + j - origin;
			else if (view.width > 160 && ((spr[i].x + j) & 0xff) >= 160)
				continue;
			else if (view.width > 160)
				out_x = ((spr[i].x + j) & 0xff) - origin;
			else
				out_x = (spr[i].x + j - origin) & 0xff;

			const uint16_t col = get_pixel(hflip, tile_data);
			if (out_x >= 0 && out_x < view.width && col)
				p[out_x] = pcode + col;
		}
	}
}

} // namespace


void render_line(const uint8_t *vram, const uint8_t *palette, const LineRegs &regs, int line,
		const View &view, const SpriteShift *shift, uint32_t *out, const WideScene *wide)
{
	uint16_t p[MAX_WIDTH];
	const bool use_wide = wide && wide->active;
	auto plane1 = [&]() {
		if (use_wide && wide->plane1)
			draw_wide_plane(vram, p, view, *wide, 0, line, regs.wx1, regs.s1y, 0x40);
		else if (use_wide && wide->p1_window)
			draw_scroll_plane(vram, p, view, 0x1000, line, regs.s1x, regs.s1y, 0x40,
					regs.p1_lo - view.origin, regs.p1_lo - view.origin + 256);
		else if (use_wide && wide->centred)
		{
			// The card, in the middle of the view (and only its own 160 pixels
			// until it starts to move).
			View card = view;
			card.origin = view.card_origin;
			const int lo = wide->card_hold ? -card.origin : 0;
			const int hi = wide->card_hold ? 160 - card.origin : MAX_WIDTH;
			draw_scroll_plane(vram, p, card, 0x1000, line, regs.s1x, regs.s1y, 0x40, lo, hi);
		}
		else
			draw_scroll_plane(vram, p, view, 0x1000, line, regs.s1x, regs.s1y, 0x40);
	};
	auto plane2 = [&]() {
		if (use_wide)
			draw_wide_plane(vram, p, view, *wide, 1, line, regs.wx2, regs.s2y, 0x80);
		else
			draw_scroll_plane(vram, p, view, 0x1800, line, regs.s2x, regs.s2y, 0x80);
	};
	const uint16_t oowcol = 0xf8 + (regs.oow & 7);

	if (line < regs.wba_v || line >= regs.wba_v + regs.wsi_v)
	{
		for (int i = 0; i < view.width; i++)
			p[i] = oowcol;
	}
	else
	{
		const uint16_t bgcol = 0xf0 + (((regs.bg & 0xc0) == 0x80) ? (regs.bg & 7) : 0);
		for (int i = 0; i < view.width; i++)
			p[i] = bgcol;

		if (bit(regs.pf, 7))
		{
			draw_sprite_plane(vram, p, view, 1, line, regs, shift);
			plane1();
			draw_sprite_plane(vram, p, view, 2, line, regs, shift);
			plane2();
			draw_sprite_plane(vram, p, view, 3, line, regs, shift);
		}
		else
		{
			draw_sprite_plane(vram, p, view, 1, line, regs, shift);
			plane2();
			draw_sprite_plane(vram, p, view, 2, line, regs, shift);
			plane1();
			draw_sprite_plane(vram, p, view, 3, line, regs, shift);
		}

		// A window as wide as the screen clips nothing, so the bands a wide
		// view adds beyond the screen are left alone.
		if (!(regs.wba_h == 0 && regs.wsi_h >= 160))
		{
			for (int i = 0; i < view.width; i++)
			{
				const int sx = i + view.origin;
				if (sx < regs.wba_h || sx >= regs.wba_h + regs.wsi_h)
					p[i] = oowcol;
			}
		}
	}

	for (int i = 0; i < view.width; i++)
	{
		const int sx = i + view.origin;
		bool black = view.screen_only && (sx < 0 || sx >= 160);
		if (use_wide)
		{
			const int world_x = regs.wx2 + sx;
			black = world_x < wide->min_x || world_x >= wide->max_x;
		}
		out[i] = black ? 0xff000000u : pen_to_argb(palette, p[i]);
	}
}


void render_frame(const Frame &frame, const LineRegs *regs, const View &view,
		const SpriteShift *shift, uint32_t *out, const WideScene *wide)
{
	for (int y = 0; y < LINES; y++)
	{
		const LineRegs &r = regs ? regs[y] : frame.line[y];
		render_line(frame.vram.data(), frame.palette(y).data(), r, y, view, shift, out + y * view.width, wide);
	}
}

} // namespace k2ge_render
