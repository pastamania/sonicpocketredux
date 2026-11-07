#include "ngpc/option_screen.h"

#include "cpu/tlcs900/instruction_hook.h"
#include "cpu/tlcs900/tlcs900.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace ngpc {

namespace {

// The Game Option screen's code (see docs/notes/options-screen.md). It is one task whose
// pointer the code keeps in XIZ; +0x38 is the cursor's row, +0x3C how much is unlocked (0 none,
// 1 the Sound Test, 2 that and the Special Stage) and +0x3D is not 0 once a child (the Sound Test
// or the Special Stage) is being launched.
//
//   0x3CC62F    the end of the setup: the screen is drawn and its task is about to be given
//               the tick state
//   0x3CC642    the tick state: reads the buttons (only 0x4D41, those pressed since the last
//               tick) and acts on the row the cursor is on
//   0x3CCA6E    in the bar's task: CALL 0x23C224 draws the highlight bar at IX, IY
//   0x3CC9EC, 0x3CC9FA, 0x3CCA0A
//               in ShowVal_AutoPOff, which the game calls after a change to a row: each has just
//               loaded XBC with where in the plane's map to draw the value of AUTO POWER OFF
constexpr uint32_t OPTIONS_SETUP_END = 0x3CC62F;
constexpr uint32_t OPTIONS_TICK = 0x3CC642;
constexpr uint32_t OPTIONS_BAR = 0x3CCA6E;
constexpr uint32_t AUTO_POWER_OFF_DRAWS[] = { 0x3CC9EC, 0x3CC9FA, 0x3CCA0A };

constexpr uint32_t TASK_ROW = 0x38, TASK_UNLOCKED = 0x3C, TASK_CHILD = 0x3D;
constexpr uint32_t INPUT_PRESSED = 0x4D41;
constexpr uint8_t UP = 0x01, DOWN = 0x02, LEFT = 0x04, RIGHT = 0x08, BUTTON_A = 0x10, BUTTON_B = 0x20;

// Registers by the index the CPU gives them (XWA, XBC, XDE, XHL, XIX, XIY, XIZ, XSP) and the
// 16-bit forms of the bar's position.
constexpr uint8_t REG_XBC = 1, REG_XIZ = 6;
constexpr uint8_t REG_IY = 0xDD;

// The game's rows, by where its own code has them (0 to 5 are its cursor's rows).
enum Row : uint8_t
{
	Difficulty,
	TimeOut,
	Player,
	DropDash,
	SoundTest,
	SpecialStage,
	FrameRate,
	AspectRatio,
};
constexpr int MAX_ROWS = 8;

bool game_owns(Row row)
{
	return row == Difficulty || row == TimeOut || row == Player || row == SoundTest || row == SpecialStage;
}

// The rows on the screen, in order, for an unlock level. They are placed at tile rows 3, 5, 7 ...
// as the game's are, so SOUND TEST and SPECIAL STAGE, which the game draws itself at tile rows 11
// and 13, are on their own rows when they are shown: the new rows come before, between and after.
int row_list(int unlocked, Row *list)
{
	int n = 0;
	list[n++] = Difficulty;
	list[n++] = TimeOut;
	list[n++] = Player;
	list[n++] = DropDash;
	if (unlocked >= 1)
		list[n++] = SoundTest;
	if (unlocked >= 2)
		list[n++] = SpecialStage;
	list[n++] = FrameRate;
	list[n++] = AspectRatio;
	return n;
}

// Plane 1's map is at 0x9000: 32 words a row; a word is the tile, and the palette from bit 9.
// The screen is 20 tiles wide: a label starts at the second, a value ends at the nineteenth.
constexpr uint32_t PLANE1 = 0x9000;
constexpr uint32_t CHAR_RAM = 0xA000;
constexpr int MAP_ROWS_FIRST = 3;        // the tile row of the first row on the screen; 2 a row
constexpr int LABEL_COLUMN = 1, VALUE_END = 19;
constexpr uint16_t BLANK_TILE = 0x1E;    // the screen's empty tile
constexpr int LABEL_PALETTE = 3, VALUE_PALETTE = 2;
// Where the bar's task has a row's bar: 31 for the first, 16 more for each.
constexpr int BAR_FIRST = 31, BAR_STEP = 16;
// Where a map made for the screen's own drawing code (a width, a height, a word, then the
// words of tiles) is kept for ON and OFF in the cartridge.
constexpr uint32_t MAP_ON = 0x27A240, MAP_OFF = 0x27A248;

// A place in plane 1's map nothing shows (below the 19 rows of the screen), where the game's
// drawing of AUTO POWER OFF's value is sent.
constexpr uint32_t OFF_SCREEN = PLANE1 + 24 * 32 * 2;

// Char RAM has 512 tiles; the screen's picture and bar use up to 0xE4. The font is put after.
constexpr int FONT_FIRST_TILE = 0x120;

// A 5 by 7 font, drawn bold with a shadow (the game's own letters are the lettering of its
// labels). '#' is a lit point.
struct Glyph
{
	char letter;
	const char *rows[7];
};

constexpr Glyph FONT[] = {
	{ ' ', { ".....", ".....", ".....", ".....", ".....", ".....", "....." } },
	{ 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
	{ 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
	{ 'C', { ".####", "#....", "#....", "#....", "#....", "#....", ".####" } },
	{ 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
	{ 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
	{ 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
	{ 'G', { ".####", "#....", "#....", "#.###", "#...#", "#...#", ".####" } },
	{ 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
	{ 'I', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####" } },
	{ 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
	{ 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
	{ 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
	{ 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
	{ 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
	{ 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
	{ 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
	{ 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
	{ 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
	{ 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
	{ 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
	{ 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
	{ 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
	{ 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#" } },
	{ 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
	{ 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
	{ 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
	{ '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
	{ '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
	{ '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
	{ '3', { "####.", "....#", "....#", ".###.", "....#", "....#", "####." } },
	{ '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
	{ '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
	{ '6', { ".###.", "#....", "#....", "####.", "#...#", "#...#", ".###." } },
	{ '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
	{ '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
	{ '9', { ".###.", "#...#", "#...#", ".####", "....#", "....#", ".###." } },
	{ ':', { ".....", "..#..", "..#..", ".....", "..#..", "..#..", "....." } },
	{ '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
	{ '/', { "....#", "....#", "...#.", "..#..", ".#...", "#....", "#...." } },
	{ '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
};
constexpr int FONT_SIZE = static_cast<int>(sizeof(FONT) / sizeof(FONT[0]));

// The colours in a tile (0 is clear): the letter and its shadow.
constexpr int LETTER_COLOUR = 2, SHADOW_COLOUR = 3;

int font_tile(char letter)
{
	for (int i = 0; i < FONT_SIZE; ++i)
		if (FONT[i].letter == letter)
			return FONT_FIRST_TILE + i;
	return FONT_FIRST_TILE;   // a space
}

// The tile of a glyph, as the video chip has it: a word a line, the leftmost point in the top
// two bits.
void glyph_words(const Glyph &glyph, uint16_t words[8])
{
	uint8_t bold[8] = {};
	for (int y = 0; y < 7; ++y)
	{
		uint8_t bits = 0;
		for (int x = 0; x < 5; ++x)
			if (glyph.rows[y][x] == '#')
				bits |= static_cast<uint8_t>(0x80 >> x);
		bold[y] = static_cast<uint8_t>(bits | (bits >> 1));
	}
	for (int y = 0; y < 8; ++y)
	{
		const uint8_t shadow = y > 0 ? static_cast<uint8_t>(bold[y - 1] >> 1) : 0;
		uint16_t word = 0;
		for (int x = 0; x < 8; ++x)
		{
			const uint8_t bit = static_cast<uint8_t>(0x80 >> x);
			const int colour = (bold[y] & bit) ? LETTER_COLOUR : (shadow & bit) ? SHADOW_COLOUR : 0;
			word = static_cast<uint16_t>(word | (colour << (14 - 2 * x)));
		}
		words[y] = word;
	}
}

// What each CPU is doing: the hooks are global (spa-verify runs two machines).
struct State
{
	HostOptions options;
	unsigned seen = 0;             // the generation of the host's options these are from
	Row row = Difficulty;          // where the cursor is
	int bar_y = BAR_FIRST;         // where the bar goes
	int unlocked = -1;             // the unlock level the rows on the screen were drawn for
	bool drawn = false;
	SoundCall sound;
};

bool s_enabled = false;
bool s_installed = false;
HostOptions s_host;
unsigned s_generation = 1;
HostOptions s_menu;
bool s_menu_pending = false;
// The CPU runs on one thread at a time (the host and the recompiled game's thread take turns).
std::unordered_map<const tlcs900_device *, State> s_states;

State &state_of(tlcs900_device &cpu)
{
	return s_states[&cpu];
}

State *existing_state(tlcs900_device &cpu)
{
	const auto it = s_states.find(&cpu);
	return it == s_states.end() ? nullptr : &it->second;
}

void sync_options(State &st)
{
	if (st.seen == s_generation)
		return;
	st.options = s_host;
	st.seen = s_generation;
	st.drawn = false;
}

// ---- Drawing -----------------------------------------------------------------------------

void put(tlcs900_device &cpu, int row, int column, uint16_t word)
{
	cpu.WRMEMW(PLANE1 + static_cast<uint32_t>(row * 32 + column) * 2, word);
}

uint16_t tile_word(int tile, int palette)
{
	return static_cast<uint16_t>(tile | (palette << 9));
}

void clear_row(tlcs900_device &cpu, int row)
{
	for (int column = 0; column < 20; ++column)
		put(cpu, row, column, tile_word(BLANK_TILE, LABEL_PALETTE));
}

void put_text(tlcs900_device &cpu, int row, int column, const char *text, int palette)
{
	for (; *text; ++text, ++column)
		put(cpu, row, column, tile_word(font_tile(*text), palette));
}

void put_value_text(tlcs900_device &cpu, int row, const char *text)
{
	put_text(cpu, row, VALUE_END - static_cast<int>(std::strlen(text)), text, VALUE_PALETTE);
}

// A picture of the game's own (a map as its drawing code reads them) at the right of a row.
void put_value_map(tlcs900_device &cpu, int row, uint32_t map)
{
	const int width = cpu.RDMEM(map);
	for (int i = 0; i < width; ++i)
		put(cpu, row, VALUE_END - width + i, cpu.RDMEMW(map + 4 + 2 * static_cast<uint32_t>(i)));
}

const char *row_label(Row row)
{
	switch (row)
	{
	case DropDash: return "DROP DASH";
	case FrameRate: return "FRAME RATE";
	case AspectRatio: return "ASPECT";
	default: return "";
	}
}

const char *refresh_text(int refresh)
{
	return refresh == 120 ? "120 HZ" : refresh == 90 ? "90 HZ" : "60 HZ";
}

const char *aspect_text(k2ge_render::Aspect aspect)
{
	return aspect == k2ge_render::Aspect::Wide16x9 ? "16:9" :
			aspect == k2ge_render::Aspect::Wide16x10 ? "16:10" : "ORIGINAL";
}

void load_font(tlcs900_device &cpu)
{
	for (int i = 0; i < FONT_SIZE; ++i)
	{
		uint16_t words[8];
		glyph_words(FONT[i], words);
		for (int y = 0; y < 8; ++y)
			cpu.WRMEMW(CHAR_RAM + static_cast<uint32_t>(FONT_FIRST_TILE + i) * 16 + static_cast<uint32_t>(y) * 2, words[y]);
	}
}

void draw_rows(tlcs900_device &cpu, State &st, int unlocked)
{
	Row list[MAX_ROWS];
	const int count = row_list(unlocked, list);
	load_font(cpu);
	// The rows from the fourth on are the game's (SOUND TEST, SPECIAL STAGE, drawn as it does at
	// the rows they are on here) or ours; the old AUTO POWER OFF and EXIT are cleared with them.
	for (int slot = 3; slot < MAX_ROWS; ++slot)
	{
		const int row = MAP_ROWS_FIRST + 2 * slot;
		if (slot < count && game_owns(list[slot]))
			continue;
		clear_row(cpu, row);
		if (slot >= count)
			continue;
		put_text(cpu, row, LABEL_COLUMN, row_label(list[slot]), LABEL_PALETTE);
		switch (list[slot])
		{
		case DropDash: put_value_map(cpu, row, st.options.drop_dash ? MAP_ON : MAP_OFF); break;
		case FrameRate: put_value_text(cpu, row, refresh_text(st.options.refresh)); break;
		case AspectRatio: put_value_text(cpu, row, aspect_text(st.options.aspect)); break;
		default: break;
		}
	}
	st.unlocked = unlocked;
	st.drawn = true;
}

// ---- Changing a row ----------------------------------------------------------------------

constexpr int REFRESH_CHOICES[] = { 60, 90, 120 };
constexpr k2ge_render::Aspect ASPECT_CHOICES[] = {
	k2ge_render::Aspect::Wide16x9, k2ge_render::Aspect::Wide16x10, k2ge_render::Aspect::Original };

template <typename T, size_t N>
int index_of(const T (&choices)[N], const T &value)
{
	for (size_t i = 0; i < N; ++i)
		if (choices[i] == value)
			return static_cast<int>(i);
	return 0;
}

// A host row steps by `direction` (a choice of three wraps round). Returns whether it changed.
bool step_row(State &st, Row row, int direction)
{
	switch (row)
	{
	case DropDash:
		st.options.drop_dash = !st.options.drop_dash;
		return true;
	case FrameRate:
	{
		const int next = (index_of(REFRESH_CHOICES, st.options.refresh) + direction + 3) % 3;
		st.options.refresh = REFRESH_CHOICES[next];
		return true;
	}
	case AspectRatio:
	{
		const int next = (index_of(ASPECT_CHOICES, st.options.aspect) + direction + 3) % 3;
		st.options.aspect = ASPECT_CHOICES[next];
		return true;
	}
	default:
		return false;
	}
}

// What the game's own code is shown for a row of its own: the row it counts it as, and the press
// that does what the player asked.
int game_row(Row row)
{
	switch (row)
	{
	case Difficulty: return 0;
	case TimeOut: return 1;
	case Player: return 2;
	case SoundTest: return 4;
	default: return 5;
	}
}

// ---- Hooks -------------------------------------------------------------------------------

void on_setup_end(tlcs900_device &cpu, void *)
{
	if (!s_enabled)
		return;
	State &st = state_of(cpu);
	sync_options(st);
	// The screen is new: the game puts the cursor on the first row.
	st.row = Difficulty;
	st.bar_y = BAR_FIRST;
	draw_rows(cpu, st, std::min<int>(cpu.RDMEM(*cpu.get_reg32_current(REG_XIZ) + TASK_UNLOCKED), 2));
}

void on_tick(tlcs900_device &cpu, void *)
{
	State *found = s_enabled ? &state_of(cpu) : existing_state(cpu);
	if (!found)
		return;
	State &st = *found;
	// Back from the cursor's sound: its press has been handed over, so the tick goes on.
	if (resume_sound(cpu, st.sound, OPTIONS_TICK) || !s_enabled)
		return;

	const uint32_t task = *cpu.get_reg32_current(REG_XIZ);
	if (cpu.RDMEM(task + TASK_CHILD) != 0)
		return;   // the Sound Test or the Special Stage is being started: the game's own
	sync_options(st);
	const int unlocked = std::min<int>(cpu.RDMEM(task + TASK_UNLOCKED), 2);
	Row list[MAX_ROWS];
	const int count = row_list(unlocked, list);
	int slot = 0;
	for (int i = 0; i < count; ++i)
		if (list[i] == st.row)
			slot = i;

	const uint8_t pressed = cpu.RDMEM(INPUT_PRESSED);
	uint8_t hand_over = 0;
	bool beep = false;
	Row row = list[slot];
	if (pressed & UP)
	{
		slot = (slot + count - 1) % count;
		beep = true;
	}
	else if (pressed & DOWN)
	{
		slot = (slot + 1) % count;
		beep = true;
	}
	else if (pressed & BUTTON_B)
	{
		hand_over = BUTTON_B;   // the game's own way out: it saves, plays its sound and leaves
	}
	else if (game_owns(row))
	{
		if (row == SoundTest || row == SpecialStage)
			hand_over = pressed & BUTTON_A;
		else
			hand_over = (pressed & LEFT) ? LEFT : (pressed & (RIGHT | BUTTON_A)) ? RIGHT : 0;
	}
	else if (pressed & (LEFT | RIGHT | BUTTON_A))
	{
		if (step_row(st, row, (pressed & LEFT) ? -1 : 1))
		{
			beep = true;
			st.drawn = false;   // the value is drawn again below
			s_menu = st.options;
			s_menu_pending = true;
		}
	}
	row = list[slot];
	st.row = row;
	st.bar_y = BAR_FIRST + BAR_STEP * slot;
	if (hand_over && game_owns(row))
		cpu.WRMEM(task + TASK_ROW, static_cast<uint8_t>(game_row(row)));
	else if (hand_over)
		cpu.WRMEM(task + TASK_ROW, 0);
	// The game is shown the press asked of it and no other: its own cursor does not move.
	cpu.WRMEM(INPUT_PRESSED, hand_over);

	if (!st.drawn || st.unlocked != unlocked)
		draw_rows(cpu, st, unlocked);
	if (beep)
		call_sound(cpu, st.sound, OPTIONS_TICK, 0x82);
}

void on_bar(tlcs900_device &cpu, void *)
{
	if (!s_enabled)
		return;
	const State *st = existing_state(cpu);
	if (st && st->drawn)
		*cpu.get_reg16_current(REG_IY) = static_cast<uint16_t>(st->bar_y);
}

// The game draws AUTO POWER OFF's value after a change to another row; it goes where nothing
// is seen, as the row it was on now holds a row of ours.
void on_auto_power_off_draw(tlcs900_device &cpu, void *)
{
	if (!s_enabled)
		return;
	const State *st = existing_state(cpu);
	if (st && st->drawn)
		*cpu.get_reg32_current(REG_XBC) = OFF_SCREEN;
}

} // namespace

void set_option_screen(bool enabled)
{
	s_enabled = enabled;
	if (enabled && !s_installed)
	{
		tlcs900_hook::add(OPTIONS_SETUP_END, on_setup_end);
		tlcs900_hook::add(OPTIONS_TICK, on_tick);
		tlcs900_hook::add(OPTIONS_BAR, on_bar);
		for (const uint32_t pc : AUTO_POWER_OFF_DRAWS)
			tlcs900_hook::add(pc, on_auto_power_off_draw);
		s_installed = true;
	}
}

bool option_screen_enabled()
{
	return s_enabled;
}

void set_host_options(const HostOptions &options)
{
	s_host = options;
	++s_generation;
}

bool take_option_changes(HostOptions &options)
{
	if (!s_menu_pending)
		return false;
	s_menu_pending = false;
	options = s_menu;
	s_host = s_menu;
	return true;
}

} // namespace ngpc
