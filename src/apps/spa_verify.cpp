// spa-verify: runs the interpreter and the recompiled game side by side on two
// identical machines and checks that they stay in exact lockstep. After every
// frame the CPU registers, cycle counter, work RAM, sound RAM, I/O registers
// and video RAM must match.
//
//   spa-verify --rom <cart> [--frames N] [--out dir] [--input buttons.txt] [--drop-dash] [--option-screen]
//
// --input feeds both machines the same buttons (the format ngpc-run reads), to
// check a stretch of play and not only the boot and title; --drop-dash switches
// on the optional move (ngpc/drop_dash.h) in both, and --option-screen the host's rows on the
// Game Option screen (ngpc/option_screen.h).

#include "apps/input_log.h"
#include "common/png.h"
#include "ngpc/drop_dash.h"
#include "ngpc/option_screen.h"
#include "ngpc/machine.h"
#include "recomp_rt/game.h"
#include "recomp_rt/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void describe_cpu(const char *label, const ngpc_cpu &c)
{
	std::printf("  %s PC=%06X SR=%04X bank=%d XSP=%08X XIX=%08X XIY=%08X XIZ=%08X\n", label,
			c.m_pc.d, c.m_sr.w.l, c.m_regbank, c.m_xssp.d, c.m_xix.d, c.m_xiy.d, c.m_xiz.d);
	for (int b = 0; b < 4; ++b)
		std::printf("     bank%d XWA=%08X XBC=%08X XDE=%08X XHL=%08X\n", b,
				c.m_xwa[b].d, c.m_xbc[b].d, c.m_xde[b].d, c.m_xhl[b].d);
}

bool cpu_equal(const ngpc_cpu &a, const ngpc_cpu &b)
{
	for (int i = 0; i < 4; ++i)
		if (a.m_xwa[i].d != b.m_xwa[i].d || a.m_xbc[i].d != b.m_xbc[i].d ||
				a.m_xde[i].d != b.m_xde[i].d || a.m_xhl[i].d != b.m_xhl[i].d)
			return false;
	return a.m_xix.d == b.m_xix.d && a.m_xiy.d == b.m_xiy.d && a.m_xiz.d == b.m_xiz.d &&
			a.m_xssp.d == b.m_xssp.d && a.m_pc.d == b.m_pc.d && a.m_sr.w.l == b.m_sr.w.l &&
			a.m_f2.d == b.m_f2.d && a.m_regbank == b.m_regbank &&
			std::memcmp(a.m_int_reg, b.m_int_reg, sizeof(a.m_int_reg)) == 0;
}

int report_block(const char *what, uint32_t base, const uint8_t *a, const uint8_t *b, size_t size)
{
	int count = 0;
	for (size_t i = 0; i < size; ++i)
	{
		if (a[i] != b[i])
		{
			if (count < 16)
				std::printf("  %s %06zX: interpreter %02X recompiled %02X\n", what, base + i, a[i], b[i]);
			++count;
		}
	}
	return count;
}

// How a drop dash hook sends the CPU to the game's code: the recompiled game
// unwinds to the runtime, the interpreted one takes the new program counter.
void redirect_hook(tlcs900_device &cpu, uint32_t pc)
{
	if (rt::on_recompiled_game_thread())
		rt::unwind(static_cast<Cpu *>(&cpu), pc);
	ngpc::redirect_interpreted(cpu, pc);
}

} // namespace

int main(int argc, char **argv)
{
	fs::path rom_path;
	fs::path out_dir = "out/spa-verify";
	fs::path input_path;
	bool drop_dash = false;
	bool option_screen = false;
	int frames = 600;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (arg == "--rom") rom_path = next();
		else if (arg == "--frames") frames = std::stoi(next());
		else if (arg == "--out") out_dir = next();
		else if (arg == "--input") input_path = next();
		else if (arg == "--drop-dash") drop_dash = true;
		else if (arg == "--option-screen") option_screen = true;
	}
	if (rom_path.empty())
	{
		std::cerr << "usage: spa-verify --rom <cart> [--frames N] [--out dir] [--input buttons.txt] [--drop-dash] [--option-screen]\n";
		return 2;
	}

	const std::vector<uint8_t> rom = read_file(rom_path);
	const std::map<int, uint8_t> inputs = input_path.empty() ? std::map<int, uint8_t>{} : read_input_log(input_path);
	if (drop_dash || option_screen)
		ngpc::set_hook_redirect(redirect_hook);
	if (drop_dash)
		ngpc::set_drop_dash(true);
	if (option_screen)
		ngpc::set_option_screen(true);
	ngpc::Machine interpreter;
	ngpc::Machine recompiled;
	for (ngpc::Machine *m : { &interpreter, &recompiled })
	{
		if (const std::string error = m->load_cartridge(rom); !error.empty())
		{
			std::cerr << error << "\n";
			return 1;
		}
		m->boot();
	}
	fs::create_directories(out_dir);

	rt::RecompiledGame game(recompiled);
	double interp_seconds = 0, recomp_seconds = 0;

	for (int frame = 0; frame < frames; ++frame)
	{
		if (const auto it = inputs.find(frame); it != inputs.end())
		{
			interpreter.set_buttons(it->second);
			recompiled.set_buttons(it->second);
		}
		auto t0 = std::chrono::steady_clock::now();
		interpreter.run_frame();
		auto t1 = std::chrono::steady_clock::now();
		game.run_frame();
		auto t2 = std::chrono::steady_clock::now();
		interp_seconds += std::chrono::duration<double>(t1 - t0).count();
		recomp_seconds += std::chrono::duration<double>(t2 - t1).count();

		const ngpc_cpu &a = interpreter.cpu();
		const ngpc_cpu &b = recompiled.cpu();
		bool ok = cpu_equal(a, b) && interpreter.cpu_cycle() == recompiled.cpu_cycle();
		int diffs = 0;
		diffs += std::memcmp(interpreter.work_ram(), recompiled.work_ram(), 0x3000) != 0;
		diffs += std::memcmp(interpreter.sound_ram(), recompiled.sound_ram(), 0x1000) != 0;
		diffs += std::memcmp(interpreter.io_registers(), recompiled.io_registers(), 0x40) != 0;
		diffs += std::memcmp(interpreter.video().vram, recompiled.video().vram, 0x4000) != 0;
		if (!ok || diffs)
		{
			std::printf("MISMATCH after frame %d (cycles %lld vs %lld)\n", frame,
					static_cast<long long>(interpreter.cpu_cycle()), static_cast<long long>(recompiled.cpu_cycle()));
			describe_cpu("interpreter", a);
			describe_cpu("recompiled ", b);
			report_block("ram", 0x4000, interpreter.work_ram(), recompiled.work_ram(), 0x3000);
			report_block("sound", 0x7000, interpreter.sound_ram(), recompiled.sound_ram(), 0x1000);
			report_block("io", 0x80, interpreter.io_registers(), recompiled.io_registers(), 0x40);
			report_block("vram", 0x8000, interpreter.video().vram, recompiled.video().vram, 0x4000);
			write_png(out_dir / "interpreter.png", interpreter.framebuffer(), k2ge::SCREEN_WIDTH, k2ge::SCREEN_HEIGHT);
			write_png(out_dir / "recompiled.png", recompiled.framebuffer(), k2ge::SCREEN_WIDTH, k2ge::SCREEN_HEIGHT);
			return 1;
		}
		if (frame % 100 == 0)
		{
			char name[64];
			std::snprintf(name, sizeof(name), "recompiled_%05d.png", frame);
			write_png(out_dir / name, recompiled.framebuffer(), k2ge::SCREEN_WIDTH, k2ge::SCREEN_HEIGHT);
		}
	}

	std::printf("%d frames in exact lockstep\n", frames);
	std::printf("interpreter %.2fs, recompiled %.2fs\n", interp_seconds, recomp_seconds);
	const rt::Stats &stats = rt::stats();
	std::printf("recompiled routine calls %llu, interpreted steps %llu, unwinds %llu, dispatch misses %llu\n",
			static_cast<unsigned long long>(stats.recompiled_calls),
			static_cast<unsigned long long>(stats.interpreted_steps),
			static_cast<unsigned long long>(stats.unwinds),
			static_cast<unsigned long long>(stats.dispatch_misses));
	for (const auto &[address, count] : stats.miss_addresses)
		std::printf("  miss %06X x%llu\n", address, static_cast<unsigned long long>(count));
	return 0;
}
