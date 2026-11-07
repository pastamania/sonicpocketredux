// Calling the game's PlaySound from an instruction hook.
//
// A hook (cpu/tlcs900/instruction_hook.h) runs before an instruction and can change
// registers and memory, but cannot call a routine: recompiled code ignores a change to
// the program counter. The way round is the one the runtime uses for any unexpected
// return: the hook pushes its own address as a return address, sets WA to the sound and
// sends the CPU to PlaySound, which returns to the same instruction. The hook runs again
// there, sees the call is over, puts the registers back and carries on.
//
// How a CPU is "sent" depends on who runs it, so the host supplies that (set_hook_redirect).
//
// Specific to this cartridge: PlaySound is at 0x3F1883 and takes the sound in WA.
#pragma once

#include <cstdint>

class tlcs900_device;

namespace ngpc {

// How a hook sends the CPU to the game's code at `pc`. Recompiled code ignores a change to
// the program counter, so a host that runs it supplies a function that unwinds to the runtime
// instead (rt::unwind) when the CPU is running recompiled code, and uses redirect_interpreted
// otherwise.
using HookRedirect = void (*)(tlcs900_device &cpu, uint32_t pc);
void set_hook_redirect(HookRedirect redirect);

// Sets the program counter: the redirect for the interpreter.
void redirect_interpreted(tlcs900_device &cpu, uint32_t pc);

// A call to PlaySound that is under way for the hook at `return_pc`: the registers to put back
// when it returns there, and the stack pointer it must come back with. One per CPU and hook
// (the hooks are global and spa-verify runs two machines).
struct SoundCall
{
	bool calling = false;
	uint32_t return_pc = 0;
	uint32_t sp_before = 0;
	uint32_t saved[7] = {};   // XWA, XBC, XDE, XHL, XIX, XIY, XIZ
	uint8_t flags = 0;
};

// Sends the CPU to PlaySound with `sound`, to come back to the instruction at `pc` (where the
// hook that calls this sits), which will run its hook again: resume_sound() then puts the
// registers back. Returns false, having done nothing, when there is no way to send the CPU.
// With a redirect that unwinds, it does not return.
bool call_sound(tlcs900_device &cpu, SoundCall &call, uint32_t pc, uint16_t sound);

// Whether the CPU has just come back from that call to the hook at `pc`; if so the registers
// are put back as they were.
bool resume_sound(tlcs900_device &cpu, SoundCall &call, uint32_t pc);

} // namespace ngpc
