// Runs the recompiled game on its own thread, one video frame at a time.
//
// The recompiled code never returns to the host (the game's main loop is an
// endless task scheduler), so it runs on a dedicated thread. The machine's
// frame-end callback hands control back to the host after every frame and
// waits until the host asks for the next one; only one side ever runs.
#pragma once

#include "ngpc/machine.h"

#include <memory>

namespace rt {

class RecompiledGame
{
public:
	explicit RecompiledGame(ngpc::Machine &machine);
	~RecompiledGame();

	RecompiledGame(const RecompiledGame &) = delete;
	RecompiledGame &operator=(const RecompiledGame &) = delete;

	// Runs the game until the current frame is finished.
	void run_frame();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Whether the calling thread is a recompiled game's, that is whether the code
// that is running is recompiled (and can be redirected with rt::unwind) and
// not interpreted.
bool on_recompiled_game_thread();

} // namespace rt
