cmake_minimum_required(VERSION 3.25)

project(SonicPocket LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# NGPC machine: MAME-derived TLCS-900/H CPU, K2GE video and T6W28 sound, a Z80
# core and a high-level BIOS. Builds without the cartridge.

add_library(tlcs900 STATIC
    src/cpu/tlcs900/tlcs900.cpp
)
target_include_directories(tlcs900 PUBLIC src)
target_compile_features(tlcs900 PUBLIC cxx_std_20)

add_library(z80core STATIC
    third_party/z80/z80.c
)
target_include_directories(z80core PUBLIC third_party/z80)

add_library(ngpc STATIC
    src/ngpc/ngpc_cpu.cpp
    src/ngpc/k2ge.cpp
    src/ngpc/k2ge_render.cpp
    src/ngpc/k2ge_interp.cpp
    src/ngpc/level_map.cpp
    src/ngpc/machine.cpp
    src/ngpc/sprite_trace.cpp
    src/ngpc/wide_levels.cpp
    src/ngpc/wide_sprites.cpp
    src/ngpc/drop_dash.cpp
    src/ngpc/hook_call.cpp
    src/ngpc/option_screen.cpp
    src/ngpc/t6w28.cpp
    src/common/png.cpp
    src/common/cartridge.cpp
)
target_include_directories(ngpc PUBLIC src)
target_link_libraries(ngpc PUBLIC tlcs900 z80core)

# Headless interpreter runs, screenshots and RAM dumps.
add_executable(ngpc-run src/apps/ngpc_run.cpp)
target_link_libraries(ngpc-run PRIVATE ngpc)

# Checks that need no cartridge: `ctest --test-dir <build> -C Release`.
enable_testing()
add_executable(cartridge-test tests/cartridge_test.cpp)
target_link_libraries(cartridge-test PRIVATE ngpc)
add_test(NAME cartridge COMMAND cartridge-test)

# The static recompiler itself.
add_executable(spa-recomp
    src/recomp/main.cpp
    src/recomp/decoder.cpp
)
target_link_libraries(spa-recomp PRIVATE tlcs900)

if(MSVC)
    target_compile_options(tlcs900 PRIVATE /W3)
    target_compile_options(ngpc PRIVATE /W4 /permissive-)
    target_compile_options(ngpc-run PRIVATE /W4 /permissive-)
    target_compile_options(cartridge-test PRIVATE /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# The recompiled game. Generated at build time from the user's cartridge.

set(SPA_ROM "" CACHE FILEPATH "Sonic Pocket Adventure cartridge image used to generate the recompiled code")
set(SPA_LISTING "" CACHE FILEPATH "Optional listing_instructions.txt for disassembly comments in generated code")
option(SPA_PLAYER "Build the sonic-pocket player without a cartridge: it holds none of the game and interprets the cartridge it is given when it runs. This is the build to hand out. Ignored when SPA_ROM is set, which builds the player with the recompiled game in it." OFF)

if(SPA_ROM)
    set(SPA_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
    set(SPA_GENERATED_SOURCES ${SPA_GENERATED_DIR}/dispatch.cpp)
    foreach(shard RANGE 0 31)
        string(LENGTH "${shard}" shard_length)
        if(shard_length EQUAL 1)
            set(shard "0${shard}")
        endif()
        list(APPEND SPA_GENERATED_SOURCES ${SPA_GENERATED_DIR}/game_${shard}.cpp)
    endforeach()

    set(SPA_RECOMP_ARGS
        --rom ${SPA_ROM}
        --functions ${CMAKE_SOURCE_DIR}/config/recomp/functions.txt
        --out ${SPA_GENERATED_DIR}
    )
    if(SPA_LISTING)
        list(APPEND SPA_RECOMP_ARGS --listing ${SPA_LISTING})
    endif()

    add_custom_command(
        OUTPUT ${SPA_GENERATED_SOURCES} ${SPA_GENERATED_DIR}/functions.h
        COMMAND spa-recomp ${SPA_RECOMP_ARGS}
        DEPENDS spa-recomp ${CMAKE_SOURCE_DIR}/config/recomp/functions.txt ${SPA_ROM}
        COMMENT "Recompiling Sonic Pocket Adventure"
        VERBATIM
    )

    add_library(spa_game STATIC
        ${SPA_GENERATED_SOURCES}
        src/recomp_rt/runtime.cpp
        src/recomp_rt/game.cpp
    )
    target_include_directories(spa_game PUBLIC src ${SPA_GENERATED_DIR})
    target_link_libraries(spa_game PUBLIC ngpc)
    if(MSVC)
        # One compiler process: the generated sources are large.
        target_compile_options(spa_game PRIVATE /bigobj /EHsc)
    endif()

    # Lockstep check: recompiled game vs interpreter.
    add_executable(spa-verify src/apps/spa_verify.cpp)
    target_link_libraries(spa-verify PRIVATE spa_game)

endif()

# ---------------------------------------------------------------------------
# The player, with an SDL3 window, input and audio. Two kinds:
#   SPA_ROM set     the game recompiled from that cartridge is linked in (and the
#                   interpreter is still there, --interpreter).
#   SPA_PLAYER on   nothing of the game is linked in; the cartridge is read when the
#                   program runs and interpreted. Needs no cartridge to build, and
#                   the result holds no part of it: this is the one to hand out.

if(SPA_ROM OR SPA_PLAYER)
    include(FetchContent)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.8
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)

    add_executable(sonic-pocket src/apps/sonic_pocket.cpp)
    if(SPA_ROM)
        message(STATUS "sonic-pocket: with the game recompiled from ${SPA_ROM}")
        target_link_libraries(sonic-pocket PRIVATE spa_game SDL3::SDL3)
        target_compile_definitions(sonic-pocket PRIVATE SPA_RECOMPILED)
    else()
        message(STATUS "sonic-pocket: without a cartridge (interpreter only)")
        target_link_libraries(sonic-pocket PRIVATE ngpc SDL3::SDL3)
    endif()
    add_custom_command(
        TARGET sonic-pocket
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:sonic-pocket>
    )
    # The licences go beside the program, and follow any edit to them.
    add_custom_target(sonic-pocket-notice
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/NOTICE.txt
            $<TARGET_FILE_DIR:sonic-pocket>
        COMMENT "Copying NOTICE.txt"
        VERBATIM
    )
    add_dependencies(sonic-pocket sonic-pocket-notice)
endif()
