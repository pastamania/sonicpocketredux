// Checks of how a wide picture's extra width is shared either side of the screen, and how far the
// game's sprite tests are told the view reaches (see wide_sprites.h). They need no cartridge.
// Run by ctest.

#include "ngpc/k2ge_render.h"
#include "ngpc/wide_sprites.h"

#include <cstdio>
#include <memory>

namespace
{

int failures = 0;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
			++failures; \
		} \
	} while (0)

// A level scene whose camera can show world x 64 to 4672 (as Aero Base's).
std::unique_ptr<k2ge_render::WideScene> level(int min_x = 64, int max_x = 4672)
{
	auto scene = std::make_unique<k2ge_render::WideScene>();
	scene->active = true;
	scene->min_x = min_x;
	scene->max_x = max_x;
	return scene;
}

void split_is(const k2ge_render::WideScene &scene, int world_x, int extra, int want_left, int want_right, int line)
{
	int left = -1, right = -1;
	k2ge_render::wide_split(scene, world_x, extra, left, right);
	if (left != want_left || right != want_right)
	{
		std::fprintf(stderr, "line %d: wide_split(world_x %d, extra %d) gave %d, %d; wanted %d, %d\n",
				line, world_x, extra, left, right, want_left, want_right);
		++failures;
	}
}

#define SPLIT(scene, world_x, extra, left, right) split_is(scene, world_x, extra, left, right, __LINE__)

void test_split()
{
	const auto scene = level();

	// In the middle the extra is shared evenly (the odd pixel goes to the right).
	SPLIT(*scene, 2000, 110, 55, 55);
	SPLIT(*scene, 2000, 83, 41, 42);
	SPLIT(*scene, 2000, 0, 0, 0);

	// At the very start of the level, all of it is to the right of the screen, and it moves
	// across as the camera does until it is even.
	SPLIT(*scene, 64, 110, 0, 110);
	SPLIT(*scene, 64 + 30, 110, 30, 80);
	SPLIT(*scene, 64 + 55, 110, 55, 55);
	SPLIT(*scene, 64 + 300, 110, 55, 55);
	SPLIT(*scene, 64, 83, 0, 83);

	// And at the end, all of it to the left.
	SPLIT(*scene, 4672 - 160, 110, 110, 0);
	SPLIT(*scene, 4672 - 160 - 30, 110, 80, 30);
	SPLIT(*scene, 4672 - 160 - 55, 110, 55, 55);

	// The camera is locked in an arena or the act clear: even, wherever it is.
	{
		auto arena = level();
		arena->arena = true;
		SPLIT(*arena, 64, 110, 55, 55);
		SPLIT(*arena, 4672 - 160, 110, 55, 55);
		auto clear = level();
		clear->act_clear = true;
		SPLIT(*clear, 64, 110, 55, 55);
	}

	// A range narrower than the picture (a locked camera): even, the rest is left black.
	{
		auto narrow = level(100, 200);
		SPLIT(*narrow, 100, 110, 55, 55);
	}

	// Whatever the placement, the two sides add up to the extra and neither is negative.
	for (int extra : { 83, 110 })
		for (int x = 0; x < 4800; x += 7)
		{
			int left = -1, right = -1;
			k2ge_render::wide_split(*scene, x, extra, left, right);
			CHECK(left >= 0 && right >= 0 && left + right == extra);
		}
}

void reach_is(int want_left, int want_right, int line)
{
	int left = -1, right = -1;
	ngpc::wide_view_reach(left, right);
	if (left != want_left || right != want_right)
	{
		std::fprintf(stderr, "line %d: the view reaches %d, %d; wanted %d, %d\n", line, left, right, want_left, want_right);
		++failures;
	}
}

#define REACH(left, right) reach_is(left, right, __LINE__)

void test_reach()
{
	const auto scene = level();
	constexpr int EXTRA = 110, MARGIN = 55;

	// With no margin the hooks are not in play, and nothing is tracked.
	ngpc::set_wide_sprite_margin(0);
	scene->world_x = 64;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(0, 0);

	// With one, until the host says otherwise the view reaches the even share each side.
	ngpc::set_wide_sprite_margin(MARGIN);
	REACH(MARGIN, MARGIN);

	// At the start of the level the right side reaches all of the extra, the left keeps the share.
	scene->world_x = 64;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(MARGIN, EXTRA);

	// Camera in the middle: back to the share.
	scene->world_x = 2000;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(MARGIN, MARGIN);

	// At the end the left side reaches it all.
	scene->world_x = 4672 - 160;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(EXTRA, MARGIN);

	// Not a level (a screen shown as the machine draws it): the share again.
	ngpc::track_wide_view(nullptr, EXTRA);
	REACH(MARGIN, MARGIN);

	// The camera is followed: the sprites of the next tick are made for where it will be. At 52
	// pixels from the end of the range the view is shared 58 left, 52 right.
	ngpc::track_wide_view(nullptr, EXTRA);   // forget where the camera was
	scene->world_x = 4460;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(58, MARGIN);                       // no step to follow yet
	// Eight on, at 44 from the end it is 66 left; going on at that rate it will be 36 from the end,
	// 74 left, in the tick the sprites are made for.
	scene->world_x = 4468;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(74, MARGIN);
	// A frame in which the game ran no tick leaves the camera where it was: the step is kept.
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(74, MARGIN);
	// Going the other way at the start of the level: the right side keeps all of the extra.
	ngpc::track_wide_view(nullptr, EXTRA);
	scene->world_x = 64 + 40;
	ngpc::track_wide_view(scene.get(), EXTRA);
	scene->world_x = 64 + 32;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(MARGIN, EXTRA - 24);   // the camera may be at 64 + 24 or 64 + 40: 86 beyond the right, or 70

	// A jump (a respawn) is not a step to follow: the reach is that of the new place alone.
	scene->world_x = 2000;
	ngpc::track_wide_view(scene.get(), EXTRA);
	scene->world_x = 64;
	ngpc::track_wide_view(scene.get(), EXTRA);
	REACH(MARGIN, EXTRA);

	ngpc::set_wide_sprite_margin(0);
}

} // namespace

int main()
{
	test_split();
	test_reach();
	if (failures)
	{
		std::fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	std::printf("wide view tests passed\n");
	return 0;
}
