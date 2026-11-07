#include "recomp_rt/game.h"

#include "recomp_rt/runtime.h"

#include <cstdio>
#include <exception>
#include <semaphore>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace rt {

namespace {

// Recompiled routines nest like the original calls; give them room.
constexpr size_t GAME_THREAD_STACK = 256u * 1024 * 1024;

struct Stop
{
};

thread_local bool t_game_thread = false;

} // namespace

bool on_recompiled_game_thread()
{
	return t_game_thread;
}

struct RecompiledGame::Impl
{
	ngpc::Machine &machine;
	std::binary_semaphore to_game{ 0 };
	std::binary_semaphore to_host{ 0 };
	bool stop = false;
	bool finished = false;
#if defined(_WIN32)
	HANDLE thread = nullptr;
#else
	pthread_t thread{};
#endif

	explicit Impl(ngpc::Machine &m) : machine(m) {}

	void body()
	{
		t_game_thread = true;
		to_game.acquire();
		if (!stop)
		{
			try
			{
				run(&machine.cpu());
			}
			catch (const Stop &)
			{
			}
			catch (const std::exception &error)
			{
				std::fprintf(stderr, "recompiled game stopped: %s (PC=%06X)\n", error.what(), machine.cpu().m_pc.d);
			}
		}
		finished = true;
		to_host.release();
	}

#if defined(_WIN32)
	static DWORD WINAPI entry(LPVOID self)
	{
		static_cast<Impl *>(self)->body();
		return 0;
	}
#else
	static void *entry(void *self)
	{
		static_cast<Impl *>(self)->body();
		return nullptr;
	}
#endif
};


RecompiledGame::RecompiledGame(ngpc::Machine &machine) :
	m_impl(std::make_unique<Impl>(machine))
{
	Impl *impl = m_impl.get();
	machine.on_frame_end = [impl]() {
		impl->to_host.release();
		impl->to_game.acquire();
		if (impl->stop)
			throw Stop{};
	};
#if defined(_WIN32)
	impl->thread = CreateThread(nullptr, GAME_THREAD_STACK, &Impl::entry, impl,
			STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
#else
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, GAME_THREAD_STACK);
	pthread_create(&impl->thread, &attr, &Impl::entry, impl);
	pthread_attr_destroy(&attr);
#endif
}


RecompiledGame::~RecompiledGame()
{
	Impl *impl = m_impl.get();
	if (!impl->finished)
	{
		impl->stop = true;
		impl->to_game.release();
		impl->to_host.acquire();
	}
#if defined(_WIN32)
	WaitForSingleObject(impl->thread, INFINITE);
	CloseHandle(impl->thread);
#else
	pthread_join(impl->thread, nullptr);
#endif
	impl->machine.on_frame_end = nullptr;
}


void RecompiledGame::run_frame()
{
	if (m_impl->finished)
		return;
	m_impl->to_game.release();
	m_impl->to_host.acquire();
}

} // namespace rt
