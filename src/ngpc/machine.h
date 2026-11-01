// Neo Geo Pocket Color machine used by the Sonic Pocket recomp runtime.
//
// Memory map and I/O behaviour follow MAME's src/mame/snk/ngp.cpp. The BIOS is
// high-level emulated: boot state, interrupt dispatch and system calls match
// the Mednafen/BizHawk NGP core so runs can be compared with its RAM dumps.
#pragma once

#include "ngpc/k2ge.h"
#include "ngpc/ngpc_cpu.h"
#include "ngpc/t6w28.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "z80.h"
}

namespace ngpc {

// Controller bits as read from I/O register 0xB0.
enum Button : uint8_t
{
	BUTTON_UP = 0x01,
	BUTTON_DOWN = 0x02,
	BUTTON_LEFT = 0x04,
	BUTTON_RIGHT = 0x08,
	BUTTON_A = 0x10,
	BUTTON_B = 0x20,
	BUTTON_OPTION = 0x40,
};

struct RtcTime
{
	int year = 2000;   // full year; stored as two BCD digits
	int month = 1;
	int day = 1;
	int hour = 0;
	int minute = 0;
	int second = 0;
	int day_of_week = 6;
};

class Machine : public tlcs900_bus
{
public:
	static constexpr uint32_t CART_BASE = 0x200000;
	static constexpr uint32_t BIOS_BASE = 0xff0000;
	static constexpr int CPU_CLOCK = 6144000;

	Machine();
	~Machine() override;

	// Loads a cartridge image. Returns an empty string on success.
	std::string load_cartridge(std::vector<uint8_t> image);

	// Puts the machine in the state the BIOS leaves it in when it starts the
	// cartridge (entry point, system RAM, I/O and video defaults).
	void boot(int language = 1, const RtcTime &rtc = {});

	// Runs one video frame (199 lines, 102485 CPU cycles) with the
	// interpreter.
	void run_frame();

	// Called when the last line of a frame has finished. The recompiled
	// runtime uses this to hand the finished frame to the host.
	std::function<void()> on_frame_end;

	// Calls `fn(address, value, is_write)` for every access to main RAM in
	// [first, last), for tooling. It never changes what the machine does, and
	// costs one comparison per RAM access when nothing is watched.
	void watch_ram(uint32_t first, uint32_t last, std::function<void(uint32_t, uint8_t, bool)> fn)
	{
		m_watch_first = first;
		m_watch_last = last;
		m_watch = std::move(fn);
	}

	void set_buttons(uint8_t buttons) { m_buttons = buttons; }

	// Prints the next `count` instructions' PC and registers to stderr.
	void trace_instructions(int64_t count) { m_trace_remaining = count; }

	const uint32_t *framebuffer() const { return m_video.framebuffer; }
	uint64_t frame_count() const { return m_frame; }

	// Main work RAM, 0x4000-0x6FFF.
	uint8_t *work_ram() { return m_ram; }
	const uint8_t *sound_ram() const { return m_sound_ram; }
	const uint8_t *io_registers() const { return m_io; }
	const std::vector<uint8_t> &cartridge() const { return m_cart; }
	// The cartridge as the game reads it, for the host to change level data in
	// (see wide_levels.h). The game's save area is the last 32 KiB of it.
	std::vector<uint8_t> &cartridge_data() { return m_cart; }
	int64_t cpu_cycle() const { return m_cpu_cycle; }

	// tlcs900_bus
	uint8_t read_byte(offs_t addr) override;
	void write_byte(offs_t addr, uint8_t data) override;

	ngpc_cpu &cpu() { return *m_cpu; }
	k2ge &video() { return m_video; }

	// Stereo audio produced so far, interleaved left/right at AUDIO_RATE.
	// The host takes the samples it wants and clears the buffer.
	static constexpr int AUDIO_RATE = 48000;
	std::vector<int16_t> &audio_samples() { return m_audio; }

private:
	enum FlashState
	{
		F_READ,
		F_PROG1,
		F_PROG2,
		F_COMMAND,
		F_ID_READ,
		F_AUTO_PROGRAM,
		F_AUTO_CHIP_ERASE,
		F_AUTO_BLOCK_ERASE,
		F_BLOCK_PROTECT,
	};

	uint8_t io_read(offs_t offset);
	void io_write(offs_t offset, uint8_t data);
	void flash_write(offs_t offset, uint8_t data);
	void restore_flash_id_bytes();
	void advance_audio(int cycles);

	bool bios_trap(offs_t pc);
	void bios_system_call(int function);
	void bios_return_from_call();
	void bios_return_from_interrupt();

	// Advances video/sound timing after each CPU step.
	void advance(int cycles);
	void run_z80_until(int64_t target_cpu_cycle);

	static uint8_t z80_read(void *userdata, uint16_t addr);
	static void z80_write(void *userdata, uint16_t addr, uint8_t data);
	static uint8_t z80_in(z80 *z, uint8_t port);
	static void z80_out(z80 *z, uint8_t port, uint8_t data);

	std::unique_ptr<ngpc_cpu> m_cpu;
	k2ge m_video;
	t6w28 m_psg;
	uint8_t m_dac_left = 0x80;
	uint8_t m_dac_right = 0x80;
	int m_audio_cycles = 0;            // CPU cycles towards the next PSG tick
	int m_audio_ticks = 0;             // PSG ticks accumulated for this sample
	int m_audio_left = 0;
	int m_audio_right = 0;
	std::vector<int16_t> m_audio;
	z80 m_z80 = {};
	bool m_z80_running = false;
	uint8_t m_old_to3 = 0;

	std::vector<uint8_t> m_cart;        // flash contents, mutable
	std::vector<uint8_t> m_bios;        // synthetic 64 KiB BIOS image
	uint8_t m_ram[0x3000] = {};
	uint8_t m_sound_ram[0x1000] = {};
	uint8_t m_io[0x40] = {};
	uint8_t m_buttons = 0;

	FlashState m_flash_state = F_READ;
	uint8_t m_flash_command = 0;
	uint8_t m_flash_id_backup[16] = {};

	int64_t m_cpu_cycle = 0;           // total CPU cycles since boot
	int64_t m_z80_cycle_target = 0;    // in CPU cycles
	uint64_t m_frame = 0;
	int64_t m_line_start_cycle = 0;    // ideal start of the current line
	int m_line = 0;
	bool m_hblank_pending = false;     // HBlank pin raised this line
	bool m_frame_done = false;
	int64_t m_trace_remaining = 0;

	uint32_t m_watch_first = 0;
	uint32_t m_watch_last = 0;
	std::function<void(uint32_t, uint8_t, bool)> m_watch;
};

} // namespace ngpc
