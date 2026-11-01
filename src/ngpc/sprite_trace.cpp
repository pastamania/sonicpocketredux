#include "sprite_trace.h"

#include "ngpc/machine.h"

#include <algorithm>

namespace ngpc {

namespace {

constexpr uint32_t POOL = 0x4A06;
constexpr uint32_t POOL_END = 0x4B6E;
constexpr uint32_t ENTRY_SIZE = 12;
constexpr uint32_t SHADOW = 0x4B6E;
constexpr uint32_t SHADOW_END = 0x4C6E;

// AddSpriteObject stores the entry's layout pointer at this instruction; XIY
// is the entry, XIX holds x (low) and y (high), WA the priority, HL the layout.
constexpr uint32_t ADD_STORE_PC = 0x23C283;
// CopySprites; from COPY_HIDE_REST it blanks the sprites it did not use.
constexpr uint32_t COPY_FIRST = 0x23C03D;
constexpr uint32_t COPY_LAST = 0x23C1F7;
constexpr uint32_t COPY_HIDE_REST = 0x23C1C7;

bool in_copy(uint32_t pc) { return pc >= COPY_FIRST && pc <= COPY_LAST; }

} // namespace


SpriteTrace::SpriteTrace(Machine &machine) : m_machine(machine)
{
	for (SpriteTable &table : m_history)
		table.entry_of_slot.fill(-1);
	m_building.entry_of_slot.fill(-1);
	machine.watch_ram(POOL, SHADOW_END, [this](uint32_t address, uint8_t value, bool write) {
		on_access(address, value, write);
	});
}


SpriteTrace::~SpriteTrace()
{
	m_machine.watch_ram(0, 0, nullptr);
}


void SpriteTrace::on_access(uint32_t address, uint8_t, bool write)
{
	const ngpc_cpu &cpu = m_machine.cpu();
	const uint32_t pc = cpu.m_pc.d;

	if (write && pc == ADD_STORE_PC && address >= POOL && address < POOL_END && address == cpu.m_xiy.d + 8)
	{
		SpriteEntry &entry = m_pending[(cpu.m_xiy.d - POOL) / ENTRY_SIZE];
		uint32_t caller = 0;
		for (int i = 3; i >= 0; --i)
			caller = caller << 8 | m_machine.read_byte(cpu.m_xssp.d + i);
		entry.owner = cpu.m_xiz.d;
		entry.caller = caller;
		entry.x = static_cast<int16_t>(cpu.m_xix.w.l);
		entry.y = static_cast<int16_t>(cpu.m_xix.w.h);
		entry.layout = cpu.m_xhl[cpu.m_regbank].w.l;
		entry.priority = cpu.m_xwa[cpu.m_regbank].w.l;
		return;
	}

	if (!in_copy(pc))
	{
		if (m_expanding)
			finish_expansion();
		return;
	}
	if (!m_expanding)
		begin_expansion();

	if (!write)
	{
		if (address >= POOL && address < POOL_END)
		{
			const int index = static_cast<int>((address - POOL) / ENTRY_SIZE);
			if (index != m_current_index)
			{
				m_current_index = index;
				m_building.entries.push_back(m_pending[index]);
			}
		}
	}
	else if (address >= SHADOW && address < SHADOW_END)
	{
		const int slot = static_cast<int>((address - SHADOW) / 4);
		if (slot < SpriteTable::SPRITES)
			m_building.entry_of_slot[slot] = pc >= COPY_HIDE_REST || m_building.entries.empty() ? -1
					: static_cast<int8_t>(m_building.entries.size() - 1);
	}
}


void SpriteTrace::begin_expansion()
{
	m_expanding = true;
	m_current_index = -1;
	m_building.entries.clear();
	m_building.entry_of_slot.fill(-1);
}


void SpriteTrace::finish_expansion()
{
	m_expanding = false;
	m_current_index = -1;

	// Several entries from one call site (a loop) are told apart by order.
	for (size_t i = 0; i < m_building.entries.size(); ++i)
		for (size_t j = 0; j < i; ++j)
			if (m_building.entries[j].owner == m_building.entries[i].owner &&
					m_building.entries[j].caller == m_building.entries[i].caller)
				++m_building.entries[i].ordinal;

	const uint8_t *shadow = m_machine.work_ram() + (SHADOW - 0x4000);
	std::copy(shadow, shadow + m_building.shadow.size(), m_building.shadow.begin());
	m_building.serial = ++m_serial;
	m_newest = (m_newest + 1) % HISTORY;
	m_history[m_newest] = m_building;
}


void SpriteTrace::collect()
{
	// An expansion still running at the frame's end has not finished; one
	// whose code the machine has left has.
	if (m_expanding && !in_copy(m_machine.cpu().m_pc.d))
		finish_expansion();
}


const SpriteTable *SpriteTrace::match(const uint8_t *vram_table) const
{
	for (int age = 0; age < HISTORY && m_newest >= 0; ++age)
	{
		const SpriteTable &table = m_history[(m_newest - age + HISTORY) % HISTORY];
		if (table.serial != 0 && std::equal(table.shadow.begin(), table.shadow.end(), vram_table))
			return &table;
	}
	return nullptr;
}

} // namespace ngpc
