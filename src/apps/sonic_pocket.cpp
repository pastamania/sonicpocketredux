// sonic-pocket: plays the recompiled Sonic Pocket Adventure in an SDL3 window.
//
//   sonic-pocket <cartridge> [--interpreter] [--japanese] [--scale N]
//                [--aspect original|16:10|16:9] [--fps N] [--no-smoothing]
//                [--scale-mode sharp|integer|nearest] [--frame-delay auto|off|MS]
//                [--audio-latency FRAMES] [--window WxH]
//                [--record input.txt | --replay input.txt] [--stats]
//                [--screenshot file.png [--screenshot-at FRAME]]
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
// How the picture is scaled to the window (--scale-mode, F8 cycles it, and the
// choice is remembered): `sharp` fills the window with even, crisp pixels at
// any size (SDL's pixel-art scaling), `integer` uses whole multiples only, with
// bars, and `nearest` is plain nearest-neighbour scaling (whole multiples for
// the original screen, any size for a wide one: uneven pixels). F11 or
// Alt+Enter toggles borderless fullscreen. The window is made with a high pixel
// density, so on a display that has one the picture is drawn in its own pixels.
//
// Latency (see --stats for the numbers). The sound is kept `--audio-latency`
// frames deep (default 2; it is raised by itself if the device runs dry). With
// vsync, `--frame-delay` waits before reading the buttons and running the game's
// frames so that they are read as late in the refresh as the work allows: `auto`
// (the default) moves the time the loop spent waiting for the screen to the
// front, keeping a margin of about 3 ms that widens after a refresh is missed.
// It does something only where a refresh is 12 ms or longer (60 Hz): on a faster
// display there is too little to gain for the risk of a missed refresh. `off`
// never waits; a number waits that many milliseconds every frame.
//
// Controls: arrow keys or D-pad/left stick, Z / gamepad south = A,
// X / gamepad east = B, Enter / gamepad start = Option, Escape quits.
//
// The game's save data lives in the cartridge's flash chip (the last 32 KiB of
// the ROM); it is written to the SDL preferences folder whenever it changes.

#include "common/png.h"
#include "ngpc/k2ge_interp.h"
#include "ngpc/level_map.h"
#include "ngpc/machine.h"
#include "ngpc/sprite_trace.h"
#include "ngpc/wide_levels.h"
#include "ngpc/wide_sprites.h"
#include "recomp_rt/game.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// A list of measurements in milliseconds, reported as mean and percentiles.
class Samples
{
public:
	void add(double value) { m_values.push_back(value); }
	size_t size() const { return m_values.size(); }

	void report(const char *label) const
	{
		if (m_values.empty())
			return;
		std::vector<double> sorted = m_values;
		std::sort(sorted.begin(), sorted.end());
		double mean = 0;
		for (double v : sorted)
			mean += v;
		mean /= static_cast<double>(sorted.size());
		auto percentile = [&](double p) { return sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]; };
		std::fprintf(stderr, "%s: %zu samples; ms: mean %.1f, min %.1f, p50 %.1f, p95 %.1f, max %.1f\n",
				label, sorted.size(), mean, sorted.front(), percentile(0.5), percentile(0.95), sorted.back());
	}

private:
	std::vector<double> m_values;
};

enum class ScaleMode { Sharp, Integer, Nearest };

const char *scale_mode_name(ScaleMode mode)
{
	return mode == ScaleMode::Sharp ? "sharp" : mode == ScaleMode::Integer ? "integer" : "nearest";
}

bool parse_scale_mode(const std::string &text, ScaleMode &mode)
{
	for (ScaleMode candidate : { ScaleMode::Sharp, ScaleMode::Integer, ScaleMode::Nearest })
		if (text == scale_mode_name(candidate))
		{
			mode = candidate;
			return true;
		}
	return false;
}

ScaleMode next_scale_mode(ScaleMode mode)
{
	return mode == ScaleMode::Sharp ? ScaleMode::Integer : mode == ScaleMode::Integer ? ScaleMode::Nearest : ScaleMode::Sharp;
}

// "auto" is -1, "off" 0, a number of milliseconds to wait every frame.
bool parse_frame_delay(const std::string &text, double &milliseconds)
{
	if (text == "auto")
		milliseconds = -1;
	else if (text == "off")
		milliseconds = 0;
	else
	{
		char *end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		if (text.empty() || *end || value < 0 || value > 15)
			return false;
		milliseconds = value;
	}
	return true;
}

constexpr int AUDIO_LATENCY_DEFAULT = 2;     // frames of sound kept queued
constexpr int AUDIO_LATENCY_MAX = 6;
// The automatic frame delay is for refresh periods at least this long (60 Hz).
constexpr double AUTO_DELAY_MIN_PERIOD_NS = 12e6;

constexpr size_t SAVE_OFFSET = 0x1F8000;   // cartridge offset of the flash save area
constexpr size_t SAVE_SIZE = 0x8000;
constexpr int AUDIO_CHANNELS = 2;

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

// What is remembered between runs, in a file of `key=value` lines. The aspect
// and the scale mode follow the keys that change them; the frame delay and the
// audio latency are only read (edit the file, or give the options for one run).
struct Settings
{
	k2ge_render::Aspect aspect = k2ge_render::Aspect::Original;
	ScaleMode scale_mode = ScaleMode::Sharp;
	double frame_delay = -1;                      // see parse_frame_delay
	int audio_latency = AUDIO_LATENCY_DEFAULT;    // frames
};

Settings load_settings()
{
	Settings settings;
	std::ifstream file(settings_path());
	for (std::string line; std::getline(file, line);)
	{
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
		if (key == "aspect")
			k2ge_render::parse_aspect(value.c_str(), settings.aspect);
		else if (key == "scale-mode")
			parse_scale_mode(value, settings.scale_mode);
		else if (key == "frame-delay")
			parse_frame_delay(value, settings.frame_delay);
		else if (key == "audio-latency")
			settings.audio_latency = std::clamp(std::atoi(value.c_str()), 1, AUDIO_LATENCY_MAX);
	}
	return settings;
}

void save_settings(const Settings &settings)
{
	std::ofstream file(settings_path());
	file << "aspect=" << k2ge_render::aspect_name(settings.aspect) << "\n";
	file << "scale-mode=" << scale_mode_name(settings.scale_mode) << "\n";
	if (settings.frame_delay < 0)
		file << "frame-delay=auto\n";
	else if (settings.frame_delay == 0)
		file << "frame-delay=off\n";
	else
		file << "frame-delay=" << settings.frame_delay << "\n";
	file << "audio-latency=" << settings.audio_latency << "\n";
}

uint8_t read_buttons(const bool *keys, SDL_Gamepad *pad)
{
	uint8_t buttons = 0;
	// Alt+Enter is the fullscreen key, not the Option button.
	const bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
	auto pressed = [&](SDL_Scancode key, SDL_GamepadButton button) {
		return keys[key] || (pad && SDL_GetGamepadButton(pad, button));
	};
	if (pressed(SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP)) buttons |= ngpc::BUTTON_UP;
	if (pressed(SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) buttons |= ngpc::BUTTON_DOWN;
	if (pressed(SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) buttons |= ngpc::BUTTON_LEFT;
	if (pressed(SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) buttons |= ngpc::BUTTON_RIGHT;
	if (pressed(SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= ngpc::BUTTON_A;
	if (pressed(SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_EAST)) buttons |= ngpc::BUTTON_B;
	if ((keys[SDL_SCANCODE_RETURN] && !alt) || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)))
		buttons |= ngpc::BUTTON_OPTION;
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
	// What was chosen last time, which the options below override for this run.
	Settings stored = load_settings();
	k2ge_render::Aspect aspect = stored.aspect;
	ScaleMode scale_mode = stored.scale_mode;
	double frame_delay = stored.frame_delay;
	int audio_latency = stored.audio_latency;
	int window_width = 0, window_height = 0;   // 0: from --scale
	fs::path record_path, replay_path;
	fs::path screenshot_path;
	uint64_t screenshot_at = 600;              // game frame
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
		}
		else if (arg == "--widescreen")
			aspect = k2ge_render::Aspect::Wide16x9;
		else if (arg == "--scale-mode" && i + 1 < argc)
		{
			if (!parse_scale_mode(argv[++i], scale_mode))
			{
				std::fprintf(stderr, "unknown scale mode %s (sharp, integer or nearest)\n", argv[i]);
				return 2;
			}
		}
		else if (arg == "--frame-delay" && i + 1 < argc)
		{
			if (!parse_frame_delay(argv[++i], frame_delay))
			{
				std::fprintf(stderr, "bad frame delay %s (auto, off or milliseconds up to 15)\n", argv[i]);
				return 2;
			}
		}
		else if (arg == "--audio-latency" && i + 1 < argc)
			audio_latency = std::clamp(std::atoi(argv[++i]), 1, AUDIO_LATENCY_MAX);
		else if (arg == "--window" && i + 1 < argc)
		{
			if (std::sscanf(argv[++i], "%dx%d", &window_width, &window_height) != 2 || window_width < 160 || window_height < 152)
			{
				std::fprintf(stderr, "bad window size %s (WIDTHxHEIGHT)\n", argv[i]);
				return 2;
			}
		}
		else if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
		else if (arg == "--screenshot-at" && i + 1 < argc) screenshot_at = std::stoull(argv[++i]);
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
				"                    [--scale-mode sharp|integer|nearest] [--frame-delay auto|off|MS]\n"
				"                    [--audio-latency FRAMES] [--window WxH]\n"
				"                    [--record input.txt | --replay input.txt] [--stats]\n"
				"                    [--screenshot file.png [--screenshot-at FRAME]]\n");
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
	k2ge_render::View view = k2ge_render::aspect_view(aspect);
	k2ge_render::Interpolator interpolator(view);
	ngpc::SpriteTrace sprite_trace(*machine);
	ngpc::LevelMap level_map(machine->cartridge());
	auto wide_scene = std::make_unique<k2ge_render::WideScene>();
	std::vector<uint32_t> picture;

	// A high pixel density window is drawn in the display's own pixels; without
	// it a display that has more pixels than points scales the picture up itself,
	// and softens the edges of every pixel.
	SDL_Window *window = SDL_CreateWindow("Sonic Pocket Adventure",
			window_width ? window_width : view.width * scale, window_height ? window_height : k2ge::SCREEN_HEIGHT * scale,
			SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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

	// How the picture is fitted to the window (see the header comment): sharp
	// fills the window with SDL's pixel-art scaling, integer uses whole multiples,
	// and nearest is the plain scaling this started with.
	auto apply_scale_mode = [&]() {
		SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_LETTERBOX;
		SDL_ScaleMode filter = SDL_SCALEMODE_NEAREST;
		if (scale_mode == ScaleMode::Sharp)
			filter = SDL_SCALEMODE_PIXELART;
		else if (scale_mode == ScaleMode::Integer || view.width == 160)
			presentation = SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
		SDL_SetRenderLogicalPresentation(renderer, view.width, k2ge::SCREEN_HEIGHT, presentation);
		if (texture)
			SDL_SetTextureScaleMode(texture, filter);
	};

	// Sets up everything that depends on the shape of the picture. Switching
	// while playing also reshapes the window.
	auto apply_aspect = [&](bool reshape_window) {
		view = k2ge_render::aspect_view(aspect);
		interpolator.set_view(view);
		ngpc::set_wide_sprite_margin((view.width - 160 + 1) / 2);
		if (view.width == 160)
			ngpc::extend_level_ends(*machine, ngpc::LevelRam(), false);   // the cartridge as it was made
		picture.assign(static_cast<size_t>(view.width) * k2ge_render::LINES, 0xff000000u);
		if (texture)
			SDL_DestroyTexture(texture);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING, view.width, k2ge::SCREEN_HEIGHT);
		apply_scale_mode();
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

	// How much sound is kept queued ahead of the device, in the game's frames: the
	// sound reaches the speakers that long after it is made, so it is kept short,
	// and raised by itself if the device runs dry (see the loop).
	const double frame_ns = 1e9 * 102485 / ngpc::Machine::CPU_CLOCK;  // ~59.95 Hz
	const double audio_bytes_per_frame = frame_ns / 1e9 * ngpc::Machine::AUDIO_RATE * AUDIO_CHANNELS * 2;
	int audio_frames = audio_latency;
	int audio_raises = 0, audio_underruns = 0;
	auto audio_target = [&]() { return static_cast<int>(audio_bytes_per_frame * audio_frames) & ~3; };
	int audio_target_bytes = audio_target();

	// The device asks for its sound in chunks of this many sample frames (about
	// 10 ms); the default is several times that on some systems.
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
	const SDL_AudioSpec spec = { SDL_AUDIO_S16, AUDIO_CHANNELS, ngpc::Machine::AUDIO_RATE };
	SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (audio)
	{
		// Start with the queue at its working level, so it does not run dry.
		const std::vector<int16_t> silence(static_cast<size_t>(audio_target_bytes) / sizeof(int16_t), 0);
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
	double clock = 0;            // time given to the game, ns
	double next_frame = 0;       // when the next frame is due on that clock
	double frame_start = 0;      // when the frame being shown began
	double rate = 1.0;           // clock speed relative to real time
	double queued_average = audio_target_bytes;
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

	// Input latency: when each press arrived (the event's own time), until the
	// game frame that reads it has run and its picture has been presented. This is
	// the part of the delay the host controls; the game itself reads the buttons
	// once a tick (every second frame) and moves on that tick.
	std::vector<uint64_t> input_waiting;   // pressed, not yet read by a frame
	std::vector<uint64_t> input_read;      // read by a frame run in this pass of the loop
	Samples input_latency, audio_queue_ms;

	// Late sampling (--frame-delay): how long the loop waits at the top of each
	// pass, before it reads the buttons, so that the work finishes just before the
	// refresh instead of well before it. `delay_ms` follows the time the last passes
	// spent waiting for the screen (inside the render calls, in ms): it is
	// raised by a fifth of the waiting beyond a margin and lowered, faster, when
	// there was less than the margin, so the loop stays locked to the refresh with
	// about 3 ms to spare. A pass that missed a refresh costs a wider margin for a
	// while (a simulation of this against a blocking present, with work of 3-5 ms
	// and the odd spike, missed one refresh in about a thousand and read the
	// buttons 3-10 ms nearer to the refresh than without).
	double delay_ms = frame_delay > 0 ? frame_delay : 0;
	double margin_extra_ms = 0;
	int passes = 0;
	uint64_t pass_end = SDL_GetTicksNS();
	Samples delay_samples;

	// Runs one frame of the game with the buttons as they are now.
	auto run_frame = [&]() {
		input_read.insert(input_read.end(), input_waiting.begin(), input_waiting.end());
		input_waiting.clear();
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
			// The level's last 64 pixels are unfinished: in a wide view they get
			// the same floor as before them, and are put back for the original.
			ngpc::extend_level_ends(*machine, level_ram, wide_view);
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

	// The keys that move the game, for the input latency.
	auto is_game_key = [](SDL_Scancode key) {
		return key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN || key == SDL_SCANCODE_LEFT ||
				key == SDL_SCANCODE_RIGHT || key == SDL_SCANCODE_Z || key == SDL_SCANCODE_X || key == SDL_SCANCODE_RETURN;
	};

	while (running)
	{
		if (quit_after > 0 && static_cast<double>(SDL_GetTicksNS() - started_at) / 1e9 > quit_after)
			running = false;
		// Wait before reading the buttons (see delay_ms). Only with vsync, when
		// there is a refresh to be late for.
		if (vsync && delay_ms > 0.3)
		{
			const uint64_t wake = pass_end + static_cast<uint64_t>(delay_ms * 1e6);
			const uint64_t at = SDL_GetTicksNS();
			if (wake > at)
				SDL_DelayPrecise(wake - at);
		}
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && is_game_key(event.key.scancode) &&
					!(event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT)))
				input_waiting.push_back(event.key.timestamp);
			else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
				input_waiting.push_back(event.gbutton.timestamp);

			if (event.type == SDL_EVENT_QUIT)
				running = false;
			else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
				running = false;
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
					(event.key.scancode == SDL_SCANCODE_TAB || event.key.scancode == SDL_SCANCODE_F10))
			{
				aspect = k2ge_render::next_aspect(aspect);
				apply_aspect(true);
				stored.aspect = aspect;
				save_settings(stored);
			}
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F8)
			{
				scale_mode = next_scale_mode(scale_mode);
				apply_scale_mode();
				stored.scale_mode = scale_mode;
				save_settings(stored);
				std::printf("scale mode: %s\n", scale_mode_name(scale_mode));
			}
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
					(event.key.scancode == SDL_SCANCODE_F11 ||
					(event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
				SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
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
		// From here to the end of the present is where the loop waits for the
		// screen (when there is vsync).
		const uint64_t render_start = SDL_GetTicksNS();
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
		if (!screenshot_path.empty() && machine->frame_count() >= screenshot_at)
		{
			// The window as it will be shown, in the display's own pixels.
			SDL_Surface *shot = SDL_RenderReadPixels(renderer, nullptr);
			SDL_Surface *argb = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888) : nullptr;
			if (argb)
			{
				std::vector<uint32_t> pixels_out(static_cast<size_t>(argb->w) * argb->h);
				for (int y = 0; y < argb->h; y++)
					std::copy_n(reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(argb->pixels) + y * argb->pitch),
							argb->w, pixels_out.begin() + static_cast<size_t>(y) * argb->w);
				std::printf("screenshot %s: %dx%d pixels\n", screenshot_path.string().c_str(), argb->w, argb->h);
				write_png(screenshot_path, pixels_out.data(), argb->w, argb->h);
			}
			else
				std::fprintf(stderr, "cannot read the picture back: %s\n", SDL_GetError());
			SDL_DestroySurface(argb);
			SDL_DestroySurface(shot);
			running = false;
		}
		SDL_RenderPresent(renderer);
		const uint64_t shown = SDL_GetTicksNS();
		present_stats.present(shown);
		if (shown - started_at > 1000000000ull)
			for (uint64_t pressed : input_read)
				if (shown >= pressed)
					input_latency.add(static_cast<double>(shown - pressed) / 1e6);
		input_read.clear();

		// Late sampling: what the last pass waited for the screen sets how long the
		// next waits before it starts.
		++passes;
		if (vsync && frame_delay < 0 && passes > 30 && period >= AUTO_DELAY_MIN_PERIOD_NS)
		{
			const double period_ms = period / 1e6;
			const double block_ms = static_cast<double>(shown - render_start) / 1e6;
			const double pass_ms = static_cast<double>(shown - pass_end) / 1e6;
			if (pass_ms > 1.4 * period_ms)
			{
				// A refresh was missed: back off, and keep a wider margin for a while.
				delay_ms = std::max(0.0, delay_ms - 3.0);
				margin_extra_ms = std::min(margin_extra_ms + 1.0, 6.0);
			}
			else
			{
				const double spare = block_ms - (3.0 + margin_extra_ms);
				delay_ms += spare > 0 ? std::min(0.5, 0.2 * spare) : 1.5 * spare;
				margin_extra_ms = std::max(0.0, margin_extra_ms - 0.0005);
			}
			delay_ms = std::clamp(delay_ms, 0.0, period_ms - 2.0);
		}
		if (vsync && passes > 120)
			delay_samples.add(delay_ms);

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
			if (shown - started_at > 1000000000ull)
			{
				audio_queue_ms.add(queued * 1000.0 / (static_cast<double>(ngpc::Machine::AUDIO_RATE) * AUDIO_CHANNELS * 2));
				if (queued == 0)
				{
					// The device ran dry: keep a deeper queue from here on.
					++audio_underruns;
					if (audio_frames < AUDIO_LATENCY_MAX)
					{
						++audio_frames;
						++audio_raises;
						audio_target_bytes = audio_target();
					}
				}
			}
			if (queued > 8 * audio_target_bytes)
			{
				// Far beyond what the trim can recover (the sound device is
				// not keeping time): drop the backlog rather than let the
				// sound lag the picture more and more.
				SDL_ClearAudioStream(audio);
				const std::vector<int16_t> silence(static_cast<size_t>(audio_target_bytes) / sizeof(int16_t), 0);
				SDL_PutAudioStreamData(audio, silence.data(), static_cast<int>(silence.size() * sizeof(int16_t)));
				queued_average = audio_target_bytes;
			}
			queued_average += 0.02 * (static_cast<double>(queued) - queued_average);
			const double error_frames = (queued_average - audio_target_bytes) / audio_bytes_per_frame;
			rate = 1.0 - std::clamp(0.003 * error_frames, -0.004, 0.004);
		}
		pass_end = SDL_GetTicksNS();
	}

	if (stats)
	{
		present_stats.report("presents");
		clock_stats.report("game clock steps");
		input_latency.report("input to picture presented (the buttons' press to the present returning; the game's own tick comes on top)");
		if (audio)
		{
			audio_queue_ms.report("audio queued");
			std::printf("audio: queued %d bytes (target %d, %d frames: raised %d times, %d underruns), clock rate %.5f\n",
					SDL_GetAudioStreamQueued(audio), audio_target_bytes, audio_frames, audio_raises, audio_underruns, rate);
		}
		if (vsync)
		{
			delay_samples.report("frame delay (ms waited before reading the buttons)");
			std::printf("frame delay: %s, margin widened by %.1f ms\n",
					frame_delay < 0 ? (period >= AUTO_DELAY_MIN_PERIOD_NS ? "auto" : "auto (not used: the refresh is under 12 ms)")
							: frame_delay == 0 ? "off" : "fixed", margin_extra_ms);
		}
		int out_w = 0, out_h = 0;
		SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
		std::printf("window: pixel density %.2f, output %dx%d pixels, scale mode %s, %s\n", SDL_GetWindowPixelDensity(window),
				out_w, out_h, scale_mode_name(scale_mode), vsync ? "vsync" : "paced by the clock");
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
