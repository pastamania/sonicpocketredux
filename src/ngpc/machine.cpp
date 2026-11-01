#include "ngpc/machine.h"

#include <array>
#include <stdexcept>
#include <cstdio>
#include <cstring>

namespace ngpc {

namespace {

constexpr uint8_t to_bcd(int value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }

// System call entry points. The addresses match the Mednafen/BizHawk HLE BIOS
// so any pointer the game reads out of the 0xFFFE00 table has the same value.
constexpr std::array<uint32_t, 0x1b> kSystemCallAddresses = {
	0xFF27A2, // 0x00 VECT_SHUTDOWN
	0xFF1030, // 0x01 VECT_CLOCKGEARSET
	0xFF1440, // 0x02 VECT_RTCGET
	0xFF12B4, // 0x03
	0xFF1222, // 0x04 VECT_INTLVSET
	0xFF8D8A, // 0x05 VECT_SYSFONTSET
	0xFF6FD8, // 0x06 VECT_FLASHWRITE
	0xFF7042, // 0x07 VECT_FLASHALLERS
	0xFF7082, // 0x08 VECT_FLASHERS
	0xFF149B, // 0x09 VECT_ALARMSET
	0xFF1033, // 0x0a
	0xFF1487, // 0x0b VECT_ALARMDOWNSET
	0xFF731F, // 0x0c
	0xFF70CA, // 0x0d VECT_FLASHPROTECT
	0xFF17C4, // 0x0e VECT_GEMODESET
	0xFF1032, // 0x0f
	0xFF2BBD, // 0x10 VECT_COMINIT
	0xFF2C0C, // 0x11 VECT_COMSENDSTART
	0xFF2C44, // 0x12 VECT_COMRECEIVESTART
	0xFF2C86, // 0x13 VECT_COMCREATEDATA
	0xFF2CB4, // 0x14 VECT_COMGETDATA
	0xFF2D27, // 0x15 VECT_COMONRTS
	0xFF2D33, // 0x16 VECT_COMOFFRTS
	0xFF2D3A, // 0x17 VECT_COMSENDSTATUS
	0xFF2D4E, // 0x18 VECT_COMRECEIVESTATUS
	0xFF2D6C, // 0x19 VECT_COMCREATEBUFDATA
	0xFF2D85, // 0x1a VECT_COMGETBUFDATA
};

// Default handler installed in every user interrupt vector (a lone RETI).
constexpr uint32_t kDefaultInterruptHandler = 0xFF23DF;

// CPU vectors at 0xFFFF00 point into this trap window: 0xFF3000 + vector.
constexpr uint32_t kVectorTrapBase = 0xFF3000;

// Slot in the user vector table at 0x6FB8 for each CPU vector offset.
int user_vector_slot(uint32_t vector)
{
	switch (vector)
	{
	case 0x0c: return 0;   // SWI 3
	case 0x10: return 1;   // SWI 4
	case 0x14: return 2;   // SWI 5
	case 0x18: return 3;   // SWI 6
	case 0x28: return 4;   // INT0: RTC alarm
	case 0x2c: return 5;   // INT4: VBlank
	case 0x30: return 6;   // INT5: Z80
	case 0x40: return 7;   // INTT0
	case 0x44: return 8;   // INTT1
	case 0x48: return 9;   // INTT2
	case 0x4c: return 10;  // INTT3
	case 0x64: return 11;  // INTTX0: serial transmission
	case 0x60: return 12;  // INTRX0: serial reception
	case 0x74: return 14;  // INTTC0: micro DMA 0 end
	case 0x78: return 15;
	case 0x7c: return 16;
	case 0x80: return 17;
	default: return -1;
	}
}

} // namespace


Machine::Machine()
{
	m_cpu = std::make_unique<ngpc_cpu>(*this);
	m_cpu->device_start();
	m_cpu->on_trap = [this](offs_t pc) { return bios_trap(pc); };
	m_cpu->on_timer3 = [this]() {
		if (m_z80_running)
			z80_gen_int(&m_z80, 0xff);
	};

	m_cpu->on_step_finished = [this](int cycles) { advance(cycles); };
	m_video.current_cycle_in_line = [this]() {
		return static_cast<int>(m_cpu_cycle - m_line_start_cycle);
	};
	m_video.vblank_pin_w = [this](int state) {
		m_cpu->set_input_line(TLCS900_INT4, state ? ASSERT_LINE : CLEAR_LINE);
	};
	m_video.hblank_pin_w = [this](int state) {
		m_cpu->set_input_line(TLCS900_TIO, state ? ASSERT_LINE : CLEAR_LINE);
	};

	z80_init(&m_z80);
	m_z80.userdata = this;
	m_z80.read_byte = &Machine::z80_read;
	m_z80.write_byte = &Machine::z80_write;
	m_z80.port_in = &Machine::z80_in;
	m_z80.port_out = &Machine::z80_out;

	// Synthetic BIOS: system call table, CPU vectors and a default handler.
	m_bios.assign(0x10000, 0x00);
	for (size_t i = 0; i < kSystemCallAddresses.size(); ++i)
	{
		const uint32_t address = kSystemCallAddresses[i];
		std::memcpy(&m_bios[0xfe00 + i * 4], &address, 4);
	}
	for (uint32_t vector = 0; vector < 0x100; vector += 4)
	{
		const uint32_t address = kVectorTrapBase + vector;
		std::memcpy(&m_bios[0xff00 + vector], &address, 4);
	}
	m_bios[kDefaultInterruptHandler & 0xffff] = 0x07; // RETI
}


Machine::~Machine() = default;


std::string Machine::load_cartridge(std::vector<uint8_t> image)
{
	if (image.size() != 0x200000)
		return "expected a 2 MiB cartridge image";
	m_cart = std::move(image);
	for (int i = 0; i < 4; ++i)
	{
		m_flash_id_backup[i] = m_cart[i];
		m_flash_id_backup[4 + i] = m_cart[0x7c000 + i];
		m_flash_id_backup[8 + i] = m_cart[0xfc000 + i];
		m_flash_id_backup[12 + i] = m_cart[0x1fc000 + i];
	}
	return {};
}


void Machine::boot(int language, const RtcTime &rtc)
{
	std::memset(m_ram, 0, sizeof(m_ram));
	std::memset(m_sound_ram, 0, sizeof(m_sound_ram));
	std::memset(m_io, 0, sizeof(m_io));
	m_flash_state = F_READ;
	m_flash_command = 0;
	m_cpu_cycle = 0;
	m_z80_cycle_target = 0;
	m_frame = 0;
	m_z80_running = false;
	m_old_to3 = 0;
	m_psg = t6w28();
	m_dac_left = m_dac_right = 0x80;
	m_audio_cycles = m_audio_ticks = m_audio_left = m_audio_right = 0;
	m_audio.clear();

	m_line = 0;
	m_line_start_cycle = 0;
	m_hblank_pending = false;
	m_frame_done = false;

	m_video.reset();
	m_cpu->device_reset();

	// Internal CPU registers as left by the BIOS (Mednafen's snapshot).
	static const uint8_t internal_defaults[0x80] = {
		0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0xFF, 0xFF,
		0x34, 0x3C, 0xFF, 0xFF, 0xFF, 0x3F, 0x00, 0x00, 0x3F, 0xFF, 0x2D, 0x01, 0xFF, 0xFF, 0x03, 0xB2,
		0x80, 0x00, 0x01, 0x90, 0x03, 0xB0, 0x90, 0x62, 0x05, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x4C, 0x4C,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x20, 0xFF, 0x80, 0x7F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x20, 0x69, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x17, 0x03, 0x03, 0x02, 0x00, 0x00, 0x4E,
		0x02, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	std::memcpy(m_cpu->m_raw, internal_defaults, sizeof(internal_defaults));
	m_cpu->m_trun = internal_defaults[0x20];
	m_cpu->m_t8_reg[0] = internal_defaults[0x22];
	m_cpu->m_t8_reg[1] = internal_defaults[0x23];
	m_cpu->m_t8_mode[0] = internal_defaults[0x24];
	m_cpu->m_t8_invert = internal_defaults[0x25] | 0xcc;
	m_cpu->m_t8_reg[2] = internal_defaults[0x26];
	m_cpu->m_t8_reg[3] = internal_defaults[0x27];
	m_cpu->m_t8_mode[1] = internal_defaults[0x28];
	m_cpu->m_trdc = internal_defaults[0x29];
	m_cpu->m_watchdog_mode = internal_defaults[0x6e];
	for (int i = 0; i < 0x0b; ++i)
		m_cpu->m_int_reg[i] = internal_defaults[0x70 + i];

	// I/O area 0x80-0xBF.
	m_io[0x34] = 0x0a;
	m_io[0x38] = 0xaa; // sound chip disabled
	m_io[0x39] = 0xaa; // Z80 halted
	m_io[0x11] = to_bcd(rtc.year % 100);
	m_io[0x12] = to_bcd(rtc.month);
	m_io[0x13] = to_bcd(rtc.day);
	m_io[0x14] = to_bcd(rtc.hour);
	m_io[0x15] = to_bcd(rtc.minute);
	m_io[0x16] = to_bcd(rtc.second);
	m_io[0x17] = static_cast<uint8_t>(((rtc.year % 4) << 4) | (rtc.day_of_week & 7));

	// BIOS work area 0x6C00-0x6FFF.
	auto ram8 = [this](uint32_t address, uint8_t value) { m_ram[address - 0x4000] = value; };
	auto ram16 = [&](uint32_t address, uint16_t value) { ram8(address, value & 0xff); ram8(address + 1, value >> 8); };
	auto ram32 = [&](uint32_t address, uint32_t value) { ram16(address, value & 0xffff); ram16(address + 2, value >> 16); };

	uint32_t entry;
	std::memcpy(&entry, &m_cart[0x1c], 4);
	const uint16_t catalog = m_cart[0x20] | (m_cart[0x21] << 8);
	const uint8_t sub_catalog = m_cart[0x22];
	const uint8_t mode = m_cart[0x23];
	ram32(0x6C00, entry);
	ram16(0x6C04, catalog);
	ram16(0x6E82, catalog);
	ram8(0x6C06, sub_catalog);
	ram8(0x6E84, sub_catalog);
	for (int i = 0; i < 12; ++i)
		ram8(0x6C08 + i, m_cart[0x24 + i]);
	// Flash chip size codes for chips 0 and 1: 1 = 4 Mbit, 2 = 8 Mbit,
	// 3 = 16 Mbit. The game's own erase routine indexes its block tables with
	// this. Mednafen always stores 1, which makes the game erase the wrong
	// block on a 16 Mbit cart; its flash model ignores that erase, ours
	// doesn't, so report the real chip like the hardware BIOS does.
	ram8(0x6C58, m_cart.size() > 0x100000 ? 0x03 : m_cart.size() > 0x80000 ? 0x02 : 0x01);
	ram8(0x6C59, 0x00);  // no second chip
	ram8(0x6C55, 0x01);  // commercial game
	ram8(0x6F80, 0xFF);  // battery level
	ram8(0x6F81, 0x03);
	ram8(0x6F84, 0x40);  // power-on start
	ram8(0x6F85, 0x00);  // no shutdown request
	ram8(0x6F86, 0x00);
	ram8(0x6F87, static_cast<uint8_t>(language)); // 0 = Japanese, 1 = English
	ram8(0x6F91, mode);
	ram8(0x6F95, mode);
	for (int i = 0; i < 0x12; ++i)
		ram32(0x6FB8 + i * 4, kDefaultInterruptHandler);

	// Video defaults.
	m_video.write(0x000, 0xC0);   // both video interrupts enabled
	m_video.write(0x002, 0x00);
	m_video.write(0x003, 0x00);
	m_video.write(0x004, 0xFF);
	m_video.write(0x005, 0xFF);
	m_video.write(0x006, 0xC6);
	m_video.write(0x012, 0x00);
	m_video.write(0x118, 0x80);
	m_video.write(0x3E0, 0xFF);
	m_video.write(0x3E1, 0x0F);
	m_video.write(0x3F0, 0xFF);
	m_video.write(0x3F1, 0x0F);
	m_video.write(0x400, 0xFF);
	m_video.write(0x402, 0x80);

	// CPU state when the BIOS jumps to the cartridge.
	m_cpu->m_pc.d = entry & 0xffffff;
	m_cpu->m_sr.w.l = 0xf800;
	m_cpu->m_regbank = 0;
	m_cpu->m_xssp.d = 0x6C00;
	m_cpu->m_prefetch_clear = true;

	m_video.begin_line(0);
	m_hblank_pending = true;
}


uint8_t Machine::read_byte(offs_t addr)
{
	addr &= 0xffffff;
	if (addr < 0x80)
		return m_cpu->internal_r(addr);
	if (addr < 0xc0)
		return io_read(addr - 0x80);
	if (addr >= 0x4000 && addr < 0x7000)
	{
		if (addr >= m_watch_first && addr < m_watch_last)
			m_watch(addr, m_ram[addr - 0x4000], false);
		return m_ram[addr - 0x4000];
	}
	if (addr >= 0x7000 && addr < 0x8000)
		return m_sound_ram[addr - 0x7000];
	if (addr >= 0x8000 && addr < 0xc000)
		return m_video.read(addr - 0x8000);
	if (addr >= CART_BASE && addr < CART_BASE + 0x200000)
		return m_cart[addr - CART_BASE];
	if (addr >= BIOS_BASE)
		return m_bios[addr - BIOS_BASE];
	return 0xff;
}


void Machine::write_byte(offs_t addr, uint8_t data)
{
	addr &= 0xffffff;
	if (addr < 0x80)
		m_cpu->internal_w(addr, data);
	else if (addr < 0xc0)
		io_write(addr - 0x80, data);
	else if (addr >= 0x4000 && addr < 0x7000)
	{
		m_ram[addr - 0x4000] = data;
		if (addr >= m_watch_first && addr < m_watch_last)
			m_watch(addr, data, true);
	}
	else if (addr >= 0x7000 && addr < 0x8000)
		m_sound_ram[addr - 0x7000] = data;
	else if (addr >= 0x8000 && addr < 0xc000)
		m_video.write(addr - 0x8000, data);
	else if (addr >= CART_BASE && addr < CART_BASE + 0x200000)
		flash_write(addr - CART_BASE, data);
}


uint8_t Machine::io_read(offs_t offset)
{
	switch (offset)
	{
	case 0x30:
		return m_buttons;
	case 0x31:
		return 0x01 | 0x02; // power on, sub-battery OK
	default:
		return m_io[offset];
	}
}


void Machine::io_write(offs_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0x20:  // T6W28 "right" (tone)
	case 0x21:  // T6W28 "left" (noise)
		if (m_io[0x38] == 0x55 && m_io[0x39] == 0xAA)
			m_psg.write(offset - 0x20, data);
		break;
	case 0x22:  // DAC right
		m_dac_right = data;
		break;
	case 0x23:  // DAC left
		m_dac_left = data;
		break;
	case 0x38:  // sound chip enable
		if (data == 0x55)
			m_psg.set_enable(true);
		else if (data == 0xAA)
			m_psg.set_enable(false);
		break;
	case 0x39:
		if (data == 0x55)
		{
			z80_init(&m_z80);
			m_z80.userdata = this;
			m_z80.read_byte = &Machine::z80_read;
			m_z80.write_byte = &Machine::z80_write;
			m_z80.port_in = &Machine::z80_in;
			m_z80.port_out = &Machine::z80_out;
			m_z80_running = true;
			m_z80_cycle_target = m_cpu_cycle;
		}
		else if (data == 0xAA)
		{
			m_z80_running = false;
		}
		break;
	case 0x3a:
		if (m_z80_running)
			z80_gen_nmi(&m_z80);
		break;
	}
	m_io[offset] = data;
}


void Machine::restore_flash_id_bytes()
{
	for (int i = 0; i < 4; ++i)
	{
		m_cart[i] = m_flash_id_backup[i];
		m_cart[0x7c000 + i] = m_flash_id_backup[4 + i];
		m_cart[0xfc000 + i] = m_flash_id_backup[8 + i];
		m_cart[0x1fc000 + i] = m_flash_id_backup[12 + i];
	}
}


// Toshiba 16 Mbit flash command state machine (MAME ngp.cpp, device 0x2f).
void Machine::flash_write(offs_t offset, uint8_t data)
{
	constexpr uint8_t manufacturer_id = 0x98;
	constexpr uint8_t device_id = 0x2f;

	switch (m_flash_state)
	{
	case F_READ:
		if (offset == 0x5555 && data == 0xaa)
			m_flash_state = F_PROG1;
		m_flash_command = 0;
		break;
	case F_PROG1:
		if (offset == 0x2aaa && data == 0x55)
			m_flash_state = F_PROG2;
		else
			m_flash_state = F_READ;
		break;
	case F_PROG2:
		if (data == 0x30)
		{
			if (m_flash_command == 0x80)
			{
				uint32_t start;
				uint32_t size = 0x10000;
				if (offset < 0x1f0000)
					start = offset & 0x1f0000;
				else if (offset & 0x8000)
				{
					if (offset & 0x4000)
					{
						start = offset & 0x1fc000;
						size = 0x4000;
					}
					else
					{
						start = offset & 0x1fe000;
						size = 0x2000;
					}
				}
				else
				{
					start = offset & 0x1f8000;
					size = 0x8000;
				}
				m_flash_state = F_AUTO_BLOCK_ERASE;
				std::memset(&m_cart[start], 0xff, size);
			}
			else
				m_flash_state = F_READ;
		}
		else if (offset == 0x5555)
		{
			switch (data)
			{
			case 0x80:
				m_flash_command = 0x80;
				m_flash_state = F_COMMAND;
				break;
			case 0x90:
				for (uint32_t base : { 0x1fc000u, 0xfc000u, 0x7c000u, 0u })
				{
					m_cart[base + 0] = manufacturer_id;
					m_cart[base + 1] = device_id;
					m_cart[base + 2] = 0x02;
					m_cart[base + 3] = 0x80;
				}
				m_flash_state = F_ID_READ;
				break;
			case 0x9a:
				if (m_flash_command == 0x9a)
					m_flash_state = F_BLOCK_PROTECT;
				else
				{
					m_flash_command = 0x9a;
					m_flash_state = F_COMMAND;
				}
				break;
			case 0xa0:
				m_flash_state = F_AUTO_PROGRAM;
				break;
			default:
				m_flash_state = F_READ;
				break;
			}
		}
		else
			m_flash_state = F_READ;
		break;
	case F_COMMAND:
		m_flash_state = (offset == 0x5555 && data == 0xaa) ? F_PROG1 : F_READ;
		break;
	case F_ID_READ:
		m_flash_state = (offset == 0x5555 && data == 0xaa) ? F_PROG1 : F_READ;
		m_flash_command = 0;
		break;
	case F_AUTO_PROGRAM:
		/* Only 1 -> 0 changes can be programmed */
		m_cart[offset] &= data;
		m_flash_state = F_READ;
		break;
	case F_AUTO_CHIP_ERASE:
	case F_AUTO_BLOCK_ERASE:
	case F_BLOCK_PROTECT:
		m_flash_state = F_READ;
		break;
	}

	if (m_flash_state == F_READ)
	{
		restore_flash_id_bytes();
		m_flash_command = 0;
	}
}


void Machine::bios_return_from_call()
{
	m_cpu->m_pc.d = m_cpu->RDMEML(m_cpu->m_xssp.d);
	m_cpu->m_xssp.d += 4;
}


void Machine::bios_return_from_interrupt()
{
	m_cpu->m_sr.w.l = m_cpu->RDMEMW(m_cpu->m_xssp.d);
	m_cpu->m_xssp.d += 2;
	m_cpu->m_pc.d = m_cpu->RDMEML(m_cpu->m_xssp.d);
	m_cpu->m_xssp.d += 4;
	m_cpu->m_regbank = m_cpu->m_sr.b.h & 0x03;
	m_cpu->m_check_irqs = 1;
}


void Machine::bios_system_call(int function)
{
	ngpc_cpu &cpu = *m_cpu;
	uint8_t &ra3 = cpu.m_xwa[3].b.l;
	uint8_t &rb3 = cpu.m_xbc[3].b.h;
	uint8_t &rc3 = cpu.m_xbc[3].b.l;
	uint32_t &xhl3 = cpu.m_xhl[3].d;
	uint32_t &xde3 = cpu.m_xde[3].d;

	switch (function)
	{
	case 0x00: // VECT_SHUTDOWN
		std::fprintf(stderr, "ngpc: game requested shutdown\n");
		break;
	case 0x01: // VECT_CLOCKGEARSET
		break;
	case 0x02: // VECT_RTCGET
		if (xhl3 < 0xc000)
			for (int i = 0; i < 7; ++i)
				write_byte(xhl3 + i, read_byte(0x91 + i));
		break;
	case 0x04: // VECT_INTLVSET
	{
		static const struct { uint8_t reg; uint8_t shift; } targets[10] = {
			{ 0x0, 0 }, { 0x1, 4 }, { 0x3, 0 }, { 0x3, 4 }, { 0x4, 0 },
			{ 0x4, 4 }, { 0x9, 0 }, { 0x9, 4 }, { 0xa, 0 }, { 0xa, 4 },
		};
		if (rc3 < 10)
		{
			const auto t = targets[rc3];
			uint8_t value = cpu.m_int_reg[t.reg];
			value = static_cast<uint8_t>((value & ~(0x07 << t.shift)) | ((rb3 & 0x07) << t.shift));
			cpu.internal_w(0x70 + t.reg, value);
		}
		break;
	}
	case 0x06: // VECT_FLASHWRITE
	{
		const uint32_t bank = ra3 == 1 ? 0x800000 : CART_BASE;
		const uint32_t bytes = cpu.m_xbc[3].w.l * 256u;
		for (uint32_t i = 0; i < bytes; ++i)
		{
			const uint32_t target = bank + xde3 + i;
			if (target >= CART_BASE && target < CART_BASE + 0x200000)
				m_cart[target - CART_BASE] = read_byte(xhl3 + i);
		}
		ra3 = 0;
		break;
	}
	case 0x08: // VECT_FLASHERS
		if (ra3 == 0 && rb3 == 31)
			std::memset(&m_cart[0x1f0000], 0xff, 0x8000);
		ra3 = 0;
		break;
	case 0x07: // VECT_FLASHALLERS
	case 0x09: // VECT_ALARMSET
	case 0x0b: // VECT_ALARMDOWNSET
	case 0x0d: // VECT_FLASHPROTECT
	case 0x10: // VECT_COMINIT
		ra3 = 0;
		break;
	case 0x14: // VECT_COMGETDATA: no link cable, buffer empty
		ra3 = 1;
		break;
	case 0x15: // VECT_COMONRTS
		write_byte(0xb2, 0);
		break;
	case 0x16: // VECT_COMOFFRTS
		write_byte(0xb2, 1);
		break;
	case 0x17: // VECT_COMSENDSTATUS
	case 0x18: // VECT_COMRECEIVESTATUS
		cpu.m_xwa[3].w.l = 0;
		break;
	default:
		break;
	}
}


bool Machine::bios_trap(offs_t pc)
{
	if ((pc & 0xff0000) != BIOS_BASE || pc == kDefaultInterruptHandler)
		return false;

	for (size_t i = 0; i < kSystemCallAddresses.size(); ++i)
	{
		if (kSystemCallAddresses[i] == pc)
		{
			bios_system_call(static_cast<int>(i));
			bios_return_from_call();
			return true;
		}
	}

	if (pc >= kVectorTrapBase && pc < kVectorTrapBase + 0x100)
	{
		const uint32_t vector = pc - kVectorTrapBase;
		if (vector == 0x04)
		{
			// SWI 1: system call selected by RW3, then return like RETI.
			bios_system_call(m_cpu->m_xwa[3].b.h);
			bios_return_from_interrupt();
			return true;
		}
		const int slot = user_vector_slot(vector);
		if (slot < 0)
		{
			bios_return_from_interrupt();
			return true;
		}
		if (vector == 0x2c)
			m_ram[0x6F82 - 0x4000] = m_buttons;
		m_cpu->m_pc.d = m_cpu->RDMEML(0x6FB8 + slot * 4) & 0xffffff;
		return true;
	}

	std::fprintf(stderr, "ngpc: execution reached unimplemented BIOS address %06X\n", pc);
	m_trace_remaining = 0;
	throw std::runtime_error("unimplemented BIOS address");
}


void Machine::run_z80_until(int64_t target_cpu_cycle)
{
	if (!m_z80_running)
	{
		m_z80_cycle_target = target_cpu_cycle;
		return;
	}
	// The Z80 runs at half the main CPU clock.
	const int64_t budget = (target_cpu_cycle - m_z80_cycle_target) / 2;
	const unsigned long start = m_z80.cyc;
	while (static_cast<int64_t>(m_z80.cyc - start) < budget)
		z80_step(&m_z80);
	m_z80_cycle_target += static_cast<int64_t>(m_z80.cyc - start) * 2;
}


// The T6W28 runs at 3.072 MHz / 16 = 192 kHz, one tick every 32 CPU cycles;
// four ticks are averaged into each 48 kHz output sample. Mixing levels follow
// MAME's NGP driver (PSG 0.5, DACs 0.25).
void Machine::advance_audio(int cycles)
{
	m_audio_cycles += cycles;
	while (m_audio_cycles >= 32)
	{
		m_audio_cycles -= 32;
		int left, right;
		m_psg.generate(left, right);
		m_audio_left += left;
		m_audio_right += right;
		if (++m_audio_ticks == 4)
		{
			const int dac_left = (m_dac_left - 0x80) * 64;
			const int dac_right = (m_dac_right - 0x80) * 64;
			m_audio.push_back(static_cast<int16_t>(m_audio_left / 8 + dac_left));
			m_audio.push_back(static_cast<int16_t>(m_audio_right / 8 + dac_right));
			m_audio_ticks = 0;
			m_audio_left = m_audio_right = 0;
		}
	}
	// Hosts that never drain the buffer (headless tools) keep only ~1 second.
	if (m_audio.size() > AUDIO_RATE * 2 * 2)
		m_audio.erase(m_audio.begin(), m_audio.begin() + AUDIO_RATE * 2);
}


void Machine::advance(int cycles)
{
	m_cpu_cycle += cycles;
	advance_audio(cycles);
	bool frame_ended = false;

	if (m_hblank_pending && m_cpu_cycle >= m_line_start_cycle + k2ge::HBLANK_END_CYCLE)
	{
		m_hblank_pending = false;
		m_video.end_hblank();
	}

	while (m_cpu_cycle >= m_line_start_cycle + k2ge::CYCLES_PER_LINE)
	{
		m_line_start_cycle += k2ge::CYCLES_PER_LINE;
		run_z80_until(m_line_start_cycle);

		if (++m_line == k2ge::LINES_PER_FRAME)
		{
			m_line = 0;
			++m_frame;
			frame_ended = true;
		}
		m_video.begin_line(m_line);
		m_hblank_pending = m_line == k2ge::LINES_PER_FRAME - 1 || m_line < 151;
		if (m_hblank_pending && m_cpu_cycle >= m_line_start_cycle + k2ge::HBLANK_END_CYCLE)
		{
			m_hblank_pending = false;
			m_video.end_hblank();
		}
	}

	if (frame_ended)
	{
		m_frame_done = true;
		if (on_frame_end)
			on_frame_end();
	}
}


void Machine::run_frame()
{
	m_frame_done = false;
	while (!m_frame_done)
	{
		if (m_trace_remaining > 0)
		{
			--m_trace_remaining;
			const ngpc_cpu &c = *m_cpu;
			const int b = c.m_regbank;
			std::fprintf(stderr, "%06X SR=%04X XWA=%08X XBC=%08X XDE=%08X XHL=%08X XIX=%08X XIY=%08X XIZ=%08X XSP=%08X\n",
					c.m_pc.d, c.m_sr.w.l, c.m_xwa[b].d, c.m_xbc[b].d, c.m_xde[b].d, c.m_xhl[b].d,
					c.m_xix.d, c.m_xiy.d, c.m_xiz.d, c.m_xssp.d);
		}
		m_cpu->step();
	}
}


uint8_t Machine::z80_read(void *userdata, uint16_t addr)
{
	auto *self = static_cast<Machine *>(userdata);
	if (addr < 0x1000)
		return self->m_sound_ram[addr];
	if (addr == 0x8000)
		return self->m_io[0x3c];
	return 0xff;
}


void Machine::z80_write(void *userdata, uint16_t addr, uint8_t data)
{
	auto *self = static_cast<Machine *>(userdata);
	if (addr < 0x1000)
		self->m_sound_ram[addr] = data;
	else if (addr == 0x4000 || addr == 0x4001)
		self->m_psg.write(addr & 1, data);
	else if (addr == 0x8000)
		self->m_io[0x3c] = data;
	else if (addr == 0xc000)
		self->m_cpu->set_input_line(TLCS900_INT5, ASSERT_LINE);
}


uint8_t Machine::z80_in(z80 *, uint8_t)
{
	return 0xff;
}


void Machine::z80_out(z80 *z, uint8_t, uint8_t)
{
	auto *self = static_cast<Machine *>(z->userdata);
	// Any Z80 port write acknowledges the interrupt (MAME ngp.cpp).
	z->int_pending = false;
	self->m_cpu->set_input_line(TLCS900_INT5, CLEAR_LINE);
}

} // namespace ngpc
