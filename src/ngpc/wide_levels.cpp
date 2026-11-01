#include "ngpc/wide_levels.h"

#include "ngpc/level_map.h"
#include "ngpc/machine.h"

#include <algorithm>
#include <vector>

namespace ngpc {

namespace {

constexpr uint32_t ROM_BASE = 0x200000;
constexpr int END_COLUMNS = 2;    // blocks of the level beyond the camera's reach

struct Changed
{
	uint32_t offset;       // in the cartridge image
	uint16_t before;
};

std::vector<Changed> s_changed;
std::vector<uint32_t> s_done;

uint16_t word(const std::vector<uint8_t> &cart, uint32_t address)
{
	return static_cast<uint16_t>(cart[address - ROM_BASE] | (cart[address - ROM_BASE + 1] << 8));
}

void extend(std::vector<uint8_t> &cart, uint32_t header)
{
	if (header < ROM_BASE || header - ROM_BASE + 0x20 > cart.size())
		return;
	const int width = word(cart, header), height = word(cart, header + 2);
	if (width <= 2 * END_COLUMNS || width > 1024 || height < 1 || height > 1024)
		return;
	const uint32_t layout = static_cast<uint32_t>(word(cart, header + 0x0A)) |
			(static_cast<uint32_t>(word(cart, header + 0x0C)) << 16);
	if (layout < ROM_BASE || layout - ROM_BASE + static_cast<uint32_t>(width) * height * 2 > cart.size())
		return;
	auto block_at = [&](int row, int column) { return layout + 2 * static_cast<uint32_t>(row * width + column); };
	// A column that holds one block all the way down has nothing made in it (the
	// fifth zone's last act ends in two columns of sky, block 1, past the floor):
	// it is unfinished like a blank one, and what comes before it goes on.
	bool unfinished[END_COLUMNS];
	for (int j = 0; j < END_COLUMNS; j++)
	{
		unfinished[j] = true;
		const uint16_t first = word(cart, block_at(0, width - END_COLUMNS + j));
		for (int row = 1; row < height && unfinished[j]; row++)
			unfinished[j] = word(cart, block_at(row, width - END_COLUMNS + j)) == first;
	}
	// Taken from the blocks as they were: the last column takes the one two to
	// its left, not a copy that has just been made.
	for (int row = 0; row < height; row++)
		for (int column = width - END_COLUMNS; column < width; column++)
		{
			const uint32_t at = block_at(row, column);
			const uint16_t source = word(cart, block_at(row, column - 2));
			if ((word(cart, at) != 0 && !unfinished[column - (width - END_COLUMNS)]) || source == 0 ||
					source == word(cart, at))
				continue;
			s_changed.push_back({ at - ROM_BASE, word(cart, at) });
			cart[at - ROM_BASE] = static_cast<uint8_t>(source & 0xff);
			cart[at - ROM_BASE + 1] = static_cast<uint8_t>(source >> 8);
		}
}

} // namespace

void extend_level_ends(Machine &machine, const LevelRam &level, bool wide)
{
	std::vector<uint8_t> &cart = machine.cartridge_data();
	if (!wide)
	{
		for (const Changed &change : s_changed)
		{
			cart[change.offset] = static_cast<uint8_t>(change.before & 0xff);
			cart[change.offset + 1] = static_cast<uint8_t>(change.before >> 8);
		}
		s_changed.clear();
		s_done.clear();
		return;
	}
	const uint32_t header = level.header;
	if (header == 0 || !arena_level(level) || std::find(s_done.begin(), s_done.end(), header) != s_done.end())
		return;
	s_done.push_back(header);
	extend(cart, header);
}

} // namespace ngpc
