// Reads the cartridge's level data to build the tile maps a wide view needs.
//
// The hardware keeps 256 pixels of each scroll plane's map and the game fills
// it just in time (about 0..36 pixels beyond the screen, in blocks of four
// columns), so a view wider than the screen has nothing to draw at its edges.
// The levels themselves are plain tables in the cartridge, so the host reads
// them directly.
//
// A level header, whose address the game keeps in RAM, holds:
//
//   +0x00 word  width in blocks
//   +0x02 word  height in blocks
//   +0x0A long  layout of plane 2 (the level): width*height block numbers
//   +0x0E long  block table: 32 bytes a block, 4x4 tile map entries, row by row
//   +0x1A long  layout of plane 1 (a sparse foreground), same size and blocks
//
// A block is 32x32 pixels. The camera's y is measured from the bottom of the
// level. The hardware map is a ring: tile column c of the world is ring column
// c & 31, and tile row r is ring row (r + k) & 31 with k fixed for each level.
// Nothing here is assumed beyond that: every frame the decoded tiles are
// compared with what the game has drawn on screen, and the wide view is used
// only while they agree (so it is not used on the title screen, a menu, a
// special stage or any other screen the decoder does not describe).
//
// Specific to this cartridge: the RAM addresses belong to its code.
#pragma once

#include "ngpc/k2ge_render.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ngpc {

constexpr uint32_t LEVEL_HEADER_PTR = 0x508E;   // long: address of the level header in the cartridge
constexpr uint32_t LEVEL_CAMERA_X = 0x506C;     // word
constexpr uint32_t LEVEL_CAMERA_Y = 0x506E;     // word
constexpr uint32_t LEVEL_CAMERA_X_START = 0x507A; // word: the camera's limits
constexpr uint32_t LEVEL_CAMERA_X_END = 0x507C;   // word
constexpr uint32_t LEVEL_NUMBER = 0x67D2;         // byte: 0..12

// The scheduler's task list (see task-scheduler.md): the head record, and the
// offsets of a record's function pointer and next-record pointer.
constexpr uint32_t TASK_LIST_HEAD = 0x401A;
constexpr uint32_t TASK_FUNCTION = 0x00;
constexpr uint32_t TASK_NEXT = 0x06;
// The zone title card (ShowTitleCard and the tasks it starts) lives in this
// range of the cartridge; the card is on screen while one of them is running.
constexpr uint32_t TITLE_CARD_FIRST = 0x3CF75A;
constexpr uint32_t TITLE_CARD_END = 0x3CFA96;

// The code of the first and second zone's bosses (Boss0_Load on to Boss1's last
// routine); a task running it is a boss, or a part of one, with its x at +0x12.
constexpr uint32_t BOSS_CODE_FIRST = 0x3A0EDA;
constexpr uint32_t BOSS_CODE_END = 0x3A2000;
constexpr int BOSS_TASKS = 8;
constexpr int CARD_TASKS = 4;

// What the game holds in RAM that locates the level, read at the end of a frame.
struct LevelRam
{
	uint32_t header = 0;
	int camera_x = 0, camera_y = 0;
	int camera_x_start = 0, camera_x_end = 0;
	bool title_card = false;        // the zone's title card is showing
	// The records of the card's tasks, which own the sprites of the card (as
	// opposed to the level's own: the player, the HUD).
	uint32_t card_owner[CARD_TASKS] = {};
	int card_owner_count = 0;
	int level = -1;                 // the level being played (0..12)
	// The x, on screen (the level's x less the camera's), of the tasks of the
	// bosses above.
	int boss_x[BOSS_TASKS] = {};
	int boss_count = 0;
};

// Whether the camera is locked where a wide view takes the space over: the end
// of an act 1 (the camera stops at the level's end for the capsule) and the
// boss arenas of the first and second zones. There the original keeps the
// picture and the player to the 160 pixels of the screen; a wide view shows
// the level beyond instead and lets the player and the first boss use the
// whole width. Levels, by their numbers: 0, 2, 4, 6 and 10 are act 1s; 1 and 3
// end in the first and second zone's boss. The third zone's boss (5) is at the
// end of its level and its camera is not locked, and the other levels' ends
// are left as the game has them.
bool wide_arena(const LevelRam &ram);

// Whether the level is one of those, locked or not.
bool arena_level(const LevelRam &ram);

class Machine;

// Reads those from the machine's RAM.
LevelRam read_level_ram(Machine &machine);

class LevelMap
{
public:
	explicit LevelMap(const std::vector<uint8_t> &cartridge) : m_rom(cartridge) {}

	// Fills `scene` for the frame the machine has just drawn. Returns false,
	// with the scene inactive, when the picture is not a level the decoder
	// reproduces.
	bool build(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene);

	// Of the tiles on screen, the share the decoder agreed with in the last
	// call (0..1), for tooling.
	double last_agreement() const { return m_agreement; }

	// How many tiles the game has changed from the level data (a collected
	// ring is wiped from the map), which are remembered for the world beyond
	// the screen.
	size_t edit_count() const { return m_edits.size(); }

private:
	struct Header
	{
		int width = 0, height = 0;           // blocks
		uint32_t layout[2] = {};             // plane 1, plane 2
		uint32_t blocks = 0;
	};

	uint16_t rom_word(uint32_t address) const;
	bool parse(uint32_t address, Header &header) const;
	uint16_t entry(const Header &header, int plane, int column, int row) const;

	const std::vector<uint8_t> &m_rom;
	uint32_t m_header = 0;
	int m_row_offset = -1;       // ring row of world tile row 0, once found for this level
	double m_agreement = 0;
	int m_last_world_x = -1;
	// Where the block on plane 1 (a boss) was last seen, as the screen x of its
	// middle, to keep it in the same lap of the 256-pixel ring from frame to
	// frame. The game clears the block while the boss is well off the screen and
	// draws it again at the same edge, so this outlasts the gaps.
	bool m_p1_known = false;
	// The title card: where plane 1 stood when it first showed, and whether it
	// has moved from there since.
	bool m_card_seen = false, m_card_moved = false;
	int m_card_s1x = 0;
	int m_p1_centre = 0;
	// Tiles the game has changed since it drew them, by plane, world column
	// and world row: what it showed the last time the tile was on screen.
	std::unordered_map<uint64_t, uint16_t> m_edits;
};

} // namespace ngpc
