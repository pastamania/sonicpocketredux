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
// routine) and of the sixth's (Boss5_Load to Boss6_Load, level 11); a task running
// it is a boss, or a part of one, with its x at +0x12. The sixth zone's body is a
// block of plane 1's tiles like theirs, scrolled to bring it in from the right of
// the screen, and its task's x is the block's middle all the way (checked from its
// coming in to its end).
constexpr uint32_t BOSS_CODE_FIRST = 0x3A0EDA;
constexpr uint32_t BOSS_CODE_END = 0x3A2000;
constexpr uint32_t BOSS5_CODE_FIRST = 0x3A3486;
constexpr uint32_t BOSS5_CODE_END = 0x3A3F4C;
// Now and then one of these tasks keeps a place on the screen, not in the level, in
// the words where its x is (the sixth's body does, for a few ticks, with 124 and 27
// for instance): less the camera's, that is thousands of pixels away, and is not
// taken as a place. Anything this far from the screen is left out.
constexpr int BOSS_X_LIMIT = 1024;
constexpr int BOSS_TASKS = 8;
constexpr int CARD_TASKS = 4;
// The player's task (x at +0x12), which in the fifth zone's flight (level 8) is
// the plane's: see plane1_block_level.
constexpr uint32_t PLAYER_TASK = 0x6708;
// The act clear (the "Sonic got through" screen, with the bonuses) is a task of
// this part of the cartridge (the one that draws its lines of text, from
// 0x3CE66F; the capsule's task before it runs from the level's end on, and is
// not the text) and runs while the screen is up.
constexpr uint32_t ACT_CLEAR_FIRST = 0x3CE60A;
constexpr uint32_t ACT_CLEAR_END = 0x3CE7F7;

// The endings and the credits are tasks of these parts of the cartridge: DoBadEnding
// and DoGoodEnding, which stay until the credits are done (0x3D6E6E to DoDelayedBGM,
// which the levels use as well and is not one), and the scenes they start and the
// credits (DoCredits, from 0x3E6AD1) up to the language error screen. They draw their
// pictures in the same rings as the level, for 160 pixels, with no level behind them.
constexpr uint32_t ENDING_FIRST = 0x3D6E6E;
constexpr uint32_t ENDING_END = 0x3D6F79;
constexpr uint32_t ENDING_SCENES_FIRST = 0x3E669A;
constexpr uint32_t ENDING_SCENES_END = 0x3E780E;
// The last fight's end: when Robotnik's machine is beaten his pod comes out and flies in
// front of the player, and a hit on it ends the level. On that hit the pod's task goes to
// this routine (the one that sends it off, with its pieces falling) and the game goes on,
// by the act clear and a short walk across the level and a fade, to the ending's first
// picture. All of it is staged for the screen's 160 pixels, so from the hit the picture is
// the screen centred between black bars (LevelRam::finale) until the next level starts.
constexpr uint32_t LAST_BOSS_POD_HIT = 0x3E540B;
constexpr int FINAL_LEVEL = 12;
// The last bonus level, Chaotic Space (13): Super Sonic against Robotnik's machine in orbit, reached by
// the level select once the game is cleared. It has no level behind it (its header pointer is 0 and the
// camera has no range): the two scroll planes are pictures of the planet and the stars, 256 pixels round,
// which the game scrolls with the camera. Its own code, the fight's controller and the machine's parts
// (b7_ and CS_ in the disassembly), is a run of the cartridge that its tasks keep to.
constexpr int CHAOTIC_SPACE_LEVEL = 13;
constexpr uint32_t CHAOTIC_SPACE_FIRST = 0x3D2656;
constexpr uint32_t CHAOTIC_SPACE_END = 0x3D3000;
// The machine's body is a block of plane 1's tiles that its main task moves by the plane's scroll; the task
// keeps the block's middle in its x (+0x12), a world x (the machine stays at 176 while the camera pans), and
// the screen's x is that less the camera's pan (0x67DC: 0 to 160; 0x506C is another counter in this level).
// The states of that task, from the machine's entrance (it comes in from the left, to 208, then 176), the fight
// (0x3D275B) and its end: when the machine is beaten it goes through these in turn.
constexpr uint32_t CHAOTIC_SPACE_PAN = 0x67DC;
constexpr uint32_t CHAOTIC_SPACE_MACHINE[] = { 0x3D26B5, 0x3D2707, 0x3D2722, 0x3D2736, 0x3D275B,
		0x3D2830, 0x3D285A, 0x3D287D, 0x3D28D0, 0x3D2907 };

// What the game holds in RAM that locates the level, read at the end of a frame.
struct LevelRam
{
	uint32_t header = 0;
	int camera_x = 0, camera_y = 0;
	int camera_x_start = 0, camera_x_end = 0;
	bool title_card = false;        // the zone's title card is showing
	bool act_clear = false;         // the act clear's text is on screen
	bool ending = false;            // the finale, an ending or the credits are on
	// The last fight's pod has been hit (see LAST_BOSS_POD_HIT): from then until the next
	// level starts. Unlike the rest it is not a state of one frame: read_level_ram is given
	// the last frame's value to carry it through the gaps between the tasks that show it.
	bool finale = false;
	// Chaotic Space's fight, or its title card, is on: the level number is 13 and a task of the
	// level's own code (or of the card) is running, so that the title screen and the menus the
	// game goes back to, which keep the number, are not taken for it.
	bool space = false;
	// The records of the card's tasks, which own the sprites of the card (as
	// opposed to the level's own: the player, the HUD).
	uint32_t card_owner[CARD_TASKS] = {};
	int card_owner_count = 0;
	int level = -1;                 // the level being played (0..12)
	// The x, on screen (the level's x less the camera's), of the tasks of the
	// bosses above.
	int boss_x[BOSS_TASKS] = {};
	int boss_count = 0;
	// The same for the player's task.
	int player_x = 0;
};

// Whether the camera is locked where a wide view takes the space over: the end
// of an act 1 (the camera stops at the level's end for the capsule) and the
// boss arenas of the first and second zones. There the original keeps the
// picture and the player to the 160 pixels of the screen; a wide view shows
// the level beyond instead and lets the player and the first boss use the
// whole width. Levels, by their numbers: 0, 2, 4, 6 and 10 are act 1s; 1, 3, 7, 9
// and 11 end in the boss of a zone and 12 is the last fight. The third zone's
// boss (5) is at the end of its level and its camera is not locked, nor is
// level 8's: they are left as the game has them. Chaotic Space (13), whose player is kept to
// the screen throughout, is one as well, though it has no level data to show beyond it.
bool wide_arena(const LevelRam &ram);

// Whether the level is one of those, locked or not.
bool arena_level(const LevelRam &ram);

// Whether the level's plane 1 holds a block that the game moves by scrolling
// the plane, outside an arena: the fifth zone's flight (level 8), where the
// plane Sonic stands on is a picture in plane 1's ring that slides in from the
// left at the start and across the screen to the right. Its ring is shown for
// one lap, as a boss's block is (WideScene::p1_window), or the plane would
// also show at the far side as it comes in; the lap is the one the player's
// task is in.
bool plane1_block_level(const LevelRam &ram);

class Machine;

// Reads those from the machine's RAM. `finale_before` is the last call's LevelRam::finale
// (false for the first call).
LevelRam read_level_ram(Machine &machine, bool finale_before = false);

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
	// The same, for what a wide view shows beyond the level's data (see
	// m_last_column): `extend` holds the level's last column on.
	uint16_t scene_entry(const Header &header, int plane, int column, int row, bool extend) const;

	// A scene for Chaotic Space: no tiles, the planes from the hardware's rings (WideScene::ring).
	bool build_space(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene);
	// A block of plane 1's tiles that the game moves by scrolling the plane (a boss's body): sets the
	// scene to draw one lap of the ring, the one the block is in. `follow_player` also takes the
	// player's task as an anchor for the lap (the fifth zone's flight).
	void p1_block_window(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene,
			bool follow_player);
	// The title card's part of a scene (WideScene::centred, card_hold).
	void card_state(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene);

	const std::vector<uint8_t> &m_rom;
	uint32_t m_header = 0;
	int m_row_offset = -1;       // ring row of world tile row 0, once found for this level
	// The last block column of the level (plane 2) that holds anything. An
	// arena's last wall can end before the level does, and what a wide view shows
	// beyond it would be the blank of an empty map: scene_entry repeats the last
	// tile column there, so the wall goes on.
	int m_last_column = -1;
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
	// The act clear's text slides in by rows, each by its own scroll, some from
	// the left and some from the right. For each row of plane 1's map: where its
	// scroll stood last frame (-1: not seen), which way it last moved (+1: to
	// the left of the screen, that is in from the right; -1: the other way; 0
	// not yet), and which lap of the ring its text was last drawn in.
	struct RowTrack
	{
		int s1x = -1;
		int direction = 0;
		int lap = 2;           // -1 or 0 (see build), 2: not decided
	};
	RowTrack m_rows[32];
	// Tiles the game has changed since it drew them, by plane, world column
	// and world row: what it showed the last time the tile was on screen.
	std::unordered_map<uint64_t, uint16_t> m_edits;
};

} // namespace ngpc
