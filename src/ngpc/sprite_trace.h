// Observes how the cartridge builds its sprite table, so that the host knows
// which object every hardware sprite belongs to.
//
// The game keeps a list of sprite objects ("entries", 12 bytes each, RAM
// 0x4A06..0x4B6D). Tasks add entries with AddSpriteObject (0x23C224); once
// per game tick CopySprites (0x23C03D) walks the list and expands each entry
// into one or more hardware sprites in a shadow table (RAM 0x4B6E), which is
// later copied to video RAM. Watching those accesses gives, for every sprite
// slot, the entry that produced it and, for every entry, the task that added
// it, so sprites need not be guessed at from the picture.
//
// Specific to this cartridge: the addresses below belong to its code. On any
// other program nothing fires and no table is ever produced.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ngpc {

class Machine;

// Whether an object was added from the HUD routines (ShowHUD_*: time, rings,
// lives), by the return address of its AddSpriteObject call. The HUD is one
// task, but where its record lives changes from one run to another (0x40B2 in
// one, 0x4196 in another), so it is recognised by its code: every object of a
// task that has added one from these routines is the HUD's. Draw2DigitNum is
// left out because anything may call it.
constexpr bool is_hud_caller(uint32_t caller)
{
	return (caller >= 0x39DA25 && caller < 0x39DB22) ||
			(caller >= 0x39DCF2 && caller < 0x39DD59) ||
			(caller >= 0x39DD79 && caller < 0x39DE14);
}

struct SpriteEntry
{
	uint32_t owner = 0;       // task record that added it (XIZ at the call)
	uint32_t caller = 0;      // return address of the AddSpriteObject call
	int x = 0, y = 0;         // origin of the object, in sprite coordinates
	uint16_t layout = 0;      // low word of the sprite layout data pointer
	uint16_t priority = 0;
	int ordinal = 0;          // which of the entries with this owner and caller
};

// One expansion of the list into hardware sprites.
struct SpriteTable
{
	static constexpr int SPRITES = 64;

	uint64_t serial = 0;                         // grows with every expansion
	std::array<int8_t, SPRITES> entry_of_slot{}; // index into entries, -1 if none
	std::vector<SpriteEntry> entries;            // in the order they were expanded
	std::array<uint8_t, SPRITES * 4> shadow{};   // the table as built: attr, x, y per sprite
};

class SpriteTrace
{
public:
	explicit SpriteTrace(Machine &machine);
	~SpriteTrace();
	SpriteTrace(const SpriteTrace &) = delete;
	SpriteTrace &operator=(const SpriteTrace &) = delete;

	// Call after each Machine::run_frame(): completes an expansion that has
	// finished.
	void collect();

	// The latest table whose sprite attributes are `vram_table` (the 256 bytes
	// the video chip holds at 0x8800), or null if none is: the game may be
	// writing sprites some other way, or have been caught part way.
	const SpriteTable *match(const uint8_t *vram_table) const;

private:
	static constexpr int HISTORY = 4;

	void on_access(uint32_t address, uint8_t value, bool write);
	void begin_expansion();
	void finish_expansion();

	Machine &m_machine;
	std::array<SpriteEntry, 30> m_pending;   // what AddSpriteObject stored in each entry
	bool m_expanding = false;
	int m_current_index = -1;                // entry CopySprites is reading
	SpriteTable m_building;
	std::array<SpriteTable, HISTORY> m_history;
	int m_newest = -1;
	uint64_t m_serial = 0;
};

} // namespace ngpc
