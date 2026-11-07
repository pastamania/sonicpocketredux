// Headless runner for the NGPC machine: boots the cartridge, runs frames,
// writes screenshots and compares work RAM with a BizHawk reference capture
// produced by scripts/bizhawk-dump-reference.lua.

#include "apps/input_log.h"
#include "common/png.h"
#include "ngpc/drop_dash.h"
#include "ngpc/option_screen.h"
#include "ngpc/k2ge_interp.h"
#include "ngpc/machine.h"
#include "ngpc/level_map.h"
#include "ngpc/sprite_trace.h"
#include "ngpc/wide_levels.h"
#include "ngpc/wide_sprites.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// A machine that tells the game it is on a given level whenever the game asks
// (the level number lives in RAM at 0x67D2), so that every level can be
// examined without playing to it. Development aid; the interpreter only.
class ForcedLevelMachine : public ngpc::Machine
{
public:
	int level = -1;

	uint8_t read_byte(offs_t address) override
	{
		return level >= 0 && address == 0x67D2 ? static_cast<uint8_t>(level) : ngpc::Machine::read_byte(address);
	}
};

void usage()
{
	std::cerr <<
		"usage: ngpc-run --rom <file> [--frames N] [--out dir] [--png-every N]\n"
		"                [--png-frames a,b,c] [--reference ram_frames.bin]\n"
		"                [--reference-offset K] [--input input.txt] [--check-render] [--interp-frames a,b,c]\n"
		"                [--sprite-tables first-last] [--check-trace] [--interp-metrics]\n"
		"                [--bookmarks file] [--radius N] [--aspect original|16:10|16:9] [--check-wide]\n"
		"                [--level N] [--warp frame,x,y] [--no-end-fill] [--no-subpixel]\n";
}

} // namespace

int main(int argc, char **argv)
{
	fs::path rom_path;
	fs::path out_dir = "out/ngpc-run";
	fs::path reference_path;
	fs::path input_path;
	fs::path dump_ram_path;
	int frames = 600;
	int png_every = 0;
	k2ge_render::Aspect aspect = k2ge_render::Aspect::Original;   // shape of the frame_*.png and final.png pictures
	int reference_offset = 0;
	int64_t trace_count = 0;
	bool check_render = false;
	int tables_first = -1, tables_last = -1;   // frames whose sprite tables are printed
	bool check_trace = false;
	bool interp_metrics = false;
	bool subpixel = true;   // --no-subpixel: the in-between pictures at whole pixels
	bool no_end_fill = false;   // --no-end-fill: leave the last 64 pixels of the levels as the game has them
	bool check_wide = false;
	bool drop_dash = false;   // --drop-dash: the optional move (see ngpc/drop_dash.h)
	bool option_screen = false;   // --option-screen: the host's rows on the Game Option screen (ngpc/option_screen.h)
	bool check_centre = false;   // --check-centre: a wide picture's screen must equal the machine's own
	int centre_bad = 0, centre_checked = 0, centre_from = 0, centre_to = 1 << 30;   // --centre-from, --centre-to: the frames checked
	int forced_level = -1;
	int warp_frame = -1, warp_x = 0, warp_y = 0;   // --warp: put the player somewhere else in the level
	fs::path bookmarks_path;   // frames to lay out as contact sheets (one frame number per line)
	int radius = 1;
	int metrics_print_from = 0;
	std::set<int> interp_frames;   // write in-between pictures at these frames
	k2ge_render::Interpolator interpolator{ k2ge_render::View{} };
	int render_mismatches = 0;
	std::set<int> png_frames;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) { usage(); std::exit(2); }
			return argv[++i];
		};
		if (arg == "--rom") rom_path = next();
		else if (arg == "--frames") frames = std::stoi(next());
		else if (arg == "--out") out_dir = next();
		else if (arg == "--png-every") png_every = std::stoi(next());
		else if (arg == "--aspect")
		{
			if (!k2ge_render::parse_aspect(next().c_str(), aspect))
			{
				usage();
				return 2;
			}
		}
		else if (arg == "--png-frames")
		{
			std::stringstream list(next());
			std::string item;
			while (std::getline(list, item, ','))
				png_frames.insert(std::stoi(item));
		}
		else if (arg == "--reference") reference_path = next();
		else if (arg == "--reference-offset") reference_offset = std::stoi(next());
		else if (arg == "--input") input_path = next();
		else if (arg == "--trace") trace_count = std::stoll(next());
		else if (arg == "--dump-ram") dump_ram_path = next();
		else if (arg == "--check-render") check_render = true;
		else if (arg == "--sprite-tables")
		{
			const std::string range = next();
			tables_first = std::stoi(range);
			tables_last = range.find('-') == std::string::npos ? tables_first : std::stoi(range.substr(range.find('-') + 1));
		}
		else if (arg == "--check-trace") check_trace = true;
		else if (arg == "--interp-metrics") interp_metrics = true;
		else if (arg == "--no-subpixel") subpixel = false;
		else if (arg == "--no-end-fill") no_end_fill = true;
		else if (arg == "--check-wide") check_wide = true;
		else if (arg == "--drop-dash") drop_dash = true;
		else if (arg == "--option-screen") option_screen = true;
		else if (arg == "--check-centre") check_centre = true;
		else if (arg == "--centre-from") centre_from = std::stoi(next());
		else if (arg == "--centre-to") centre_to = std::stoi(next());
		else if (arg == "--level") forced_level = std::stoi(next());
		else if (arg == "--warp")
		{
			// Development aid for looking at the far parts of a level: on
			// this frame the player (his x and y in the level) and the camera
			// are set to a position, and the game carries on from there.
			if (std::sscanf(next().c_str(), "%d,%d,%d", &warp_frame, &warp_x, &warp_y) != 3)
			{
				usage();
				return 2;
			}
		}
		else if (arg == "--bookmarks") bookmarks_path = next();
		else if (arg == "--radius") radius = std::stoi(next());
		else if (arg == "--metrics-from") metrics_print_from = std::stoi(next());
		else if (arg == "--interp-frames")
		{
			std::stringstream list(next());
			std::string item;
			while (std::getline(list, item, ','))
				interp_frames.insert(std::stoi(item));
		}
		else { usage(); return 2; }
	}
	if (rom_path.empty()) { usage(); return 2; }

	ForcedLevelMachine machine;

	// The wide pictures (--aspect) are drawn by their own interpolator, fed the
	// same frames, with the level's maps from the cartridge.
	k2ge_render::Interpolator wide_interpolator{ k2ge_render::aspect_view(aspect) };
	interpolator.set_subpixel(subpixel);
	wide_interpolator.set_subpixel(subpixel);
	std::unique_ptr<k2ge_render::WideScene> wide_scene = std::make_unique<k2ge_render::WideScene>();
	std::unique_ptr<ngpc::LevelMap> level_map;
	bool finale = false;   // the last fight's pod has been hit (LevelRam::finale)
	int wide_snaps = 0, wide_left_prev = -1, wide_uncovered = 0, wide_frames = 0, wide_active = 0, wide_imperfect = 0, wide_switches = 0, wide_clipped = 0, wide_narrowed = 0;
	// --check-wide: what the wide view showed beyond the screen last frame,
	// by plane, world column and row, to compare with what the game then draws
	// there when it scrolls into view.
	std::unordered_map<uint64_t, uint16_t> wide_shown;
	long pop_checked[2] = {}, pop_bad[2] = {};
	bool wide_was_active = false;
	double wide_worst = 1.0;

	// Writes the screen as the host would show it in the chosen aspect.
	auto write_screen_png = [&](const fs::path &path) {
		if (aspect == k2ge_render::Aspect::Original)
		{
			write_png(path, machine.framebuffer(), k2ge::SCREEN_WIDTH, k2ge::SCREEN_HEIGHT);
			return;
		}
		std::vector<uint32_t> picture(static_cast<size_t>(k2ge_render::aspect_width(aspect)) * k2ge_render::LINES);
		wide_interpolator.render_as_drawn(picture.data());
		write_png(path, picture.data(), k2ge_render::aspect_width(aspect), k2ge::SCREEN_HEIGHT);
	};
	if (const std::string error = machine.load_cartridge(read_file(rom_path)); !error.empty())
	{
		std::cerr << "cannot load " << rom_path << ": " << error << '\n';
		return 1;
	}
	machine.level = forced_level;
	if (drop_dash || option_screen)
		ngpc::set_hook_redirect(ngpc::redirect_interpreted);
	if (drop_dash)
		ngpc::set_drop_dash(true);
	if (option_screen)
		ngpc::set_option_screen(true);
	machine.boot();
	machine.trace_instructions(trace_count);
	level_map = std::make_unique<ngpc::LevelMap>(machine.cartridge());
	// A wide picture needs the sprites of the extra width (changes game timing slightly).
	ngpc::set_wide_sprite_margin((k2ge_render::aspect_width(aspect) - 160 + 1) / 2);
	ngpc::SpriteTrace sprite_trace(machine);
	int sprite_frames = 0, sprite_frames_matched = 0;
	// --bookmarks: for the frames around each bookmark, the real frame and its
	// in-between pictures, kept to be laid out side by side.
	std::vector<int> bookmarks;
	if (!bookmarks_path.empty())
	{
		std::ifstream list(bookmarks_path);
		for (int frame_number; list >> frame_number;)
			bookmarks.push_back(frame_number);
	}
	constexpr int SUB_PICTURES = 4;
	std::map<int, std::vector<std::vector<uint32_t>>> sheet_pictures;   // frame -> real, then each sub-frame
	auto wanted_for_sheet = [&](int frame_number) {
		for (int b : bookmarks)
			if (frame_number >= b - radius && frame_number <= b + radius)
				return true;
		return false;
	};
	int big_shifts = 0, printed_mismatches = 0;
	// --interp-metrics: how the in-between pictures treat the game's objects.
	struct { long samples = 0, torn = 0, hud = 0, hud_moved = 0, player = 0, player_moved = 0, world = 0, world_matched = 0;
			int max_shift = 0; } metrics;
	fs::create_directories(out_dir);

	std::vector<uint8_t> reference;
	if (!reference_path.empty())
		reference = read_file(reference_path);
	const size_t ram_size = 0x3000;
	const int reference_frames = static_cast<int>(reference.size() / ram_size);
	std::map<int, uint8_t> inputs;
	if (!input_path.empty())
		inputs = read_input_log(input_path);

	std::ofstream ram_dump;
	if (!dump_ram_path.empty())
		ram_dump.open(dump_ram_path, std::ios::binary);

	int first_mismatch = -1;
	for (int frame = 0; frame < frames; ++frame)
	{
		if (auto it = inputs.find(frame); it != inputs.end())
			machine.set_buttons(it->second);
		if (frame == warp_frame)
		{
			// The player's position (task 0x6708, +0x12 and +0x16), his
			// velocities, and the camera 48 pixels left of and 75 below him.
			auto poke_word = [&](uint32_t address, int value) {
				machine.write_byte(address, static_cast<uint8_t>(value & 0xff));
				machine.write_byte(address + 1, static_cast<uint8_t>((value >> 8) & 0xff));
			};
			poke_word(0x671A, warp_x);
			poke_word(0x671E, warp_y);
			for (uint32_t address = 0x6720; address < 0x6728; address++)
				machine.write_byte(address, 0);
			poke_word(ngpc::LEVEL_CAMERA_X, warp_x - 48);
			poke_word(ngpc::LEVEL_CAMERA_Y, warp_y - 75);
		}
		try
		{
			machine.run_frame();
		}
		catch (const std::exception &error)
		{
			std::fprintf(stderr, "stopped in frame %d: %s (PC=%06X)\n", frame, error.what(), machine.cpu().m_pc.d);
			return 1;
		}

		// The captured frame, redrawn by the host renderer, must equal what the
		// machine drew while it ran.
		if (check_render)
		{
			static std::vector<uint32_t> redrawn(k2ge::SCREEN_WIDTH * k2ge::SCREEN_HEIGHT);
			k2ge_render::render_frame(machine.video().frame(), nullptr, k2ge_render::View{}, nullptr, redrawn.data());
			if (std::memcmp(redrawn.data(), machine.framebuffer(), redrawn.size() * 4) != 0 && frame > 0)
			{
				if (render_mismatches++ < 5)
					std::printf("renderer differs from framebuffer at frame %d\n", frame);
			}
		}

		// What the game says its sprites are: print each new table, and check
		// that every frame's sprite table can be paired with one.
		sprite_trace.collect();
		const ngpc::SpriteTable *table = sprite_trace.match(machine.video().frame().vram.data() + 0x800);
		{
			const uint8_t *vram_table = machine.video().frame().vram.data() + 0x800;
			static uint64_t last_serial = 0;
			bool any_visible = false;
			for (int i = 0; i < 64; ++i)
				any_visible |= (vram_table[i * 4 + 1] & 0x18) != 0;
			if (any_visible && frame > 0)
			{
				++sprite_frames;
				sprite_frames_matched += table != nullptr;
				if (check_trace && !table && sprite_frames - sprite_frames_matched <= 5)
					std::printf("frame %d: sprite table matches no traced expansion\n", frame);
			}
			if (table && table->serial != last_serial)
			{
				last_serial = table->serial;
				if (frame >= tables_first && frame <= tables_last)
				{
					std::printf("frame %d: table #%llu, %zu entries\n", frame,
							static_cast<unsigned long long>(table->serial), table->entries.size());
					for (size_t e = 0; e < table->entries.size(); ++e)
					{
						const ngpc::SpriteEntry &entry = table->entries[e];
						std::printf("  entry %2zu owner=%04X caller=%06X #%d pos=(%4d,%4d) layout=%04X prio=%04X slots:",
								e, entry.owner, entry.caller, entry.ordinal, entry.x, entry.y, entry.layout, entry.priority);
						for (int s = 0; s < 64; ++s)
							if (table->entry_of_slot[s] == static_cast<int8_t>(e))
								std::printf(" %d", s);
						std::printf("\n");
					}
				}
			}
		}

		if (aspect != k2ge_render::Aspect::Original || check_wide)
		{
			++wide_frames;
			const ngpc::LevelRam level_ram = ngpc::read_level_ram(machine, finale);
			finale = level_ram.finale;
			ngpc::extend_level_ends(machine, level_ram, aspect != k2ge_render::Aspect::Original && !no_end_fill);
			const bool active = level_map->build(machine.video().frame(), level_ram, *wide_scene);
			// (A ring scene, Chaotic Space's, holds no tiles: the hardware's rings are drawn as they are.)
			const bool tiles = active && !wide_scene->ring;
			if (aspect != k2ge_render::Aspect::Original)
			{
				const int extra = k2ge_render::aspect_width(aspect) - 160;
				ngpc::set_wide_arena(ngpc::wide_arena(level_ram), extra / 2, extra - extra / 2);
				ngpc::track_wide_view(active ? wide_scene.get() : nullptr, extra);
			}
			if (check_wide && active)
			{
				// The camera's range must always contain the screen itself.
				if (wide_scene->min_x > wide_scene->world_x || wide_scene->world_x + 160 > wide_scene->max_x)
				{
					if (++wide_clipped <= 5)
						std::printf("frame %d: the camera's range %d..%d does not contain the screen at %d\n", frame,
								wide_scene->min_x, wide_scene->max_x, wide_scene->world_x);
				}
				if (wide_scene->max_x - wide_scene->min_x < k2ge_render::aspect_width(k2ge_render::Aspect::Wide16x9))
					++wide_narrowed;
			}
			if (check_wide)
			{
				const k2ge_render::Frame &f = machine.video().frame();
				auto key = [](int plane, int column, int row) {
					return (static_cast<uint64_t>(plane) << 48) | (static_cast<uint64_t>(column & 0xffffff) << 24) |
							static_cast<uint64_t>(row & 0xffffff);
				};
				if (tiles)
				{
					const int first_column = wide_scene->world_x >> 3, first_ring_row = f.line[0].s2y >> 3;
					const int top_row = wide_scene->row[first_ring_row];
					for (int plane = 0; plane < 2; plane++)
					{
						if (plane == 0 && !wide_scene->plane1)
							continue;
						const int base = plane ? 0x1800 : 0x1000;
						for (int r = 0; r < 20; r++)
							for (int c = 0; c < 21; c++)
							{
								const int ring_row = (first_ring_row + r) & 31, column = first_column + c;
								const auto it = wide_shown.find(key(plane, column, wide_scene->row[ring_row]));
								if (it == wide_shown.end())
									continue;
								const int at = base + ring_row * 64 + (column & 31) * 2;
								++pop_checked[plane];
								if (it->second != (f.vram[at] | (f.vram[at + 1] << 8)))
								{
									++pop_bad[plane];
									if (pop_bad[0] + pop_bad[1] <= 8)
										std::printf("frame %d: plane %d column %d row %d came on screen as %04x, the wide view had %04x\n",
												frame, plane + 1, column, wide_scene->row[ring_row],
												f.vram[at] | (f.vram[at + 1] << 8), it->second);
								}
							}
					}
					wide_shown.clear();
					for (int plane = 0; plane < 2; plane++)
						for (int ring_row = 0; ring_row < 32; ring_row++)
							for (int j = 0; j < k2ge_render::WIDE_COLS; j++)
							{
								const int column = wide_scene->first_col + j, row = wide_scene->row[ring_row];
								const bool on_screen = column >= first_column && column < first_column + 21 &&
										row >= top_row && row < top_row + 20;
								if (!on_screen)
									wide_shown[key(plane, column, row)] = wide_scene->entry[plane][ring_row][j];
							}
				}
				else
					wide_shown.clear();
			}
			if (active != wide_was_active)
			{
				++wide_switches;
				if (check_wide && wide_switches <= 12)
					std::printf("frame %d: %s\n", frame, active ? "wide" : "centred");
			}
			wide_was_active = active;
			if (active)
			{
				++wide_active;
				if (level_map->last_agreement() < 1.0)
				{
					++wide_imperfect;
					if (wide_imperfect <= 10)
						std::printf("frame %d: decoder agrees with %.1f%% of the tiles on screen\n", frame,
								level_map->last_agreement() * 100);
				}
				wide_worst = std::min(wide_worst, level_map->last_agreement());
			}
			if (aspect != k2ge_render::Aspect::Original)
			{
				wide_interpolator.push(machine.video().frame(), table, wide_scene.get());
				// --check-centre: the part of the picture where the machine's own screen
				// lies must be that screen (where the level is drawn from the scene, the
				// scene must have it right). Sprites that only a wide view keeps may differ
				// where they overlap, so a few pixels are allowed to differ.
				if (check_centre && active && frame >= centre_from && frame < centre_to)
				{
					int first = 0, last = 0;
					if (wide_interpolator.wide_extent(first, last))
					{
						const int width = k2ge_render::aspect_width(aspect), left = wide_scene->world_x - first;
						std::vector<uint32_t> picture(static_cast<size_t>(width) * k2ge_render::LINES);
						wide_interpolator.render_as_drawn(picture.data());
						int bad = 0, x0 = 999, x1 = -1, y0 = 999, y1 = -1;
						for (int y = 0; y < k2ge::SCREEN_HEIGHT; y++)
							for (int x = 0; x < k2ge::SCREEN_WIDTH; x++)
								// (The HUD, at the picture's edges, is not where the machine put it.)
								if (!((x < 48 && y < 56) || (x < 32 && y >= 136)) && picture[static_cast<size_t>(y) * width + left + x] != machine.framebuffer()[y * k2ge::SCREEN_WIDTH + x])
								{
									++bad;
									x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
								}
						++centre_checked;
						if (bad > 0)
						{
							++centre_bad;
							if (centre_bad <= 12)
								std::printf("frame %d: %d pixels of the screen differ, x %d..%d y %d..%d\n", frame, bad, x0, x1, y0, y1);
							if (centre_bad == 1)
							{
								// Both pictures of the first frame that differs, to look at.
								write_png(out_dir / "centre_machine.png", machine.framebuffer(), k2ge::SCREEN_WIDTH, k2ge::SCREEN_HEIGHT);
								write_png(out_dir / "centre_wide.png", picture.data(), width, k2ge::SCREEN_HEIGHT);
							}
						}
					}
				}
				// The scene must hold a tile for every column of the view.
				int first = 0, last = 0;
				if (active && wide_interpolator.wide_extent(first, last))
				{
					// The view should glide: report a sudden change of how far it reaches left of the screen.
					const int left_now = wide_scene->world_x - first;
					if (wide_left_prev >= 0 && std::abs(left_now - wide_left_prev) > 8)
					{
						++wide_snaps;
						if (wide_snaps <= 5)
							std::printf("frame %d: the view's left extra went from %d to %d pixels\n", frame, wide_left_prev, left_now);
					}
					wide_left_prev = left_now;
				}
				else
					wide_left_prev = -1;
				if (tiles && wide_interpolator.wide_extent(first, last) &&
						(first < wide_scene->first_col * 8 || last > (wide_scene->first_col + k2ge_render::WIDE_COLS) * 8))
				{
					++wide_uncovered;
					if (wide_uncovered <= 5)
						std::printf("frame %d: the view shows world x %d..%d, the scene holds %d..%d\n", frame, first, last,
								wide_scene->first_col * 8, (wide_scene->first_col + k2ge_render::WIDE_COLS) * 8);
				}
			}
		}

		if (!interp_frames.empty() || interp_metrics || !bookmarks.empty())
		{
			interpolator.push(machine.video().frame(), table);
			if (interp_metrics && table)
			{
				for (int k = 0; k < 4; ++k)
				{
					const k2ge_render::SpriteShift shift = interpolator.sprite_shift(k / 4.0);
					int scenery_x = 0, scenery_y = 0;
					interpolator.scenery_shift(k / 4.0, scenery_x, scenery_y);
					for (size_t e = 0; e < table->entries.size(); ++e)
					{
						// Every sprite of an object must get the same shift.
						int first = -1;
						bool torn = false;
						for (int s = 0; s < 64; ++s)
						{
							if (table->entry_of_slot[s] != static_cast<int8_t>(e))
								continue;
							if (first < 0)
								first = s;
							else
								torn |= shift.dx[s] != shift.dx[first] || shift.dy[s] != shift.dy[first];
						}
						if (first < 0)
							continue;
						const int dx = shift.dx[first], dy = shift.dy[first];
						++metrics.samples;
						metrics.torn += torn;
						metrics.max_shift = std::max({ metrics.max_shift, std::abs(dx), std::abs(dy) });
						if (std::max(std::abs(dx), std::abs(dy)) > 48 && big_shifts++ < 8)
							std::printf("frame %d k=%d: object owner=%04X caller=%06X pos=(%d,%d) shift (%d,%d)\n", frame, k,
									table->entries[e].owner, table->entries[e].caller, table->entries[e].x,
									table->entries[e].y, dx, dy);
						const uint32_t owner = table->entries[e].owner;
						if (owner == 0x40B2)
						{
							++metrics.hud;
							if (dx || dy)
							{
								++metrics.hud_moved;
								if (metrics.hud_moved <= 5)
									std::printf("frame %d k=%d: HUD entry %zu (caller %06X) shifted (%d,%d)\n", frame, k, e,
											table->entries[e].caller, dx, dy);
							}
						}
						else if (owner == 0x6708)
						{
							++metrics.player;
							metrics.player_moved += dx || dy;
						}
						if ((scenery_x || scenery_y) && owner != 0x40B2 && owner != 0x6708)
						{
							++metrics.world;
							const bool same = dx == scenery_x && dy == scenery_y;
							metrics.world_matched += same;
							if (!same && frame >= metrics_print_from && printed_mismatches++ < 24)
								std::printf("frame %d k=%d: owner=%04X caller=%06X pos=(%d,%d) shift (%d,%d), scenery (%d,%d)\n", frame,
										k, owner, table->entries[e].caller, table->entries[e].x, table->entries[e].y, dx, dy, scenery_x, scenery_y);
						}
					}
				}
			}
			if (wanted_for_sheet(frame))
			{
				std::vector<std::vector<uint32_t>> &pictures = sheet_pictures[frame];
				pictures.emplace_back(machine.framebuffer(), machine.framebuffer() + k2ge::SCREEN_WIDTH * k2ge::SCREEN_HEIGHT);
				for (int k = 0; k < SUB_PICTURES; ++k)
				{
					pictures.emplace_back(k2ge::SCREEN_WIDTH * k2ge::SCREEN_HEIGHT);
					interpolator.render(static_cast<double>(k) / SUB_PICTURES, pictures.back().data());
				}
			}
			if (interp_frames.count(frame))
			{
				// In the chosen aspect (the wide interpolator is fed the same frames).
				const int width = k2ge_render::aspect_width(aspect);
				std::vector<uint32_t> picture(static_cast<size_t>(width) * k2ge::SCREEN_HEIGHT);
				for (int k = 0; k < 4; ++k)
				{
					if (aspect == k2ge_render::Aspect::Original)
						interpolator.render(k / 4.0, picture.data());
					else
						wide_interpolator.render(k / 4.0, picture.data());
					char name[64];
					std::snprintf(name, sizeof(name), "interp_%05d_%d.png", frame, k);
					write_png(out_dir / name, picture.data(), width, k2ge::SCREEN_HEIGHT);
				}
			}
		}

		if (ram_dump)
			ram_dump.write(reinterpret_cast<const char *>(machine.work_ram()), 0x3000);

		if ((png_every > 0 && frame % png_every == 0) || png_frames.count(frame))
		{
			char name[64];
			std::snprintf(name, sizeof(name), "frame_%05d.png", frame);
			write_screen_png(out_dir / name);
		}

		const int ref_frame = frame + reference_offset;
		if (first_mismatch < 0 && ref_frame >= 0 && ref_frame < reference_frames)
		{
			const uint8_t *ref = &reference[static_cast<size_t>(ref_frame) * ram_size];
			const uint8_t *ours = machine.work_ram();
			std::vector<int> diffs;
			for (size_t a = 0; a < ram_size; ++a)
				if (ref[a] != ours[a])
					diffs.push_back(static_cast<int>(a));
			if (!diffs.empty())
			{
				first_mismatch = frame;
				std::printf("first RAM mismatch at frame %d (reference frame %d): %zu bytes differ\n",
						frame, ref_frame, diffs.size());
				for (size_t k = 0; k < diffs.size() && k < 48; ++k)
				{
					const int a = diffs[k];
					std::printf("  %04X: ours %02X ref %02X\n", 0x4000 + a, ours[a], ref[a]);
				}
			}
		}
	}

	std::ofstream(out_dir / "ram_final.bin", std::ios::binary)
		.write(reinterpret_cast<const char *>(machine.work_ram()), 0x3000);
	write_screen_png(out_dir / "final.png");
	std::printf("ran %d frames; PC=%06X\n", frames, machine.cpu().m_pc.d);
	for (int b : bookmarks)
	{
		// One row per frame: the real frame, then the pictures drawn between it and the next.
		const int columns = 1 + SUB_PICTURES, rows = 2 * radius + 1;
		const int width = columns * k2ge::SCREEN_WIDTH, height = rows * k2ge::SCREEN_HEIGHT;
		std::vector<uint32_t> sheet(static_cast<size_t>(width) * height, 0xff000000u);
		for (int row = 0; row < rows; ++row)
		{
			auto it = sheet_pictures.find(b - radius + row);
			if (it == sheet_pictures.end())
				continue;
			for (int column = 0; column < columns; ++column)
				for (int y = 0; y < k2ge::SCREEN_HEIGHT; ++y)
					std::copy_n(&it->second[column][y * k2ge::SCREEN_WIDTH], k2ge::SCREEN_WIDTH,
							&sheet[static_cast<size_t>(row * k2ge::SCREEN_HEIGHT + y) * width + column * k2ge::SCREEN_WIDTH]);
		}
		char name[64];
		std::snprintf(name, sizeof(name), "sheet_%05d.png", b);
		write_png(out_dir / name, sheet.data(), width, height);
		std::printf("wrote %s (frames %d..%d, rows: real frame then %d in-between pictures)\n", name, b - radius, b + radius,
				SUB_PICTURES);
	}
	if (check_wide)
		std::printf("wide check: frames where the scene did not hold every column the view shows: %d; sudden changes of the view's left extra: %d\n",
				wide_uncovered, wide_snaps);
	if (check_wide)
		std::printf("wide check: the camera's range clipped the screen itself on %d frames, and was narrower than 16:9 on %d\n",
				wide_clipped, wide_narrowed);
	if (check_wide)
		std::printf("wide check: of the tiles that scrolled onto the screen, plane 2 had %ld of %ld different from the wide view's "
				"picture a frame earlier, plane 1 %ld of %ld; the decoder holds %zu game edits\n",
				pop_bad[1], pop_checked[1], pop_bad[0], pop_checked[0], level_map->edit_count());
	if (check_wide)
		std::printf("wide check: %d frames, the level decoder reproduces the screen on %d (%.1f%%); "
				"%d of those not exactly (worst %.1f%%); switched between wide and centred %d times\n", wide_frames, wide_active,
				wide_frames ? 100.0 * wide_active / wide_frames : 0.0, wide_imperfect, wide_worst * 100, wide_switches);
	if (interp_metrics)
	{
		std::printf("interp metrics: %ld object samples; %ld objects torn (parts shifted differently); max shift %d px\n",
				metrics.samples, metrics.torn, metrics.max_shift);
		std::printf("  HUD: %ld samples, %ld shifted;  player: %ld samples, %ld shifted\n", metrics.hud, metrics.hud_moved,
				metrics.player, metrics.player_moved);
		std::printf("  while the scenery was displaced, other objects: %ld samples, %ld shifted exactly with it (informational: effects attached to the player rightly do not)\n", metrics.world,
				metrics.world_matched);
	}
	if (check_trace)
		std::printf("trace check: %d of %d frames with visible sprites pair with a traced expansion\n", sprite_frames_matched, sprite_frames);
	if (check_render)
		std::printf("render check: %d of %d frames differ\n", render_mismatches, frames);
	if (check_centre)
		std::printf("centre check: %d of %d wide frames differ from the machine's own screen\n", centre_bad, centre_checked);
	if (!reference.empty() && first_mismatch < 0)
		std::printf("RAM matched the reference for all compared frames\n");
	return 0;
}
