#include "ngpc/wide_sprites.h"

#include "cpu/tlcs900/instruction_hook.h"
#include "cpu/tlcs900/tlcs900.h"

namespace ngpc {

namespace {

// The JR that follows each comparison, and the comparison's registers:
//   0x23C0C8 and 0x23C14F  after  CP DE,0xA6  (CopySprites): DE is the sprite's
//                          x plus 7, and a sprite is kept if 0 <= DE <= 0xA6
//   0x23C235               after  CP BC,0x120 (AddSpriteObject): BC is the
//                          object's x plus 64, and it is kept if 0 <= BC <= 0x120
// Both skip with JR UGT: taken when neither carry nor zero is set.
constexpr uint32_t SPRITE_CULL_1 = 0x23C0C8;
constexpr uint32_t SPRITE_CULL_2 = 0x23C14F;
constexpr uint32_t OBJECT_CULL = 0x23C235;
constexpr uint8_t REG_DE = 0xDA, REG_BC = 0xD9;
constexpr int SPRITE_LIMIT = 0xA6, OBJECT_LIMIT = 0x120;
constexpr uint8_t FLAG_C = 0x01, FLAG_Z = 0x40;

// The game's other on-screen test (0x39D858, 21 callers), which decides whether
// an object that is wider than a point is updated and drawn at all:
//   DE = object x - camera x          (SUB DE,IX)
//   DE += half width, WA = 2 * half width + 160
//   CP DE,WA                          unsigned: on screen when DE < WA
//   0x39D874  JR NC -> off screen     the hook sits here
// that is -half width <= x - camera < 160 + half width.
constexpr uint32_t RECT_CULL = 0x39D874;
constexpr uint8_t REG_WA = 0xD8;

// Where the game confines the player to the screen (sub_39C47E, called after
// the player moves): the screen x of the player, in IX, is compared with a
// bound loaded into BC just before each compare.
//   0x39C496  LDW BC,0x0C   0x39C499  CP IX,BC   left bound: clamp if IX < BC
//   0x39C49D  LDW BC,0x94   0x39C4A0  CP IX,BC   right bound: clamp if IX > BC
// and the first boss (boss0_main), which turns round when its x leaves
//   0x3A10E7  IX = camera range start - 0x48
//   0x3A10EB  IY = camera range end + 0xE8
// compared at 0x3A10EF.
constexpr uint32_t PLAYER_LEFT = 0x39C499;
constexpr uint32_t PLAYER_RIGHT = 0x39C4A0;
constexpr uint32_t BOSS0_BOUNDS = 0x3A10EF;
constexpr uint8_t REG_IX = 0xDC, REG_IY = 0xDD;
constexpr int PLAYER_LEFT_BOUND = 0x0C, PLAYER_RIGHT_BOUND = 0x94;

int s_margin = 0;
bool s_installed = false;
bool s_arena = false;
int s_arena_left = 0, s_arena_right = 0;

// Makes the JR that follows decide as if the value were kept (or not).
void decide(tlcs900_device &cpu, bool keep)
{
	if (keep)
	{
		cpu.m_sr.b.l = static_cast<uint8_t>((cpu.m_sr.b.l | FLAG_Z) & ~FLAG_C);   // UGT false: carry on
	}
	else
		cpu.m_sr.b.l &= static_cast<uint8_t>(~(FLAG_C | FLAG_Z));                 // UGT true: skip it
}

void cull(tlcs900_device &cpu, uint8_t reg, int limit, int margin)
{
	if (s_margin == 0)
		return;
	const int value = static_cast<int16_t>(*cpu.get_reg16_current(reg));
	decide(cpu, value >= -margin && value <= limit + margin);
}

void sprite_cull(tlcs900_device &cpu, void *)
{
	// Sprites that touch the wider view: the margin, and the sprite's own
	// width is already in the test (DE is x + 7, a sprite is 8 wide).
	cull(cpu, REG_DE, SPRITE_LIMIT, s_margin);
}

void object_cull(tlcs900_device &cpu, void *)
{
	// Objects are placed by an origin that is not always their left edge, so
	// this test, which is already 64 pixels loose, is given the same slack
	// again.
	cull(cpu, REG_BC, OBJECT_LIMIT, s_margin);
}

void rect_cull(tlcs900_device &cpu, void *)
{
	if (s_margin == 0)
		return;
	// Only the horizontal test: the view is no taller than the screen. JR NC
	// skips the object, so it is kept by setting carry.
	const int value = static_cast<int16_t>(*cpu.get_reg16_current(REG_DE));
	const int limit = static_cast<int16_t>(*cpu.get_reg16_current(REG_WA));
	if (value >= -s_margin && value < limit + s_margin)
		cpu.m_sr.b.l |= FLAG_C;
	else
		cpu.m_sr.b.l &= static_cast<uint8_t>(~FLAG_C);
}

void player_left(tlcs900_device &cpu, void *)
{
	if (s_arena)
		*cpu.get_reg16_current(REG_BC) = static_cast<uint16_t>(PLAYER_LEFT_BOUND - s_arena_left);
}

void player_right(tlcs900_device &cpu, void *)
{
	if (s_arena)
		*cpu.get_reg16_current(REG_BC) = static_cast<uint16_t>(PLAYER_RIGHT_BOUND + s_arena_right);
}

void boss0_bounds(tlcs900_device &cpu, void *)
{
	if (!s_arena)
		return;
	*cpu.get_reg16_current(REG_IX) -= static_cast<uint16_t>(s_arena_left);
	*cpu.get_reg16_current(REG_IY) += static_cast<uint16_t>(s_arena_right);
}

} // namespace

void set_wide_arena(bool active, int left, int right)
{
	s_arena = s_margin != 0 && active;
	s_arena_left = left < 0 ? 0 : left;
	s_arena_right = right < 0 ? 0 : right;
}

void set_wide_sprite_margin(int extra)
{
	s_margin = extra < 0 ? 0 : extra;
	if (s_margin && !s_installed)
	{
		tlcs900_hook::add(SPRITE_CULL_1, sprite_cull);
		tlcs900_hook::add(SPRITE_CULL_2, sprite_cull);
		tlcs900_hook::add(OBJECT_CULL, object_cull);
		tlcs900_hook::add(RECT_CULL, rect_cull);
		tlcs900_hook::add(PLAYER_LEFT, player_left);
		tlcs900_hook::add(PLAYER_RIGHT, player_right);
		tlcs900_hook::add(BOSS0_BOUNDS, boss0_bounds);
		s_installed = true;
	}
	if (s_margin == 0)
		s_arena = false;
}

} // namespace ngpc
