# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
cmake_minimum_required(VERSION 3.20)
project(spelunkomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
# (Android: every library goes into a game's shared library, lib<id>.so; its calls of its own functions are not
# interposed: below, where the game is made)
if(ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_compile_options(-fno-semantic-interposition)
endif()

find_package(ZLIB REQUIRED)

add_library(spelunk_machine STATIC
  machine/sh3.cpp
  machine/blitter.cpp
  machine/cv1000.cpp
  machine/ymz770.cpp
  machine/amm_decoder.cpp
  runtime/romset.cpp
  runtime/romset_files.cpp
  runtime/dispatch.cpp
  runtime/timeline.cpp
  # Cave's 68000 boards
  machine/m68k.cpp
  machine/cave68k.cpp
  machine/cave_video.cpp
  machine/ymz280b.cpp
  machine/okim6295.cpp
  machine/eeprom93c46.cpp
  # (Mazinger Z's and Air Gallet's sound side: a Z80, and ymfm's YM2203 and YM2151)
  machine/cave_z80.cpp
  third_party/ymfm_opn.cpp
  third_party/ymfm_ssg.cpp
  third_party/ymfm_adpcm.cpp
  third_party/ymfm_opm.cpp
  runtime/recomp68k.cpp
  # IGS's PGM board (a 68000 too)
  machine/pgm.cpp
  machine/arm7.cpp
  machine/pgm_crypt.cpp
  machine/igs023.cpp
  machine/ics2115.cpp
  machine/z80.cpp
  runtime/recomp_z80.cpp
  runtime/romset_pgm.cpp
)
set_source_files_properties(third_party/ymfm_opn.cpp third_party/ymfm_ssg.cpp third_party/ymfm_adpcm.cpp third_party/ymfm_opm.cpp
                            PROPERTIES COMPILE_OPTIONS "-w")
option(SHADOW_TIMING "the CV1000's timing model run beside its reference (machine/bus_timing_ref.h) at every event, and compared: tools/shadow/" OFF)
if(SHADOW_TIMING)
  add_compile_definitions(SPELUNK_SHADOW_TIMING)
endif()
option(CHECK68K "translated 68000 code run a block at a time against the interpreter (--cpu check)" OFF)
if(CHECK68K)
  add_compile_definitions(SPELUNK_CHECK68K)
endif()
target_include_directories(spelunk_machine PUBLIC machine)
target_link_libraries(spelunk_machine PUBLIC ZLIB::ZLIB)

# The game's game.cfg, built into the programs; and every game's (the collection's launcher)
set(GAME mushisam CACHE STRING "the game whose translations are linked in")
file(READ ${CMAKE_SOURCE_DIR}/games/${GAME}/game.cfg GAME_CFG_TEXT)
# The game's hardware: a CV1000 (an SH-3; game.cfg names none), one of Cave's 68000 boards (hardware = cave68k) or
# IGS's PGM (hardware = pgm: a 68000 too, so SPELUNK_CAVE68K as well)
string(REGEX MATCH "\nhardware = ([^\n]*)" _ "\n${GAME_CFG_TEXT}")
set(HARDWARE "${CMAKE_MATCH_1}")
if(NOT HARDWARE)
  set(HARDWARE cv1000)
endif()
set(M68K_BOARD OFF)
if(HARDWARE STREQUAL cave68k OR HARDWARE STREQUAL pgm)
  set(M68K_BOARD ON)
  add_compile_definitions(SPELUNK_CAVE68K)
endif()
if(HARDWARE STREQUAL pgm)
  add_compile_definitions(SPELUNK_PGM)
endif()
# (a Cave 68000 board that shows more than 320 across: Mazinger Z's 384)
string(REGEX MATCH "\nscreen_width = ([0-9]+)" _ "\n${GAME_CFG_TEXT}")
if(CMAKE_MATCH_1)
  add_compile_definitions(SPELUNK_SCREEN_WIDTH=${CMAKE_MATCH_1})
endif()

# The game's translations (work/<game>/gen, made by tools/translate.sh or tools/translate68k.sh from the player's
# files; never committed). The other hardware's has none. (WORK_DIR: another folder of them; one with none, for the
# collection's launcher and for builds with no translated code: tools/release/lib.sh)
set(WORK_DIR ${CMAKE_SOURCE_DIR}/work CACHE PATH "the folder of each game's translations (<game>/gen)")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${WORK_DIR}/${GAME}/gen/*.cpp)
# (a PGM game's sound driver translated: tools/translatez80.sh, z80_*.cpp beside the 68000's code)
file(GLOB Z80_TABLE CONFIGURE_DEPENDS ${WORK_DIR}/${GAME}/gen/z80_table.cpp)
if(GEN_SOURCES AND NOT Z80_TABLE)
  list(APPEND GEN_SOURCES runtime/no_z80_blocks.cpp)
endif()
if(GEN_SOURCES)
  add_library(spelunk_gen STATIC ${GEN_SOURCES} $<IF:$<BOOL:${M68K_BOARD}>,runtime/no_blocks.cpp,runtime/no_blocks68k.cpp>)
  target_include_directories(spelunk_gen PRIVATE machine runtime)
  target_compile_options(spelunk_gen PRIVATE -O2 -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable)
else()
  add_library(spelunk_gen STATIC runtime/no_blocks.cpp runtime/no_blocks68k.cpp runtime/no_z80_blocks.cpp)
endif()
# (the board's runtime needs the translations' tables (kZ80Code and the rest) and the translations call the board:
# each needs the other, so CMake links them again after each other. GNU ld, unlike Apple's, takes a symbol only from
# a library that comes after the one asking for it.)
target_link_libraries(spelunk_machine PUBLIC spelunk_gen)
target_link_libraries(spelunk_gen PUBLIC spelunk_machine)

file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/game_cfg.inc CONTENT "R\"cfg(${GAME_CFG_TEXT})cfg\"\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/${GAME}/game.cfg)
file(GLOB ALL_GAME_CFGS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/*/game.cfg)
set(ALL_GAMES_INC "")
foreach(cfg ${ALL_GAME_CFGS})
  get_filename_component(dir ${cfg} DIRECTORY)
  get_filename_component(id ${dir} NAME)
  file(READ ${cfg} text)
  string(APPEND ALL_GAMES_INC "{\"${id}\", R\"cfg(${text})cfg\"},\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/all_games.inc CONTENT "${ALL_GAMES_INC}" @ONLY)
add_library(spelunk_game_config STATIC runtime/game_config.cpp)
target_include_directories(spelunk_game_config PRIVATE ${CMAKE_BINARY_DIR}/generated)

if(HARDWARE STREQUAL pgm)
  add_executable(spelunk-headless port/headless_pgm.cpp)
elseif(HARDWARE STREQUAL cave68k)
  add_executable(spelunk-headless port/headless68k.cpp)
else()
  add_executable(spelunk-headless port/headless.cpp)
endif()
target_link_libraries(spelunk-headless spelunk_machine spelunk_gen spelunk_game_config)
if(NOT HARDWARE STREQUAL cave68k)
  # (the PGM and CV1000 runners can run the game's features too: --features, and --boxes on the PGM's)
  target_link_libraries(spelunk-headless spelunk_game)
endif()

# The ARM7 interpreter's self-test (tests/arm7_test.cpp; tools/check_arm.sh runs it): built only when asked for
add_executable(arm7-test EXCLUDE_FROM_ALL tests/arm7_test.cpp machine/arm7.cpp)
# A set checked as the launcher checks it (tests/romset_test.cpp; tools/check_split_sets.sh runs it): built only when
# asked for
add_executable(romset-test EXCLUDE_FROM_ALL tests/romset_test.cpp)
target_link_libraries(romset-test spelunk_machine spelunk_gen spelunk_game_config)

# The game's own sources (its features and what they do)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/${GAME}/src/*.cpp)
# (runtime/shmup.cpp: the training features of a shooting game, from its facts)
add_library(spelunk_game STATIC ${GAME_SOURCES} runtime/features.cpp runtime/shmup.cpp runtime/dodge.cpp
            recomp-common/settings.cpp recomp-common/menu.cpp)
# (Dodge training's patterns, data recorded from the game and kept in WORK_DIR like the translations: its mode is built
# only when they are there, games/ddonpach/src/dodge.cpp)
if(EXISTS ${WORK_DIR}/${GAME}/dodge_patterns.inc)
  target_compile_definitions(spelunk_game PRIVATE SPELUNK_DODGE=1)
  target_include_directories(spelunk_game PRIVATE ${WORK_DIR}/${GAME})
endif()
target_link_libraries(spelunk_game PUBLIC spelunk_machine)
target_link_libraries(spelunk_game_config PUBLIC spelunk_machine)

# The game program and the launcher need SDL3; without it only the board, the headless runner and the checks are built
find_package(SDL3 CONFIG)
if(SDL3_FOUND)

if(HARDWARE STREQUAL pgm)
  set(GAME_GPU port/gpu_pgm.cpp)
elseif(HARDWARE STREQUAL cave68k)
  set(GAME_GPU port/gpu68k.cpp)
else()
  set(GAME_GPU port/gpu.cpp)
endif()
# (online play: port/netplay.cpp, with the launcher's own way of finding this machine's addresses and starting curl;
# a touch screen's controls: port/touch.cpp; a crash's log: port/crash_log.cpp;
# an arcade monitor's modes, synced to the screen: recomp-common/arcade_screen.cpp, with Switchres on Windows,
# downloaded: recomp-common/switchres.cmake)
set(GAME_PORT_SOURCES port/main.cpp port/crash_log.cpp port/gpu_base.cpp port/shading.cpp ${GAME_GPU} port/netplay.cpp port/touch.cpp
    recomp-common/launcher/src/online.cpp recomp-common/launcher/src/run_program.cpp recomp-common/arcade_screen.cpp)
# (Android: a library, lib<id>.so, which the game's activity loads and calls the main of: android/, port/android.cpp,
# tools/release/android.sh)
if(ANDROID)
  add_library(game SHARED ${GAME_PORT_SOURCES} port/android.cpp)
else()
  add_executable(game ${GAME_PORT_SOURCES})
endif()
set_target_properties(game PROPERTIES OUTPUT_NAME ${GAME})
target_link_libraries(game PRIVATE spelunk_game spelunk_machine spelunk_gen spelunk_game_config SDL3::SDL3)
if(ANDROID)
  # (the system's log; and the library's calls of its own functions kept inside it, not through the PLT: as
  # Soulcalibur II's Android build found, that is several per cent of a frame)
  target_link_libraries(game PRIVATE log android)
  target_link_options(game PRIVATE -Wl,-Bsymbolic)
endif()
# (the version both players of an online match must have: VERSION)
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION RECOMP_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)
target_compile_definitions(game PRIVATE RECOMP_VERSION="${RECOMP_VERSION}")
# (the gamepad's buttons that open the in-game menu until the player chooses, on the launcher's Controllers page: Guide,
# or Start and Select together. Not the left stick's click, which a player of a shooting game presses by accident.
# The game's, for a game started on its own, and the launcher's: recomp-common/menu_buttons.h)
set(LAUNCHER_MENU_BUTTON guide)
target_compile_definitions(game PRIVATE RECOMP_MENU_BUTTON_DEFAULT="${LAUNCHER_MENU_BUTTON}")
if(WIN32)
  target_link_libraries(game PRIVATE ws2_32 iphlpapi)
endif()
include(recomp-common/switchres.cmake)
recomp_switchres(game)

# The launcher the recomp ports share (recomp-common/launcher), described by launcher/src/description.cpp. Built for
# the collection, or for one game when asked (-DLAUNCHER=ON): a game's own build folder does not need it, and its
# fetched parts (Dear ImGui, the fonts) are 100 MB a folder
option(COLLECTION "the launcher is the collection's (every game in games/, each program beside it)" OFF)
option(LAUNCHER "build this game's own launcher too" OFF)
if(LAUNCHER OR COLLECTION)
function(game_cfg_value key out)
  string(REGEX MATCH "\n${key} = ([^\n]*)" _ "\n${GAME_CFG_TEXT}")
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
if(COLLECTION)
  set(LAUNCHER_COLLECTION ON)
  set(LAUNCHER_GAME_NAME "Spelunkomp")
  set(LAUNCHER_APP_ID io.github.tmcleroy.spelunkomp)
  set(LAUNCHER_DISCLAIMER "Not affiliated with Cave.")
  set(LAUNCHER_GAME_EXE spelunkomp)
  set(LAUNCHER_HOME_ENV SPELUNKOMP_HOME)
else()
  game_cfg_value(name LAUNCHER_GAME_NAME)
  game_cfg_value(app_id LAUNCHER_APP_ID)
  game_cfg_value(disclaimer LAUNCHER_DISCLAIMER)
  set(LAUNCHER_GAME_EXE ${GAME})
  string(TOUPPER "${GAME}_HOME" LAUNCHER_HOME_ENV)
endif()
if(COLLECTION)
  # (the collection updates itself from the releases: each system's manifest and archive, tools/release/lib.sh; the
  # Android app downloads the release's APK, checked against its SHA256SUMS.txt, and hands it to Android's installer:
  # android/, LauncherActivity.installPackage)
  set(LAUNCHER_UPDATE_REPO tmcleroy/spelunkomp)
endif()
# (VERSION's, or the release scripts' COLLECTION_VERSION, which their manifests say too)
if(COLLECTION AND COLLECTION_VERSION)
  set(LAUNCHER_VERSION ${COLLECTION_VERSION})
else()
  file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION LAUNCHER_VERSION LIMIT_COUNT 1)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)
endif()
set(LAUNCHER_PORT_SOURCES ${CMAKE_SOURCE_DIR}/launcher/src/description.cpp)
include(recomp-common/launcher/launcher.cmake)
target_link_libraries(launcher PRIVATE spelunk_game_config spelunk_machine spelunk_gen)
set_target_properties(launcher PROPERTIES CXX_STANDARD 17)
endif()
endif()
