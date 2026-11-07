// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
// Adapted from MAME's src/mame/snk/k1ge.cpp (K2GE colour path).

#include "k2ge_render.h"

#include <algorithm>
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

// `p` holds `scale` samples to each pixel of the view (1, or SUBPIXELS when sprites are
// placed to a fraction of a pixel with SpriteShift::sub).
void draw_sprite_plane(const uint8_t *vram, uint16_t *p, const View &view, uint16_t priority, int line,
		const LineRegs &regs, const SpriteShift *shift, int scale = 1)
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
			if (out_x < 0 || out_x >= view.width || !col)
				continue;
			if (scale == 1)
				p[out_x] = pcode + col;
			else
			{
				// A sprite that is not an object's stays inside the stock screen.
				const int end = (ref[i] == SpriteShift::NO_REF && view.width > 160 ? 160 - view.origin : view.width) * scale;
				const int first = out_x * scale + (shift ? shift->sub[spr[i].index] : 0);
				for (int m = 0; m < scale && first + m < end; m++)
					p[first + m] = pcode + col;
			}
		}
	}
}

// Plane 1 and plane 2 of a line, as the view and the wide scene say, into `p` (which holds
// a pen for each pixel of `view`, or leaves it alone where the plane is clear).
void draw_plane1(const uint8_t *vram, uint16_t *p, const View &view, const LineRegs &regs, int line,
		const WideScene *wide)
{
	const bool use_wide = wide && wide->active;
	if (use_wide && wide->plane1 && !wide->ring)
		draw_wide_plane(vram, p, view, *wide, 0, line, regs.wx1, regs.s1y, 0x40);
	else if (use_wide && wide->p1_window)
		draw_scroll_plane(vram, p, view, 0x1000, line, regs.s1x, regs.s1y, 0x40,
				regs.p1_lo - view.origin, regs.p1_lo - view.origin + 256);
	else if (use_wide && wide->centred)
	{
		// The card, in the middle of the view. Its text lives in the ring
		// beyond the screen's right edge, slides in from there and leaves
		// there again. The ring is 256 pixels round and the view up to 270, so
		// drawn all round the ring would show the part beyond the right edge
		// again at the far left: one lap is drawn, from the screen's left
		// edge, and only the screen's own 160 pixels until the card moves.
		View card = view;
		card.origin = view.card_origin;
		const int lo = -card.origin;
		const int hi = lo + (wide->card_hold ? 160 : 256);
		draw_scroll_plane(vram, p, card, 0x1000, line, regs.s1x, regs.s1y, 0x40, lo, hi);
	}
	else
		draw_scroll_plane(vram, p, view, 0x1000, line, regs.s1x, regs.s1y, 0x40);
}

// Plane 2 on a band line (LineRegs::band): the game's own picture, the pause banner, which it
// keeps in a few of the plane's ring rows and shows by moving the plane's scroll to them. It is
// made for the machine's 160 pixels, so it is drawn from the hardware's ring in the middle of the
// view (View::card_origin), where it is on the screen, and the pixel at each edge of the screen is
// carried on to the view's edges, so the banner goes right across a wider picture. (What lies
// in the ring beside the banner is not drawn: the game keeps no picture there.)
void draw_band_plane(const uint8_t *vram, uint16_t *p, const View &view, const LineRegs &regs, int line)
{
	const int base = 0x1800 + (((regs.s2y + line) >> 3) & 0x1f) * 0x40;
	for (int i = 0; i < view.width; i++)
	{
		const int ring_x = std::clamp(i + view.card_origin, 0, 159) + regs.s2x;
		uint16_t pcode, tile_data;
		bool hflip;
		get_tile_data_for(vram, word(vram, base + ((ring_x >> 3) & 0x1f) * 2), line, regs.s2y, 0x80, pcode, hflip, tile_data);
		const int px = ring_x & 7;
		const uint16_t pixel = hflip ? (tile_data >> (2 * px)) & 3 : (tile_data >> (14 - 2 * px)) & 3;
		if (pixel)
			p[i] = pcode + pixel;
	}
}

void draw_plane2(const uint8_t *vram, uint16_t *p, const View &view, const LineRegs &regs, int line,
		const WideScene *wide)
{
	if (wide && wide->active && regs.band)
		draw_band_plane(vram, p, view, regs, line);
	else if (wide && wide->active && !wide->ring)
		draw_wide_plane(vram, p, view, *wide, 1, line, regs.wx2, regs.s2y, 0x80);
	else
		draw_scroll_plane(vram, p, view, 0x1800, line, regs.s2x, regs.s2y, 0x80);
}

constexpr uint16_t NO_PEN = 0xffff;

// Whether anything on the line is placed to a fraction of a pixel.
bool needs_subpixels(const LineRegs &regs, const SpriteShift *shift)
{
	if (regs.sub1x || regs.sub2x)
		return true;
	if (shift)
		for (int i = 0; i < SPRITES; i++)
			if (shift->sub[i])
				return true;
	return false;
}

// One colour from the SUBPIXELS samples a pixel was drawn with: the average of their colours.
inline uint32_t pixel_from_samples(const uint8_t *palette, const uint16_t *samples)
{
	bool same = true;
	for (int m = 1; m < SUBPIXELS; m++)
		same &= samples[m] == samples[0];
	if (same)
		return pen_to_argb(palette, samples[0]);
	uint32_t r = 0, g = 0, b = 0;
	for (int m = 0; m < SUBPIXELS; m++)
	{
		const uint32_t c = pen_to_argb(palette, samples[m]);
		r += (c >> 16) & 0xff;
		g += (c >> 8) & 0xff;
		b += c & 0xff;
	}
	return 0xff000000u | ((r / SUBPIXELS) << 16) | ((g / SUBPIXELS) << 8) | (b / SUBPIXELS);
}

// The same line as render_line draws, with the scroll planes and the sprites placed to
// 1/SUBPIXELS of a pixel (LineRegs::sub1x, sub2x and SpriteShift::sub). The line is drawn
// with SUBPIXELS samples to a pixel and each pixel takes the average of its samples, so a
// layer that is a fraction of a pixel across shows its edge pixels blended, and one on a
// whole pixel is exactly as render_line draws it.
void render_line_subpixels(const uint8_t *vram, const uint8_t *palette, const LineRegs &regs, int line,
		const View &view, const SpriteShift *shift, uint32_t *out, const WideScene *wide)
{
	constexpr int S = SUBPIXELS;
	const int width = view.width;
	const bool use_wide = wide && wide->active;
	uint16_t samples[MAX_WIDTH * S];

	// A scroll plane is drawn at whole pixels, a column wider than the view (the column a
	// leftward shift brings in), and laid into the samples `sub` of them to the left.
	View lane_view = view;
	lane_view.width = width + 1;
	uint16_t lane[MAX_WIDTH + 1];
	auto lay_plane = [&](bool first, int sub) {
		std::fill(lane, lane + width + 1, NO_PEN);
		if (first)
			draw_plane1(vram, lane, lane_view, regs, line, wide);
		else
			draw_plane2(vram, lane, lane_view, regs, line, wide);
		for (int k = 0; k <= width; k++)
		{
			if (lane[k] == NO_PEN)
				continue;
			for (int m = 0; m < S; m++)
			{
				const int j = k * S - sub + m;
				if (j >= 0 && j < width * S)
					samples[j] = lane[k];
			}
		}
	};
	auto sprites = [&](uint16_t priority) { draw_sprite_plane(vram, samples, view, priority, line, regs, shift, S); };
	const uint16_t oowcol = 0xf8 + (regs.oow & 7);

	if (line < regs.wba_v || line >= regs.wba_v + regs.wsi_v)
		std::fill(samples, samples + width * S, oowcol);
	else
	{
		const uint16_t bgcol = 0xf0 + (((regs.bg & 0xc0) == 0x80) ? (regs.bg & 7) : 0);
		std::fill(samples, samples + width * S, bgcol);

		if (bit(regs.pf, 7))
		{
			sprites(1);
			lay_plane(true, regs.sub1x);
			sprites(2);
			lay_plane(false, regs.sub2x);
			sprites(3);
		}
		else
		{
			sprites(1);
			lay_plane(false, regs.sub2x);
			sprites(2);
			lay_plane(true, regs.sub1x);
			sprites(3);
		}

		if (!(regs.wba_h == 0 && regs.wsi_h >= 160))
			for (int i = 0; i < width; i++)
			{
				const int sx = i + view.origin;
				if (sx < regs.wba_h || sx >= regs.wba_h + regs.wsi_h)
					std::fill(samples + i * S, samples + (i + 1) * S, oowcol);
			}
	}

	for (int i = 0; i < width; i++)
	{
		const int sx = i + view.origin;
		bool black = view.screen_only && (sx < 0 || sx >= 160);
		if (use_wide)
		{
			const int world_x = regs.wx2 + sx;
			black = world_x < wide->min_x || world_x >= wide->max_x;
		}
		out[i] = black ? 0xff000000u : pixel_from_samples(palette, samples + i * S);
	}
}

} // namespace


void render_line(const uint8_t *vram, const uint8_t *palette, const LineRegs &regs, int line,
		const View &view, const SpriteShift *shift, uint32_t *out, const WideScene *wide)
{
	if (needs_subpixels(regs, shift))
	{
		render_line_subpixels(vram, palette, regs, line, view, shift, out, wide);
		return;
	}
	uint16_t p[MAX_WIDTH];
	const bool use_wide = wide && wide->active;
	auto plane1 = [&]() { draw_plane1(vram, p, view, regs, line, wide); };
	auto plane2 = [&]() { draw_plane2(vram, p, view, regs, line, wide); };
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
