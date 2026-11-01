// sonic-pocket: plays the recompiled Sonic Pocket Adventure in an SDL3 window.
//
//   sonic-pocket <cartridge> [--interpreter] [--japanese] [--scale N]
//                [--aspect original|16:10|16:9] [--fps N] [--no-smoothing]
//                [--record input.txt | --replay input.txt] [--stats]
//
// The game runs at its own rate (about 59.95 frames a second, with the
// scenery and sprites stepping every second frame) and every refresh of the
// monitor shows a picture drawn between the game's frames, so motion is smooth
// at whatever rate the monitor runs. --no-smoothing shows the game's own
// frames instead. --fps N presents N pictures a second by the clock instead of
// waiting for the monitor. --stats prints how evenly pictures were presented.
// --record writes every frame's buttons to a file in the format
// `ngpc-run --input` reads, starting from a fresh cartridge (the save file is
// neither read nor written), and F9 marks the frame on screen in
// `input.txt.bookmarks`. Replaying the file with `ngpc-run` reproduces the
// session exactly, so a glitch can be examined from its bookmark. --replay
// plays such a file back in the window instead of reading the keyboard (also
// from a fresh cartridge), to see a recorded session in any aspect.
// --aspect picks the shape of the picture: the machine's own 160x152, or a
// 16:10 or 16:9 window (--widescreen is 16:9). Tab or F10 cycles through them
// while playing, and the last choice is remembered in the preferences folder.
// Screens the game draws for the machine's own 160x152 are centred with black
// bars.
//
// Controls: arrow keys or D-pad/left stick, Z / gamepad south = A,
// X / gamepad east = B, Enter / gamepad start = Option, Escape quits.
//
// The game's save data lives in the cartridge's flash chip (the last 32 KiB of
// the ROM); it is written to the SDL preferences folder whenever it changes.

#include "ngpc/k2ge_interp.h"
#include "ngpc/level_map.h"
#include "ngpc/machine.h"
#include "ngpc/sprite_trace.h"
#include "ngpc/wide_sprites.h"
#include "recomp_rt/game.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// How evenly frames reach the screen: the times between presents.
class PresentStats
{
public:
	void present(uint64_t now_ns)
	{
		if (!m_first)
			m_first = now_ns;
		// The first second is window and device start-up, not steady play.
		if (m_last && now_ns - m_first > 1000000000ull)
		{
			const double interval = static_cast<double>(now_ns - m_last) / 1e6;
			if (interval > m_longest)
			{
				m_longest = interval;
				m_longest_at = static_cast<double>(now_ns - m_first) / 1e9;
			}
			m_intervals.push_back(interval);
		}
		m_last = now_ns;
	}

	void report(const char *label) const
	{
		if (m_intervals.size() < 10)
			return;
		std::vector<double> sorted = m_intervals;
		std::sort(sorted.begin(), sorted.end());
		double mean = 0;
		for (double v : sorted)
			mean += v;
		mean /= static_cast<double>(sorted.size());
		double variance = 0;
		for (double v : sorted)
			variance += (v - mean) * (v - mean);
		const double deviation = std::sqrt(variance / static_cast<double>(sorted.size()));
		auto percentile = [&](double p) { return sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]; };
		std::fprintf(stderr, "%s (after the first second): %zu intervals; ms: mean %.2f, sd %.2f, p1 %.2f, p50 %.2f, p99 %.2f, max %.2f (at %.1f s)\n",
				label, sorted.size(), mean, deviation, percentile(0.01), percentile(0.5), percentile(0.99),
				sorted.back(), m_longest_at);
	}

private:
	uint64_t m_first = 0, m_last = 0;   // m_last: the previous present, whether counted or not
	double m_longest = 0, m_longest_at = 0;
	std::vector<double> m_intervals;
};

constexpr size_t SAVE_OFFSET = 0x1F8000;   // cartridge offset of the flash save area
constexpr size_t SAVE_SIZE = 0x8000;
constexpr int AUDIO_CHANNELS = 2;
// Keep a few frames of sound queued; the emulation is paced by the audio clock.
constexpr int AUDIO_QUEUE_TARGET_BYTES = ngpc::Machine::AUDIO_RATE / 60 * AUDIO_CHANNELS * 2 * 4;

std::vector<uint8_t> read_file(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

fs::path save_path()
{
	char *pref = SDL_GetPrefPath("SonicPocket", "SonicPocket");
	fs::path path = pref ? fs::path(pref) / "flash_save.bin" : fs::path("flash_save.bin");
	SDL_free(pref);
	return path;
}

fs::path settings_path()
{
	return save_path().replace_filename("settings.txt");
}

// The settings file holds `key=value` lines; only the aspect is kept so far.
void load_aspect(k2ge_render::Aspect &aspect)
{
	std::ifstream file(settings_path());
	for (std::string line; std::getline(file, line);)
		if (line.rfind("aspect=", 0) == 0)
			k2ge_render::parse_aspect(line.c_str() + 7, aspect);
}

void save_aspect(k2ge_render::Aspect aspect)
{
	std::ofstream(settings_path()) << "aspect=" << k2ge_render::aspect_name(aspect) << "\n";
}

uint8_t read_buttons(const bool *keys, SDL_Gamepad *pad)
{
	uint8_t buttons = 0;
	auto pressed = [&](SDL_Scancode key, SDL_GamepadButton button) {
		return keys[key] || (pad && SDL_GetGamepadButton(pad, button));
	};
	if (pressed(SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP)) buttons |= ngpc::BUTTON_UP;
	if (pressed(SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) buttons |= ngpc::BUTTON_DOWN;
	if (pressed(SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) buttons |= ngpc::BUTTON_LEFT;
	if (pressed(SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) buttons |= ngpc::BUTTON_RIGHT;
	if (pressed(SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= ngpc::BUTTON_A;
	if (pressed(SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_EAST)) buttons |= ngpc::BUTTON_B;
	if (pressed(SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START)) buttons |= ngpc::BUTTON_OPTION;
	if (pad)
	{
		constexpr Sint16 dead_zone = 12000;
		const Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
		const Sint16 y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
		if (x < -dead_zone) buttons |= ngpc::BUTTON_LEFT;
		if (x > dead_zone) buttons |= ngpc::BUTTON_RIGHT;
		if (y < -dead_zone) buttons |= ngpc::BUTTON_UP;
		if (y > dead_zone) buttons |= ngpc::BUTTON_DOWN;
	}
	return buttons;
}

} // namespace

int main(int argc, char **argv)
{
	fs::path rom_path;
	bool use_interpreter = false;
	int language = 1;
	int scale = 4;
	int fps = 0;             // 0: one picture per refresh of the monitor
	bool smoothing = true;
	k2ge_render::Aspect aspect = k2ge_render::Aspect::Original;
	bool aspect_given = false;
	fs::path record_path, replay_path;
	bool stats = false;
	double quit_after = 0;   // seconds, for measuring
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--interpreter") use_interpreter = true;
		else if (arg == "--japanese") language = 0;
		else if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::stoi(argv[++i]));
		else if (arg == "--fps" && i + 1 < argc) fps = std::max(30, std::stoi(argv[++i]));
		else if (arg == "--no-smoothing") smoothing = false;
		else if (arg == "--aspect" && i + 1 < argc)
		{
			if (!k2ge_render::parse_aspect(argv[++i], aspect))
			{
				std::fprintf(stderr, "unknown aspect %s (original, 16:10 or 16:9)\n", argv[i]);
				return 2;
			}
			aspect_given = true;
		}
		else if (arg == "--widescreen")
		{
			aspect = k2ge_render::Aspect::Wide16x9;
			aspect_given = true;
		}
		else if (arg == "--record" && i + 1 < argc) record_path = argv[++i];
		else if (arg == "--replay" && i + 1 < argc) replay_path = argv[++i];
		else if (arg == "--stats") stats = true;
		else if (arg == "--quit-after" && i + 1 < argc) quit_after = std::stod(argv[++i]);
		else rom_path = arg;
	}
	if (rom_path.empty())
	{
		std::fprintf(stderr, "usage: sonic-pocket <cartridge> [--interpreter] [--japanese] [--scale N]\n"
				"                    [--aspect original|16:10|16:9] [--fps N] [--no-smoothing]\n"
				"                    [--record input.txt | --replay input.txt] [--stats]\n");
		return 2;
	}

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	std::vector<uint8_t> rom = read_file(rom_path);
	const fs::path save_file = save_path();
	const bool recording = !record_path.empty();
	// A recording or its replay starts from a fresh cartridge: the save file
	// is neither read nor written.
	const bool fresh_cartridge = recording || !replay_path.empty();
	if (rom.size() == 0x200000 && !fresh_cartridge)
	{
		std::vector<uint8_t> save = read_file(save_file);
		if (save.size() == SAVE_SIZE)
			std::copy(save.begin(), save.end(), rom.begin() + SAVE_OFFSET);
	}

	auto machine = std::make_unique<ngpc::Machine>();
	if (const std::string error = machine->load_cartridge(rom); !error.empty())
	{
		std::fprintf(stderr, "cannot load %s: %s\n", rom_path.string().c_str(), error.c_str());
		return 1;
	}
	machine->boot(language);
	std::vector<uint8_t> saved(rom.begin() + SAVE_OFFSET, rom.begin() + SAVE_OFFSET + SAVE_SIZE);

	// --replay: the buttons of each frame, from a log as --record writes it.
	std::vector<uint8_t> replay;
	if (!replay_path.empty())
	{
		const struct { const char *name; uint8_t bit; } names[] = {
			{ "Up", ngpc::BUTTON_UP }, { "Down", ngpc::BUTTON_DOWN }, { "Left", ngpc::BUTTON_LEFT },
			{ "Right", ngpc::BUTTON_RIGHT }, { "A", ngpc::BUTTON_A }, { "B", ngpc::BUTTON_B },
			{ "Option", ngpc::BUTTON_OPTION } };
		std::ifstream log(replay_path);
		if (!log)
		{
			std::fprintf(stderr, "cannot read %s\n", replay_path.string().c_str());
			return 1;
		}
		for (std::string line; std::getline(log, line);)
		{
			const size_t tab = line.find('\t');
			if (tab == std::string::npos || line[0] == '#')
				continue;
			const size_t frame_number = static_cast<size_t>(std::stoul(line.substr(0, tab)));
			uint8_t buttons = 0;
			for (const auto &name : names)
			{
				// Match whole names between commas ("A" must not match "Option").
				const std::string list = "," + line.substr(tab + 1) + ",";
				if (list.find(std::string(",") + name.name + ",") != std::string::npos)
					buttons |= name.bit;
			}
			if (replay.size() <= frame_number)
				replay.resize(frame_number + 1, 0);
			replay[frame_number] = buttons;
		}
	}

	std::ofstream record;
	std::ofstream bookmarks;
	if (recording)
	{
		record.open(record_path);
		bookmarks.open(record_path.string() + ".bookmarks");
		if (language != 1)
			std::fprintf(stderr, "note: a recording made with --japanese replays in English\n");
	}

	std::unique_ptr<rt::RecompiledGame> game;
	if (!use_interpreter)
		game = std::make_unique<rt::RecompiledGame>(*machine);

	// The shape of the picture. A wide one is scaled to fit the window; the
	// stock one only by whole numbers.
	if (!aspect_given)
		load_aspect(aspect);
	k2ge_render::View view = k2ge_render::aspect_view(aspect);
	k2ge_render::Interpolator interpolator(view);
	ngpc::SpriteTrace sprite_trace(*machine);
	ngpc::LevelMap level_map(machine->cartridge());
	auto wide_scene = std::make_unique<k2ge_render::WideScene>();
	std::vector<uint32_t> picture;

	SDL_Window *window = SDL_CreateWindow("Sonic Pocket Adventure",
			view.width * scale, k2ge::SCREEN_HEIGHT * scale, SDL_WINDOW_RESIZABLE);
	SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
	if (!renderer)
	{
		std::fprintf(stderr, "cannot create window: %s\n", SDL_GetError());
		return 1;
	}
	// One picture per refresh of the monitor, unless a rate was asked for or
	// the driver cannot wait for the refresh.
	bool vsync = fps == 0;
	if (vsync && !SDL_SetRenderVSync(renderer, 1))
	{
		std::fprintf(stderr, "no vsync (%s): presenting at 120 Hz by the clock\n", SDL_GetError());
		vsync = false;
		fps = 120;
	}
	if (!vsync)
		SDL_SetRenderVSync(renderer, 0);
	SDL_Texture *texture = nullptr;

	// Sets up everything that depends on the shape of the picture. Switching
	// while playing also reshapes the window.
	auto apply_aspect = [&](bool reshape_window) {
		view = k2ge_render::aspect_view(aspect);
		interpolator.set_view(view);
		ngpc::set_wide_sprite_margin((view.width - 160 + 1) / 2);
		picture.assign(static_cast<size_t>(view.width) * k2ge_render::LINES, 0xff000000u);
		SDL_SetRenderLogicalPresentation(renderer, view.width, k2ge::SCREEN_HEIGHT,
				view.width > 160 ? SDL_LOGICAL_PRESENTATION_LETTERBOX : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
		if (texture)
			SDL_DestroyTexture(texture);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING, view.width, k2ge::SCREEN_HEIGHT);
		SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
		const std::string title = std::string("Sonic Pocket Adventure") +
				(aspect == k2ge_render::Aspect::Original ? "" : std::string(" (") + k2ge_render::aspect_name(aspect) + ")");
		SDL_SetWindowTitle(window, title.c_str());
		if (reshape_window && !(SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)))
		{
			int w = 0, h = 0;
			SDL_GetWindowSize(window, &w, &h);
			SDL_SetWindowSize(window, (h * view.width + k2ge::SCREEN_HEIGHT / 2) / k2ge::SCREEN_HEIGHT, h);
		}
	};
	apply_aspect(false);

	const SDL_AudioSpec spec = { SDL_AUDIO_S16, AUDIO_CHANNELS, ngpc::Machine::AUDIO_RATE };
	SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (audio)
	{
		// Start with the queue at its working level, so it does not run dry.
		const std::vector<int16_t> silence(AUDIO_QUEUE_TARGET_BYTES / sizeof(int16_t), 0);
		SDL_PutAudioStreamData(audio, silence.data(), static_cast<int>(silence.size() * sizeof(int16_t)));
		SDL_ResumeAudioStreamDevice(audio);
	}
	else
		std::fprintf(stderr, "no audio: %s\n", SDL_GetError());

	// Time. The game is given time by the monotonic clock, one frame whenever a
	// frame's worth has built up, and each refresh of the screen shows the
	// picture for the point reached within the current frame. Audio is the one
	// thing that cannot follow a clock exactly (the sound device has its own),
	// so the clock is trimmed by a fraction of a percent to hold the queued
	// sound at its working level.
	SDL_Gamepad *pad = nullptr;
	const double frame_ns = 1e9 * 102485 / ngpc::Machine::CPU_CLOCK;  // ~59.95 Hz
	const double audio_bytes_per_frame = frame_ns / 1e9 * ngpc::Machine::AUDIO_RATE * AUDIO_CHANNELS * 2;
	double clock = 0;            // time given to the game, ns
	double next_frame = 0;       // when the next frame is due on that clock
	double frame_start = 0;      // when the frame being shown began
	double rate = 1.0;           // clock speed relative to real time
	double queued_average = AUDIO_QUEUE_TARGET_BYTES;
	uint64_t last_real = SDL_GetTicksNS();
	uint64_t next_present = last_real;
	// With vsync the pictures are shown one refresh apart however late or early
	// the driver happens to return, so the game's clock steps by the refresh
	// period (measured, and steered gently back to real time) rather than by
	// the jittery time between calls.
	double period = 1e9 / 60;
	if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
		if (mode->refresh_rate > 20)
			period = 1e9 / mode->refresh_rate;
	double smooth_time = static_cast<double>(last_real);
	uint64_t previous_real = last_real;
	bool running = true;
	PresentStats present_stats, clock_stats;
	const uint64_t started_at = SDL_GetTicksNS();

	// Runs one frame of the game with the buttons as they are now.
	auto run_frame = [&]() {
		uint8_t buttons = read_buttons(SDL_GetKeyboardState(nullptr), pad);
		if (!replay_path.empty())
			buttons = machine->frame_count() < replay.size() ? replay[machine->frame_count()] : 0;
		machine->set_buttons(buttons);
		if (recording)
		{
			// One line per frame, in the format ngpc-run --input reads.
			record << machine->frame_count() << '\t';
			const struct { uint8_t bit; const char *name; } names[] = {
				{ ngpc::BUTTON_UP, "Up" }, { ngpc::BUTTON_DOWN, "Down" }, { ngpc::BUTTON_LEFT, "Left" },
				{ ngpc::BUTTON_RIGHT, "Right" }, { ngpc::BUTTON_A, "A" }, { ngpc::BUTTON_B, "B" },
				{ ngpc::BUTTON_OPTION, "Option" } };
			bool first = true;
			for (const auto &name : names)
				if (buttons & name.bit)
				{
					record << (first ? "" : ",") << name.name;
					first = false;
				}
			record << '\n';
			if (machine->frame_count() % 60 == 0)
				record.flush();
		}
		if (game)
			game->run_frame();
		else
			machine->run_frame();

		std::vector<int16_t> &samples = machine->audio_samples();
		if (audio && !samples.empty())
			SDL_PutAudioStreamData(audio, samples.data(), static_cast<int>(samples.size() * sizeof(int16_t)));
		samples.clear();

		if (smoothing || view.width != 160)
		{
			// A wide view shows the level's own tile maps while the game is in
			// a level, and the machine's screen centred in it otherwise.
			sprite_trace.collect();
			const k2ge_render::Frame &frame = machine->video().frame();
			const bool wide_view = view.width != 160;
			ngpc::LevelRam level_ram;
			if (wide_view)
				level_ram = ngpc::read_level_ram(*machine);
			const bool in_level = wide_view && level_map.build(frame, level_ram, *wide_scene);
			// Where the camera is locked the game keeps the player to the screen
			// and the first boss to a little more: in a wide view they get its width.
			const int extra = view.width - 160;
			ngpc::set_wide_arena(wide_view && ngpc::wide_arena(level_ram), extra / 2, extra - extra / 2);
			interpolator.push(frame, sprite_trace.match(frame.vram.data() + 0x800), in_level ? wide_scene.get() : nullptr);
			if (!smoothing)
				interpolator.render_as_drawn(picture.data());
		}

		// Persist the flash save area when the game has written to it.
		if (machine->frame_count() % 60 == 0 && !fresh_cartridge)
		{
			const std::vector<uint8_t> &cart = machine->cartridge();
			if (!std::equal(saved.begin(), saved.end(), cart.begin() + SAVE_OFFSET))
			{
				saved.assign(cart.begin() + SAVE_OFFSET, cart.begin() + SAVE_OFFSET + SAVE_SIZE);
				std::ofstream(save_file, std::ios::binary)
					.write(reinterpret_cast<const char *>(saved.data()), static_cast<std::streamsize>(saved.size()));
			}
		}
	};

	while (running)
	{
		if (quit_after > 0 && static_cast<double>(SDL_GetTicksNS() - started_at) / 1e9 > quit_after)
			running = false;
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
				running = false;
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
					(event.key.scancode == SDL_SCANCODE_TAB || event.key.scancode == SDL_SCANCODE_F10))
			{
				aspect = k2ge_render::next_aspect(aspect);
				apply_aspect(true);
				save_aspect(aspect);
			}
			else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F9 && recording)
			{
				bookmarks << machine->frame_count() - 1 << std::endl;
				std::printf("bookmarked frame %llu\n", static_cast<unsigned long long>(machine->frame_count() - 1));
			}
			else if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad)
				pad = SDL_OpenGamepad(event.gdevice.which);
			else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && pad &&
					SDL_GetGamepadID(pad) == event.gdevice.which)
			{
				SDL_CloseGamepad(pad);
				pad = nullptr;
			}
		}

		// Advance the clock and run the frames that have come due. A long stall
		// (the window being dragged) is not made up for.
		const uint64_t real = SDL_GetTicksNS();
		uint64_t now = real;
		if (vsync)
		{
			const double measured = static_cast<double>(real - previous_real);
			previous_real = real;
			if (measured > 0.5 * period && measured < 1.5 * period)
				period += 0.02 * (measured - period);
			smooth_time += period;
			if (std::fabs(static_cast<double>(real) - smooth_time) > 5 * period)
				smooth_time = static_cast<double>(real);          // a stall: start again
			else
				smooth_time += 0.02 * (static_cast<double>(real) - smooth_time);
			now = static_cast<uint64_t>(smooth_time);
		}
		else
		{
			// Paced by the clock: the picture is made for the moment it is due
			// on screen, and held back until then, so the time the game takes
			// to run does not show up as uneven spacing.
			if (now > next_present + static_cast<uint64_t>(2e9 / fps))
				next_present = now;
			now = std::max(now, next_present);
		}
		clock_stats.present(now);
		clock += std::clamp(static_cast<double>(now) - static_cast<double>(last_real), 0.0, 3 * frame_ns) * rate;
		last_real = now;
		for (int ran = 0; clock >= next_frame && ran < 3; ++ran)
		{
			run_frame();
			frame_start = next_frame;
			next_frame += frame_ns;
		}
		if (clock - next_frame > 2 * frame_ns)
			clock = next_frame;

		// Show the picture for where the clock is within the current frame.
		const void *pixels;
		if (smoothing)
		{
			interpolator.render(std::clamp((clock - frame_start) / frame_ns, 0.0, 1.0), picture.data());
			pixels = picture.data();
		}
		else
			pixels = view.width != 160 ? picture.data() : machine->framebuffer();
		SDL_UpdateTexture(texture, nullptr, pixels, view.width * 4);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		SDL_RenderTexture(renderer, texture, nullptr, nullptr);
		if (!vsync)
		{
			const uint64_t ready = SDL_GetTicksNS();
			if (next_present > ready)
				SDL_DelayPrecise(next_present - ready);
			next_present += static_cast<uint64_t>(1e9 / fps);
		}
		SDL_RenderPresent(renderer);
		present_stats.present(SDL_GetTicksNS());

		if (vsync && SDL_GetTicksNS() - real < 2000000)
		{
			// A driver that says it waits for the refresh but does not would
			// otherwise spin; nothing needs more than ~500 pictures a second.
			SDL_DelayPrecise(2000000 - (SDL_GetTicksNS() - real));
		}

		// Keep the queued sound at its working level by trimming the clock.
		if (audio)
		{
			const int queued = SDL_GetAudioStreamQueued(audio);
			if (queued > 8 * AUDIO_QUEUE_TARGET_BYTES)
			{
				// Far beyond what the trim can recover (the sound device is
				// not keeping time): drop the backlog rather than let the
				// sound lag the picture more and more.
				SDL_ClearAudioStream(audio);
				const std::vector<int16_t> silence(AUDIO_QUEUE_TARGET_BYTES / sizeof(int16_t), 0);
				SDL_PutAudioStreamData(audio, silence.data(), static_cast<int>(silence.size() * sizeof(int16_t)));
				queued_average = AUDIO_QUEUE_TARGET_BYTES;
			}
			queued_average += 0.02 * (static_cast<double>(queued) - queued_average);
			const double error_frames = (queued_average - AUDIO_QUEUE_TARGET_BYTES) / audio_bytes_per_frame;
			rate = 1.0 - std::clamp(0.003 * error_frames, -0.004, 0.004);
		}
	}

	if (stats)
	{
		present_stats.report("presents");
		clock_stats.report("game clock steps");
		if (audio)
			std::printf("audio: queued %d bytes (target %d), clock rate %.5f\n",
					SDL_GetAudioStreamQueued(audio), AUDIO_QUEUE_TARGET_BYTES, rate);
	}
	game.reset();
	if (pad)
		SDL_CloseGamepad(pad);
	if (audio)
		SDL_DestroyAudioStream(audio);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
