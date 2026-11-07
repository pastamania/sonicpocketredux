#include "ngpc/drop_dash.h"

#include "cpu/tlcs900/instruction_hook.h"
#include "cpu/tlcs900/tlcs900.h"

#include <atomic>
#include <unordered_map>

namespace ngpc {

namespace {

// The player's code (see docs/notes/player.md). The player is a task at
// 0x6708 whose function pointer is its state and whose pointer the code keeps
// in XIZ; +0x18 is his ground speed, a signed 8.8 number that is positive in the
// direction he faces. Buttons: 0x4D40 held, 0x4D41 pressed since the last tick;
// A is 0x10 and B 0x20, and both jump.
//
//   Plr_Jumping    0x39AA86  the jump: plays the sound, sets the upward speed
//   0x39AAF7                 the airborne state's body, run once per tick in the
//                            air (the jump's first ticks, while a button is held,
//                            reach it from 0x39AAEB)
//   0x39AB4E                 on landing: `LD (XIZ),XWA` with XWA = Plr_Walking
//                            (0x399EE5), after the speed on the ground has been
//                            worked out; the lines after it make the state
//                            Plr_Rolling (0x399F3B) when Down is held
//   Plr_Hurt 0x39AA54, 0x39A98A (the spring's launch), Death_Normal 0x39A9BF
//                            other ways into the air
constexpr uint32_t PLR_JUMPING = 0x39AA86;
constexpr uint32_t PLR_AIR = 0x39AAF7;
constexpr uint32_t PLR_LANDING = 0x39AB4E;
constexpr uint32_t PLR_HURT = 0x39AA54;
constexpr uint32_t PLR_SPRING = 0x39A98A;
constexpr uint32_t PLR_DEATH = 0x39A9BF;
constexpr uint32_t PLR_ROLLING = 0x399F3B;
constexpr uint32_t PLAYER_SPEED = 0x6708 + 0x18;
constexpr uint32_t PLAYER_ANIMATION = 0x6708 + 0x1E;   // long: where the animation script has got to
constexpr uint32_t INPUT_HELD = 0x4D40, INPUT_PRESSED = 0x4D41;
constexpr uint8_t JUMP_BUTTONS = 0x30;

// The spindash's launch speeds by charge, 0 to 8 (0x0A00 to 0x0E00), and its
// two sounds: the charge (0xA3) and the release (0xA2). PlaySound takes the
// sound in WA.
constexpr uint32_t LAUNCH_TABLE = 0x3993BC;
constexpr int LAUNCH_LAST = 8;
constexpr uint16_t SOUND_CHARGE = 0xA3, SOUND_RELEASE = 0xA2;

// Mania charges a drop dash for 20 frames; the game's ticks are every second frame.
constexpr int CHARGE_TICKS = 10;

enum class Phase
{
	Idle,       // not in a jump (or one that cannot dash)
	Armed,      // jumped: a new press of a jump button starts the charge
	Charging,   // a jump button held since that press
	Charged,    // ready: landing with the button held makes the dash
};

// What each CPU is doing. The hooks are global (spa-verify runs two machines),
// so this is by CPU.
struct State
{
	Phase phase = Phase::Idle;
	int ticks = 0;     // ticks of the charge so far
	int skip = 0;      // visits of the airborne body that are the jump's own

	// A call to PlaySound under way for the hook at its return address (hook_call.h);
	// one hook's call at a time, as only one of them is waiting for it.
	SoundCall sound;
};

std::atomic<bool> s_enabled{ false };
bool s_installed = false;
// The CPU runs on one thread at a time (the host and the recompiled game's
// thread take turns), so this needs no lock.
std::unordered_map<const tlcs900_device *, State> s_states;

State &state_of(tlcs900_device &cpu)
{
	return s_states[&cpu];
}

// The state of a CPU that has one: with the dash switched off, a call to
// PlaySound that was under way when it was still has to be finished.
State *existing_state(tlcs900_device &cpu)
{
	const auto it = s_states.find(&cpu);
	return it == s_states.end() ? nullptr : &it->second;
}

// The call to PlaySound and the way back are in hook_call.h; the state of a CPU's call is its own.
bool call_sound(tlcs900_device &cpu, State &st, uint32_t pc, uint16_t sound)
{
	return ngpc::call_sound(cpu, st.sound, pc, sound);
}

bool resume(tlcs900_device &cpu, State &st, uint32_t pc)
{
	return resume_sound(cpu, st.sound, pc);
}

// The jump's spin is a script of eight entries run in a loop, one a tick, at
// 0x398B20..0x398B4F (six bytes each: 0x000C, the frame, 0x0103; the frames are
// 12 16 14 15 12 13 16 15, then a jump back to the start), and the player's task
// keeps the address of the entry it runs next at +0x1E. The picture shows an entry
// a tick after it ran. Only frame 0x16 is the solid blue ball (the script has it
// twice; the other frames show his shoes and eyes), so while he charges the script
// is put at the first of them at the start of every tick, and he stays a ball.
constexpr uint32_t SPIN_FIRST = 0x398B20, SPIN_END = 0x398B50;
constexpr uint32_t SPIN_BALL = 0x398B26;

void hold_ball(tlcs900_device &cpu)
{
	const uint32_t at = cpu.RDMEML(PLAYER_ANIMATION);
	if (at >= SPIN_FIRST && at <= SPIN_END)
		cpu.WRMEML(PLAYER_ANIMATION, SPIN_BALL);
}

void on_jump(tlcs900_device &cpu, void *)
{
	if (!s_enabled)
		return;
	State &st = state_of(cpu);
	st.phase = Phase::Armed;
	st.ticks = 0;
	// The jump's own press may still be pending on the first visit of the body.
	st.skip = 1;
}

// Other ways off the ground, which drop a charge.
void on_leave(tlcs900_device &cpu, void *)
{
	if (!s_enabled)
		return;
	state_of(cpu).phase = Phase::Idle;
}

void on_air(tlcs900_device &cpu, void *)
{
	State *found = s_enabled ? &state_of(cpu) : existing_state(cpu);
	if (!found)
		return;
	State &st = *found;
	if (resume(cpu, st, PLR_AIR) || !s_enabled)
		return;
	if (st.phase == Phase::Idle)
		return;
	if (st.skip > 0)
	{
		--st.skip;
		return;
	}
	const bool held = (cpu.RDMEM(INPUT_HELD) & JUMP_BUTTONS) != 0;
	const bool pressed = (cpu.RDMEM(INPUT_PRESSED) & JUMP_BUTTONS) != 0;
	bool charged_now = false;
	switch (st.phase)
	{
	case Phase::Armed:
		if (pressed)
		{
			st.phase = Phase::Charging;
			st.ticks = 0;
		}
		break;
	case Phase::Charging:
		if (!held)
			st.phase = Phase::Armed;
		else if (++st.ticks >= CHARGE_TICKS)
		{
			st.phase = Phase::Charged;
			charged_now = true;
		}
		break;
	case Phase::Charged:
		if (!held)
			st.phase = Phase::Armed;
		break;
	case Phase::Idle:
		break;
	}

	// While he charges he is the solid ball of the spin and not the frames
	// that show his shoes and eyes.
	if (st.phase == Phase::Charging || st.phase == Phase::Charged)
		hold_ball(cpu);
	if (charged_now)
		call_sound(cpu, st, PLR_AIR, SOUND_CHARGE);
}

void on_landing(tlcs900_device &cpu, void *)
{
	State *found = s_enabled ? &state_of(cpu) : existing_state(cpu);
	if (!found)
		return;
	State &st = *found;
	const bool resumed = resume(cpu, st, PLR_LANDING);
	if (!s_enabled)
		return;
	if (!resumed)
	{
		const bool dash = st.phase == Phase::Charged && (cpu.RDMEM(INPUT_HELD) & JUMP_BUTTONS) != 0;
		st.phase = Phase::Idle;
		if (!dash)
			return;
		if (call_sound(cpu, st, PLR_LANDING, SOUND_RELEASE))
			return;
	}

	// The ground speed the landing has just worked out becomes a quarter of
	// itself (half when he lands going backwards) plus the dash, up to the cap.
	int speed = static_cast<int16_t>(cpu.RDMEMW(PLAYER_SPEED));
	speed = speed >= 0 ? speed >> 2 : speed >> 1;
	const int base = static_cast<int16_t>(cpu.RDMEMW(LAUNCH_TABLE));
	const int cap = static_cast<int16_t>(cpu.RDMEMW(LAUNCH_TABLE + 2 * LAUNCH_LAST));
	speed += base;
	if (speed > cap)
		speed = cap;
	if (speed < -cap)
		speed = -cap;
	cpu.WRMEMW(PLAYER_SPEED, static_cast<uint16_t>(speed));
	// The instruction about to run stores XWA as the player's state: a roll, not a walk.
	*cpu.get_reg32_current(0) = PLR_ROLLING;
}

} // namespace

void set_drop_dash(bool enabled)
{
	// Drop any charge, but not the record of a call to PlaySound that is under way.
	for (auto &entry : s_states)
		entry.second.phase = Phase::Idle;
	s_enabled = enabled;
	if (enabled && !s_installed)
	{
		tlcs900_hook::add(PLR_JUMPING, on_jump);
		tlcs900_hook::add(PLR_AIR, on_air);
		tlcs900_hook::add(PLR_LANDING, on_landing);
		tlcs900_hook::add(PLR_HURT, on_leave);
		tlcs900_hook::add(PLR_SPRING, on_leave);
		tlcs900_hook::add(PLR_DEATH, on_leave);
		s_installed = true;
	}
}

bool drop_dash_enabled()
{
	return s_enabled;
}

} // namespace ngpc
