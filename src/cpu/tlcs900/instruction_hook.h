// Hooks that run just before the instruction at a given address executes,
// for both interpreted and recompiled code, so the host can adjust registers
// or memory at chosen points without changing the generated code.
//
// Hooks are global (they apply to every CPU in the process) and are off until
// one is added; with none added the cost is one test of a flag per
// instruction. Only addresses in the cartridge (0x200000-0x3FFFFF) can be
// hooked.
#pragma once

#include <cstdint>

class tlcs900_device;

namespace tlcs900_hook {

using Function = void (*)(tlcs900_device &cpu, void *context);

// Runs `function(cpu, context)` before each execution of the instruction at
// `pc`. Several hooks at one address run in the order they were added.
void add(uint32_t pc, Function function, void *context = nullptr);

// Removes every hook.
void clear();

} // namespace tlcs900_hook
