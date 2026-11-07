#include "ngpc/hook_call.h"

#include "cpu/tlcs900/tlcs900.h"

namespace ngpc {

namespace {

constexpr uint32_t PLAY_SOUND = 0x3F1883;

HookRedirect s_redirect = nullptr;

} // namespace

void set_hook_redirect(HookRedirect redirect)
{
	s_redirect = redirect;
}

void redirect_interpreted(tlcs900_device &cpu, uint32_t pc)
{
	cpu.m_pc.d = pc;
	cpu.m_prefetch_clear = true;
}

bool call_sound(tlcs900_device &cpu, SoundCall &call, uint32_t pc, uint16_t sound)
{
	if (!s_redirect)
		return false;
	for (int i = 0; i < 7; ++i)
		call.saved[i] = *cpu.get_reg32_current(static_cast<uint8_t>(i));
	call.flags = cpu.m_sr.b.l;
	uint32_t *sp = cpu.get_reg32_current(7);
	call.sp_before = *sp;
	call.return_pc = pc;
	call.calling = true;
	// The call: PlaySound returns to `pc`.
	*sp -= 4;
	cpu.WRMEML(*sp, pc);
	*cpu.get_reg16_current(0) = sound;
	s_redirect(cpu, PLAY_SOUND);
	return true;
}

bool resume_sound(tlcs900_device &cpu, SoundCall &call, uint32_t pc)
{
	if (!call.calling)
		return false;
	call.calling = false;
	if (call.return_pc != pc || *cpu.get_reg32_current(7) != call.sp_before)
		return false;   // not our return (a reset, say): an ordinary visit
	for (int i = 0; i < 7; ++i)
		*cpu.get_reg32_current(static_cast<uint8_t>(i)) = call.saved[i];
	cpu.m_sr.b.l = call.flags;
	return true;
}

} // namespace ngpc
