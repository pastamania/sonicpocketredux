// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
/*******************************************************************

Toshiba TLCS-900 emulation

Standalone adaptation of MAME's src/devices/cpu/tlcs900/tlcs900.cpp.
The instruction tables and handlers (900tbl.hxx, 900htbl.hxx) are
included unmodified from upstream.

*******************************************************************/

#include "tlcs900.h"
#include "instruction_hook.h"

#include <vector>

static constexpr u8 FLAG_CF = 0x01;
static constexpr u8 FLAG_NF = 0x02;
static constexpr u8 FLAG_VF = 0x04;
static constexpr u8 FLAG_HF = 0x10;
static constexpr u8 FLAG_ZF = 0x40;
static constexpr u8 FLAG_SF = 0x80;

tlcs900h_device::tlcs900h_device(tlcs900_bus &bus) :
	tlcs900_device(bus)
{
	m_mnemonic_80 = s_mnemonic_80;
	m_mnemonic_88 = s_mnemonic_88;
	m_mnemonic_90 = s_mnemonic_90;
	m_mnemonic_98 = s_mnemonic_98;
	m_mnemonic_a0 = s_mnemonic_a0;
	m_mnemonic_b0 = s_mnemonic_b0;
	m_mnemonic_b8 = s_mnemonic_b8;
	m_mnemonic_c0 = s_mnemonic_c0;
	m_mnemonic_c8 = s_mnemonic_c8;
	m_mnemonic_d0 = s_mnemonic_d0;
	m_mnemonic_d8 = s_mnemonic_d8;
	m_mnemonic_e0 = s_mnemonic_e0;
	m_mnemonic_e8 = s_mnemonic_e8;
	m_mnemonic_f0 = s_mnemonic_f0;
	m_mnemonic = s_mnemonic;
}


inline uint8_t tlcs900_device::RDOP()
{
	uint8_t data;

	if ( m_prefetch_clear )
	{
		for ( int i = 0; i < 4; i++ )
		{
			m_prefetch[ i ] = RDMEM( m_pc.d + i );
		}
		m_prefetch_index = 0;
		m_prefetch_clear = false;
	}
	else
	{
		m_prefetch[ m_prefetch_index ] = RDMEM( m_pc.d + 3 );
		m_prefetch_index = ( m_prefetch_index + 1 ) & 0x03;
	}
	data = m_prefetch[ m_prefetch_index ];

	m_pc.d++;
	return data;
}


void tlcs900_device::device_start()
{
	m_pc.d = 0;
	memset(m_xwa, 0x00, sizeof(m_xwa));
	memset(m_xbc, 0x00, sizeof(m_xbc));
	memset(m_xde, 0x00, sizeof(m_xde));
	memset(m_xhl, 0x00, sizeof(m_xhl));
	m_xix.d = 0;
	m_xiy.d = 0;
	m_xiz.d = 0;
	m_xnsp.d = 0;
	m_xssp.d = 0;
	m_sr.d = 0;
	m_f2.d = 0;
	memset(m_dmas, 0x00, sizeof(m_dmas));
	memset(m_dmad, 0x00, sizeof(m_dmad));
	memset(m_dmac, 0x00, sizeof(m_dmac));
	memset(m_dmam, 0x00, sizeof(m_dmam));
	m_intnest = 0;
	memset(m_level, 0x00, sizeof(m_level));
	m_nmi_state = CLEAR_LINE;
	m_icount = 0;
}


void tlcs900_device::device_reset()
{
	m_pc.d = 0x00008000;
	/* system mode, iff set to 111, min mode, register bank 0 */
	m_sr.d = 0xf000;
	m_regbank = 0;
	m_xssp.d = 0x0100;
	m_intnest = 0;
	m_halted = 0;
	m_check_irqs = 0;
	m_prefetch_clear = true;
	m_irq_inhibit = false;
}


void tlcs900h_device::device_reset()
{
	m_pc.b.l = RDMEM( 0xffff00 );
	m_pc.b.h = RDMEM( 0xffff01 );
	m_pc.b.h2 = RDMEM( 0xffff02 );
	m_pc.b.h3 = 0;
	/* system mode, iff set to 111, max mode, register bank 0 */
	m_sr.d = 0xf800;
	m_regbank = 0;
	m_xssp.d = 0x0100;
	m_intnest = 0;
	m_halted = 0;
	m_check_irqs = 0;
	m_prefetch_clear = true;
	m_irq_inhibit = false;
}


#include "900tbl.hxx"
#include "900htbl.hxx"


void tlcs900_device::step_irq_phase()
{
	if ( m_check_irqs )
	{
		if ( m_irq_inhibit )
		{
			/* Interrupt shadow after EI/RETI: acceptance is deferred until
			   after the instruction following EI or RETI. */
			m_irq_inhibit = false;
		}
		else
		{
			tlcs900_check_irqs();
			m_check_irqs = 0;
		}
	}
	else
	{
		m_irq_inhibit = false;
	}
}


namespace tlcs900_hook {

namespace {

constexpr uint32_t BASE = 0x200000;
constexpr uint32_t SIZE = 0x200000;

struct Entry
{
	uint32_t pc;
	Function function;
	void *context;
};

bool s_active = false;
std::vector<uint8_t> s_bits;       // one bit per address
std::vector<Entry> s_entries;

} // namespace

void add(uint32_t pc, Function function, void *context)
{
	if (pc < BASE || pc - BASE >= SIZE)
		return;
	if (s_bits.empty())
		s_bits.assign(SIZE / 8, 0);
	s_bits[(pc - BASE) >> 3] |= static_cast<uint8_t>(1 << (pc & 7));
	s_entries.push_back({ pc, function, context });
	s_active = true;
}

void clear()
{
	s_active = false;
	s_bits.clear();
	s_entries.clear();
}

} // namespace tlcs900_hook

// Runs the hooks for the instruction about to execute at `pc`.
static void run_hooks_at(tlcs900_device &cpu, offs_t pc)
{
	using namespace tlcs900_hook;
	const uint32_t offset = pc - BASE;
	if (offset >= SIZE || !((s_bits[offset >> 3] >> (pc & 7)) & 1))
		return;
	for (const Entry &entry : s_entries)
		if (entry.pc == pc)
			entry.function(cpu, entry.context);
}


void tlcs900_device::step_execute()
{
	const tlcs900inst *inst;

	if ( m_halted )
	{
		debugger_wait_hook();
		m_cycles += 8;
	}
	else if ( trap_hook( m_pc.d ) )
	{
		/* High-level emulated code (BIOS) ran in place of instructions at PC. */
		m_prefetch_clear = true;
		m_cycles += 8;
	}
	else
	{
		debugger_instruction_hook( m_pc.d );
		if ( tlcs900_hook::s_active )
			run_hooks_at( *this, m_pc.d );

		m_op = RDOP();
		inst = &m_mnemonic[m_op];
		prepare_operands( inst );

		/* Execute the instruction */
		(this->*inst->opfunc)();
		m_cycles += inst->cycles;
	}
}


void tlcs900_device::step_finish()
{
	tlcs900_handle_ad();

	tlcs900_handle_timers();

	tlcs900_check_hdma();

	step_finished( m_cycles );
}


int tlcs900_device::step()
{
	m_cycles = 0;
	step_irq_phase();
	step_execute();
	step_finish();
	return m_cycles;
}


bool tlcs900_device::recompiled_step_begin( offs_t pc )
{
	m_cycles = 0;
	m_pc.d = pc;
	step_irq_phase();
	if ( m_pc.d == pc && !m_halted )
	{
		if ( tlcs900_hook::s_active )
			run_hooks_at( *this, pc );
		return false;
	}

	/* An interrupt was accepted: the rest of this step executes at the
	   vector, exactly like step() would. */
	m_prefetch_clear = true;
	step_execute();
	step_finish();
	return true;
}


void tlcs900_device::recompiled_step_end( int cycles )
{
	m_cycles += cycles;
	step_finish();
}


uint8_t *tlcs900_device::control_reg8(uint8_t code)
{
	switch ( code )
	{
	case 0x22: case 0x42: return &m_dmam[0].b.l;
	case 0x26: case 0x46: return &m_dmam[1].b.l;
	case 0x2a: case 0x4a: return &m_dmam[2].b.l;
	case 0x2e: case 0x4e: return &m_dmam[3].b.l;
	default: return &m_dummy.b.l;
	}
}


uint16_t *tlcs900_device::control_reg16(uint8_t code)
{
	switch ( code )
	{
	case 0x20: case 0x40: return &m_dmac[0].w.l;
	case 0x24: case 0x44: return &m_dmac[1].w.l;
	case 0x28: case 0x48: return &m_dmac[2].w.l;
	case 0x2c: case 0x4c: return &m_dmac[3].w.l;
	case 0x3c: case 0x7c: return &m_intnest;
	default: return &m_dummy.w.l;
	}
}


uint32_t *tlcs900_device::control_reg32(uint8_t code)
{
	switch ( code )
	{
	case 0x00: return &m_dmas[0].d;
	case 0x04: return &m_dmas[1].d;
	case 0x08: return &m_dmas[2].d;
	case 0x0c: return &m_dmas[3].d;
	case 0x10: case 0x20: return &m_dmad[0].d;
	case 0x14: case 0x24: return &m_dmad[1].d;
	case 0x18: case 0x28: return &m_dmad[2].d;
	case 0x1c: case 0x2c: return &m_dmad[3].d;
	default: return &m_dummy.d;
	}
}


int tlcs900_device::execute(int cycles)
{
	m_icount = cycles;
	do
	{
		m_icount -= step();
	} while ( m_icount > 0 );
	return cycles - m_icount;
}
