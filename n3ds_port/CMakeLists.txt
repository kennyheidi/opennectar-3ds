# Standalone 3DS build of Open Nectar's game code. Lives beside (does not modify) the upstream root CMakeLists.
#   cmake -S n3ds_port -B build-3ds -DCMAKE_TOOLCHAIN_FILE=<3DS toolchain>
#   cmake --build build-3ds -j
# Milestone 1a: compile the decompiled game to a static library with the real devkitARM.
# Milestone 1b: link it with upstream's portable Dolphin stubs + n3ds_port/platform into nectar_3ds.3dsx.
cmake_minimum_required(VERSION 3.16)
# Build speed: ccache (when installed) caches every object, so a rebuild after a --clean or a switch between options is mostly cache hits.
find_program(N3DS_CCACHE ccache)
if(N3DS_CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${N3DS_CCACHE}" CACHE STRING "" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${N3DS_CCACHE}" CACHE STRING "" FORCE)
endif()
# r49b: forcing Ninja response files for EVERYTHING (r49) broke the links on MSYS: a native .exe reading @file gets the /opt/devkitpro/...
# paths untranslated ("cannot find -lctru"). Remove it from an existing cache (CMake forces response files whenever it is defined at all).
unset(CMAKE_NINJA_FORCE_RESPONSE_FILE CACHE)
unset(CMAKE_NINJA_FORCE_RESPONSE_FILE)
project(NectarN3DS C CXX ASM)
# Ninja passes all ~430 game objects to arm-none-eabi-gcc-ar on one command line, over the 32 KB Windows limit for a native .exe
# ("Argument list too long"; Make splits it, Ninja does not). platform/n3ds_ar.sh (an MSYS shell script, no such limit) writes the
# object list (relative paths only) to a response file and runs ar with @file.
# R10 (n3ds_port/AGENTS.md, from the T2K contract): math functions do not set errno, so sqrtf() compiles to a bare vsqrt.f32
# instead of vsqrt + compare + a libm call on the NaN path. Nothing in the game or the port reads errno after a math call.
# NOT -ffast-math / -Ofast: those change float results (reassociation, reciprocal approximations) and are forbidden.
add_compile_options(-fno-math-errno)
if(CMAKE_GENERATOR MATCHES "Ninja")
  foreach(_lang C CXX ASM)
    set(CMAKE_${_lang}_ARCHIVE_CREATE "/bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_ar.sh <CMAKE_AR> <TARGET> <OBJECTS>")
    set(CMAKE_${_lang}_ARCHIVE_APPEND "/bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_ar.sh <CMAKE_AR> <TARGET> <OBJECTS>")
  endforeach()
endif()
if(N3DS_CCACHE)
  message(STATUS "ccache: ${N3DS_CCACHE}")
endif()

# Build speed: the patched copies of game / audio sources are generated at configure time, and build_3ds.sh configures on every run.
# file(WRITE) gave them a new timestamp each time, so every one of them (and everything that depends on them) recompiled on EVERY build
# even when nothing had changed. Written only when the text differs now.
function(n3ds_write_if_changed PATH TEXT)
  if(EXISTS "${PATH}")
    file(READ "${PATH}" _old)
    if(_old STREQUAL TEXT)
      return()
    endif()
  endif()
  file(WRITE "${PATH}" "${TEXT}")
endfunction()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

get_filename_component(NECTAR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PC_PORT_DIR "${NECTAR_ROOT}/pc_port")
option(N3DS_CHECKER_SHIM "Sandbox-only: add a fake <mutex> (bare newlib). Leave OFF with devkitARM." OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

file(GLOB SYSCOMMON_SOURCES "${NECTAR_ROOT}/src/sysCommon/*.cpp")
set(SYSDOLPHIN_SOURCES
  ${NECTAR_ROOT}/src/sysDolphin/controllerMgr.cpp ${NECTAR_ROOT}/src/sysDolphin/dgxGraphics.cpp
  ${NECTAR_ROOT}/src/sysDolphin/gameApp.cpp       ${NECTAR_ROOT}/src/sysDolphin/sysNew.cpp
  ${NECTAR_ROOT}/src/sysDolphin/system.cpp        ${NECTAR_ROOT}/src/sysDolphin/texture.cpp)
# ---- Recovery switch --------------------------------------------------------------------------------------------------------
# src/sysDolphin/{dgxGraphics,system,texture}.cpp were overwritten by an AI-written "patch" (stub files of 93-119 lines instead
# of the real 569 / 1672 / 2387+). recovered/*.o are the LAST-KNOWN-GOOD COMPILED copies of the real files, taken from the
# libpikmin_legacy_3ds.a of the last successful build (they were built with N3DS_TRACE_CALLS=ON, so they carry
# __cyg_profile_func_* hooks; platform/n3ds_trace.cpp provides them either way).
#   cmake -S n3ds_port -B build-3ds -DN3DS_USE_RECOVERED_SYSDOLPHIN=ON     # builds again WITHOUT those three .cpp files
# Turn it OFF again once the real sources are restored (tools/verify_sysdolphin.py checks them).
option(N3DS_USE_RECOVERED_SYSDOLPHIN "Link recovered/{dgxGraphics,system,texture}.o instead of compiling those three .cpp files" OFF)
set(SYSDOLPHIN_PREBUILT "")
if(N3DS_USE_RECOVERED_SYSDOLPHIN)
  foreach(_f dgxGraphics system texture)
    set(_o "${CMAKE_CURRENT_SOURCE_DIR}/recovered/${_f}.o")
    if(NOT EXISTS "${_o}")
      message(FATAL_ERROR "N3DS_USE_RECOVERED_SYSDOLPHIN=ON but ${_o} is missing (see tools/extract_sysdolphin_objects.sh)")
    endif()
    list(REMOVE_ITEM SYSDOLPHIN_SOURCES "${NECTAR_ROOT}/src/sysDolphin/${_f}.cpp")
    list(APPEND SYSDOLPHIN_PREBUILT "${_o}")
  endforeach()
  set_source_files_properties(${SYSDOLPHIN_PREBUILT} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  message(STATUS "N3DS_USE_RECOVERED_SYSDOLPHIN=ON: linking prebuilt dgxGraphics/system/texture objects, NOT compiling those sources")
endif()
foreach(d Kando Nakata Nishimura Colin Ogawa Yamashita)
  file(GLOB _s "${NECTAR_ROOT}/src/plugPiki${d}/*.cpp")
  list(APPEND PLUGPIKI_SOURCES ${_s})
endforeach()

# GAME-SPEED CATCH-UP (nectar.ini max_dt_ms, README NEWEST-76): System::updateSysClock() caps the game-time step at 1/30 s per frame, so below 30 FPS the whole game
# (and the day clock) runs in slow motion. The two cap lines are patched in a build-time COPY of system.cpp to call n3ds_max_dt() (n3ds_config.cpp: 1/30 unless
# max_dt_ms is set). Skipped (WARNING only) when the lines are not found or the recovered objects are used.
if(NOT N3DS_USE_RECOVERED_SYSDOLPHIN)
  set(_sysf "${NECTAR_ROOT}/src/sysDolphin/system.cpp")
  file(READ "${_sysf}" _sys)
  string(FIND "${_sys}" "if (mDeltaTime > f32(1.0f / 30.0f)) {" _dt1)
  string(FIND "${_sys}" "mDeltaTime = f32(1.0f / 30.0f);" _dt2)
  if(NOT _dt1 EQUAL -1 AND NOT _dt2 EQUAL -1)
    string(REPLACE "f32(1.0f / 30.0f)" "n3ds_max_dt()" _sys "${_sys}")
    set(_sys "extern \"C\" float n3ds_max_dt(void);\n${_sys}")
    set(N3DS_SYSTEM_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/n3ds_system_patched.cpp")
    n3ds_write_if_changed("${N3DS_SYSTEM_PATCHED}" "${_sys}")
    list(REMOVE_ITEM SYSDOLPHIN_SOURCES "${_sysf}")
    list(APPEND SYSDOLPHIN_SOURCES "${N3DS_SYSTEM_PATCHED}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sysf}")
    message(STATUS "game-speed catch-up: System::updateSysClock patched (nectar.ini max_dt_ms)")
  else()
    message(WARNING "system.cpp: the 1/30 s delta-time cap lines were not found: nectar.ini max_dt_ms will have no effect")
  endif()
endif()
# 3DS-only edits of titles.cpp / gameflow.cpp, applied to build-folder copies (platform/n3ds_src_patches.cmake); edits PLUGPIKI_SOURCES.
include("${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_src_patches.cmake")
add_library(pikmin_legacy_3ds STATIC ${SYSCOMMON_SOURCES} ${SYSDOLPHIN_SOURCES} ${SYSDOLPHIN_PREBUILT} ${PLUGPIKI_SOURCES})

target_compile_definitions(pikmin_legacy_3ds PUBLIC
  PIKI_PC_PORT=1 VERSION_GPIE01_01 DTK_CONFIG_NONMATCHING=1
  PIKI_USE_GLES=0 PIKI_PC_TOUCH=0 PIKI_USE_DGX=1 PIKI_USE_JAUDIO=0 PIKI_N3DS=1)

target_include_directories(pikmin_legacy_3ds PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/shim           # must come first: GL-free pc_gfx.h, SDL/mman stand-ins
  ${NECTAR_ROOT}/include ${NECTAR_ROOT}/include/Dolphin ${NECTAR_ROOT}/include/Dolphin/OS
  ${NECTAR_ROOT}/include/Dolphin/GX ${PC_PORT_DIR} ${PC_PORT_DIR}/gl
  ${NECTAR_ROOT}/third_party/stb ${NECTAR_ROOT}/third_party/bcdec ${NECTAR_ROOT}/third_party/xxhash)
if(N3DS_CHECKER_SHIM)
  target_include_directories(pikmin_legacy_3ds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/checker_shim)
endif()

# Same permissive flags as upstream's LEGACY_COMPILE_OPTIONS (decomp needs them).
set(NECTAR_COMPILE_OPTIONS
  # SHELL: keeps each "-include <file>" pair intact; CMake would otherwise de-duplicate the repeated -include.
  "SHELL:$<$<COMPILE_LANGUAGE:CXX>:-include ${PC_PORT_DIR}/pc_types.h>"
  "SHELL:$<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/pc_compat.h>"
  -fsigned-char $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
  -Wno-narrowing -Wno-multichar -Wno-write-strings -Wno-sign-compare -Wno-unused-variable
  -Wno-unused-but-set-variable -Wno-parentheses -Wno-delete-non-virtual-dtor -Wno-reorder
  -Wno-switch -Wno-format -Wno-attributes -Wno-comment -Wno-register)
target_compile_options(pikmin_legacy_3ds PRIVATE ${NECTAR_COMPILE_OPTIONS})
# Diagnostic: record enter/exit of every non-header game function (see platform/n3ds_trace.cpp). Slower; use to find hangs.
option(N3DS_TRACE_CALLS "Instrument game code so a stall dump lists the last function calls" OFF)
if(N3DS_TRACE_CALLS)
  target_compile_options(pikmin_legacy_3ds PRIVATE -finstrument-functions -finstrument-functions-exclude-file-list=/include/,/third_party/)
  message(STATUS "N3DS_TRACE_CALLS=ON: game code instrumented (slower)")
endif()

# ---- Milestone 1b: port layer + executable -------------------------------------------------------
# Upstream's os_stubs.cpp reserves a 256 MiB static array (sArenaMemory) for the simulated GameCube arena.
# That is far bigger than a 3DS has (and 3dsxtool rejects executables >256 MiB), so build a patched COPY
# (upstream stays untouched) that allocates a smaller arena from the heap at startup instead.
# ---- Memory mode (only matters for the .cia: a .3dsx gets whatever Homebrew Launcher gives it) -----------------------------
#   -DN3DS_SYSMODE_EXT=124MB (default)  New 3DS extended mode: app region ~124 MB (what the 1k log showed: heap 87.6 MB + linear 32 MB)
#   -DN3DS_SYSMODE_EXT=178MB            app region ~178 MB. UNTESTED on hardware; I believe retail firmware only honours it under custom
#                                       firmware. If the CIA will not launch, go back to 124MB.
# The malloc heap lives in a fixed 96 MB virtual window, so every byte beyond ~96 MB must go to the LINEAR heap (textures, GPU
# rings). 178MB therefore sets N3DS_LINEAR_HEAP_MB=80 (heap = 182272 - ~6600 code/stack - 81920 = ~93.7 MB < 98304) and raises the
# texture budget; override either one explicitly if you need to.
set(N3DS_SYSMODE_EXT "124MB" CACHE STRING "CIA memory mode: 124MB or 178MB")
set_property(CACHE N3DS_SYSMODE_EXT PROPERTY STRINGS 124MB 178MB)
if(NOT N3DS_SYSMODE_EXT MATCHES "^(124|178)MB$")
  message(FATAL_ERROR "N3DS_SYSMODE_EXT must be 124MB or 178MB (got '${N3DS_SYSMODE_EXT}')")
endif()
set(N3DS_LINEAR_HEAP_MB 0 CACHE STRING "Override libctru's linear heap size in MiB (0 = default; forced to 80 for 178MB unless set)")
set(N3DS_TEX_BUDGET_KB 0 CACHE STRING "Texture pool budget in KB (0 = 10240 for 124MB, 40960 for 178MB)")
if(N3DS_SYSMODE_EXT STREQUAL "178MB")
  if(N3DS_LINEAR_HEAP_MB EQUAL 0)
    set(N3DS_LINEAR_HEAP_MB 80)
  endif()
  if(N3DS_TEX_BUDGET_KB EQUAL 0)
    set(N3DS_TEX_BUDGET_KB 40960)
  endif()
endif()
if(N3DS_TEX_BUDGET_KB EQUAL 0)
  set(N3DS_TEX_BUDGET_KB 10240)
endif()
# Guard against the startup crash (Luma crash_dump, process "Nectar", svcBreak(PANIC) before main): libctru sets
#   heap = (app region - code) - linear, and the heap must fit the fixed 96 MB (98304 KB) virtual window or svcControlMemory fails.
# A STALE CMake cache value (e.g. -DN3DS_LINEAR_HEAP_MB=24 from a 124MB build, then switching to 178MB) triggers exactly that,
# because the 178MB branch above only fills in 80 when the value is 0.
if(N3DS_SYSMODE_EXT STREQUAL "178MB")
  set(_n3ds_region_kb 182272)
else()
  set(_n3ds_region_kb 126976)
endif()
if(N3DS_LINEAR_HEAP_MB GREATER 0)
  math(EXPR _n3ds_heap_kb "${_n3ds_region_kb} - 6600 - ${N3DS_LINEAR_HEAP_MB} * 1024")
  if(_n3ds_heap_kb GREATER 98304)
    math(EXPR _n3ds_min_lin "(${_n3ds_region_kb} - 6600 - 98304 + 1023) / 1024 + 1")
    message(FATAL_ERROR
      "N3DS_LINEAR_HEAP_MB=${N3DS_LINEAR_HEAP_MB} with ${N3DS_SYSMODE_EXT} leaves a ${_n3ds_heap_kb} KB malloc heap, bigger than the "
      "98304 KB window: the game would crash before main (Luma crash_dump, svcBreak). Use N3DS_LINEAR_HEAP_MB >= ${_n3ds_min_lin} "
      "(178MB: 80 is the default), or 0 for libctru's default. If you switched modes in an existing build dir, the old value is "
      "cached: re-run cmake with -DN3DS_LINEAR_HEAP_MB=0 (or delete CMakeCache.txt).")
  endif()
endif()
message(STATUS "CIA memory mode ${N3DS_SYSMODE_EXT}, linear heap override ${N3DS_LINEAR_HEAP_MB} MB (0 = libctru default), texture budget ${N3DS_TEX_BUDGET_KB} KB")
# ---- TEST build identity ---------------------------------------------------------------------------------------------------------
# ON (default in this branch): the .cia gets its own title ID (0xEC402), product code and name, so it installs BESIDE a normal Nectar (0xEC400)
# instead of replacing it. Both share sdmc:/3ds/nectar (assets, nectar.pak, save, nectar.ini, log.txt). OFF = the normal identity (0xEC400).
option(N3DS_TEST_BUILD "Install beside the normal Nectar: different title ID, product code and name" ON)
if(N3DS_TEST_BUILD)
  set(N3DS_TITLE_ID "0xEC402")
  set(N3DS_PRODUCT_CODE "CTR-P-NTST")
  set(N3DS_EXHEADER_NAME "NectTest")      # the exheader name field is only 8 bytes
else()
  set(N3DS_TITLE_ID "0xEC400")
  set(N3DS_PRODUCT_CODE "CTR-P-NECT")
  set(N3DS_EXHEADER_NAME "Nectar")
endif()
message(STATUS "CIA identity: title ID ${N3DS_TITLE_ID}, product code ${N3DS_PRODUCT_CODE}, name ${N3DS_EXHEADER_NAME} (N3DS_TEST_BUILD=${N3DS_TEST_BUILD})")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/platform/nectar.rsf.in ${CMAKE_CURRENT_BINARY_DIR}/nectar.rsf @ONLY)

set(N3DS_CMDBUF_KB 2048 CACHE STRING "GPU command buffer in KB (linear heap). 256 KB, then 1 MB, overflowed in a scene with thousands of draws: libctru svcBreaks (crash dumps 16 and 19)")
set(N3DS_RING_VERTS 131072 CACHE STRING "Vertices per GPU vertex ring (2 rings, 28 B/vertex, from the linear heap)")
set(N3DS_ARENA_MB 24 CACHE STRING "Size in MiB of the simulated GameCube arena (falls back to smaller if malloc fails). 8 is too small: ovl heap = arena - ~2.6 MB, and the game wants an 8 MB block from it")
set(N3DS_OS_STUBS_SRC "${PC_PORT_DIR}/dolphin_stubs/os_stubs.cpp")
set(N3DS_OS_STUBS_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/n3ds_os_stubs_patched.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${N3DS_OS_STUBS_SRC}")
file(READ "${N3DS_OS_STUBS_SRC}" _os)
set(_arena_old "static u8 sArenaMemory[256 * 1024 * 1024];")
set(_arena_use "sArenaMemory + sizeof(sArenaMemory)")
string(FIND "${_os}" "${_arena_old}" _p1)
string(FIND "${_os}" "${_arena_use}" _p2)
if(_p1 EQUAL -1 OR _p2 EQUAL -1)
  message(FATAL_ERROR "os_stubs.cpp no longer matches the sArenaMemory patch pattern; update the patch in n3ds_port/CMakeLists.txt")
endif()
set(_arena_new "#include <stdlib.h>
#include <malloc.h>
#ifndef N3DS_ARENA_MB
#define N3DS_ARENA_MB 24
#endif
static size_t sArenaMemorySize = 0;
static u8* n3dsArenaAlloc()
{
    size_t sz = (size_t)N3DS_ARENA_MB << 20;
    while (sz >= ((size_t)8 << 20)) {
        void* p = memalign(32, sz);
        if (p) { sArenaMemorySize = sz; return (u8*)p; }
        sz -= ((size_t)4 << 20);
    }
    return NULL;
}
static u8* sArenaMemory = n3dsArenaAlloc();")
string(REPLACE "${_arena_old}" "${_arena_new}" _os "${_os}")
string(REPLACE "${_arena_use}" "sArenaMemory + sArenaMemorySize" _os "${_os}")
# OSResumeThread(): std::thread creation can fail on the 3DS (libctru threadCreate -> memalign of the stack when the heap is full). Upstream
# lets the std::system_error escape -> std::terminate (crash seen after ~14k frames). Catch it, log it and run the thread body inline instead.
# Also switch the new thread's VFP to RunFast (flush-to-zero + default NaN): in IEEE mode a denormal/NaN can raise a VFP exception (Luma type 3)
# that the 3DS kernel cannot handle.
set(_rt_old "it->second.thrd = new std::thread([](OSThreadStartFunction f, void* p) {
            if (f) f(p);
        }, it->second.func, it->second.param);")
string(FIND "${_os}" "${_rt_old}" _prt)
if(_prt EQUAL -1)
  message(FATAL_ERROR "os_stubs.cpp no longer matches the OSResumeThread patch pattern; update the patch in n3ds_port/CMakeLists.txt")
endif()
set(_rt_new "try {
            it->second.thrd = new std::thread([](OSThreadStartFunction f, void* p) {
                n3ds_vfp_runfast();
                if (f) f(p);
            }, it->second.func, it->second.param);
        } catch (const std::exception& e) {
            n3ds_log(\"[thread] std::thread failed (%s); running the thread body inline\", e.what());
            it->second.thrd = new std::thread();
            OSThreadStartFunction f = it->second.func; void* p = it->second.param;
            sGlobalMtx.unlock();
            if (f) f(p);
            sGlobalMtx.lock();
        }")
string(REPLACE "${_rt_old}" "${_rt_new}" _os "${_os}")
string(REPLACE "s32  OSResumeThread(OSThread* thread) {" "extern \"C\" void n3ds_log(const char* fmt, ...);\nextern \"C\" void n3ds_vfp_runfast(void);\ns32  OSResumeThread(OSThread* thread) {" _os "${_os}")
n3ds_write_if_changed("${N3DS_OS_STUBS_PATCHED}" "${_os}")

# ---- DVD layer: upstream dvd_stubs.cpp + timing + no redundant fseek (build-time copy; upstream untouched) ---------------------------------
# DVDOpen goes through n3ds_dvd_fopen() (platform/n3ds_pak.cpp): files packed in nectar.pak (tools/make_pak.py) skip the ~35 ms SD-card fopen.
# Level loads do ~60 ms of work per tiny file read. This copy logs "[dvdprof]" (time in fopen vs seek+fread) and skips the fseek when the read
# continues exactly where the previous one ended (newlib's fseek throws away the stdio buffer and re-reads the SD card). If upstream's file changes
# and a pattern no longer matches, the unpatched file is used (warning only).
set(N3DS_DVD_SRC "${PC_PORT_DIR}/dolphin_stubs/dvd_stubs.cpp")
set(N3DS_DVD_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/n3ds_dvd_stubs_patched.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${N3DS_DVD_SRC}")
file(READ "${N3DS_DVD_SRC}" _dv)
set(_d_ok TRUE)
foreach(_pat
    "static std::vector<std::string> sFastOpenPaths;"
    "FILE* f = fopen(path.c_str(), \"rb\");"
    "fseek(f, offset, SEEK_SET);\n    s32 read_bytes = fread(addr, 1, length, f);"
    "fclose(it->second);")
  string(FIND "${_dv}" "${_pat}" _dp)
  if(_dp EQUAL -1)
    set(_d_ok FALSE)
  endif()
endforeach()
if(_d_ok)
  string(REPLACE "static std::vector<std::string> sFastOpenPaths;"
    "static std::vector<std::string> sFastOpenPaths;\nextern \"C\" unsigned long long n3ds_ticks(void);\nextern \"C\" void n3ds_dvdprof(int kind, unsigned long long t0, unsigned bytes);\nextern \"C\" FILE* n3ds_dvd_fopen(const char* path);\nstatic FILE* sLastF = nullptr; static long sLastPos = -1;" _dv "${_dv}")
  string(REPLACE "FILE* f = fopen(path.c_str(), \"rb\");"
    "unsigned long long _t0 = n3ds_ticks();\n    FILE* f = n3ds_dvd_fopen(path.c_str());\n    n3ds_dvdprof(0, _t0, 0);\n    sLastF = nullptr;" _dv "${_dv}")
  string(REPLACE "fseek(f, offset, SEEK_SET);\n    s32 read_bytes = fread(addr, 1, length, f);"
    "unsigned long long _t1 = n3ds_ticks();\n    if (!(f == sLastF && sLastPos == (long)offset)) fseek(f, offset, SEEK_SET);\n    s32 read_bytes = fread(addr, 1, length, f);\n    sLastF = f; sLastPos = (long)offset + read_bytes;\n    n3ds_dvdprof(1, _t1, (unsigned)read_bytes);" _dv "${_dv}")
  string(REPLACE "fclose(it->second);" "if (it->second == sLastF) sLastF = nullptr;\n        fclose(it->second);" _dv "${_dv}")
  # packed files are pool handles from n3ds_pak.cpp, not real FILEs: every stdio call on a DVD file goes through its wrappers (real FILEs pass through)
  string(REPLACE "fseek(" "n3ds_fseek(" _dv "${_dv}")
  string(REPLACE "ftell(" "n3ds_ftell(" _dv "${_dv}")
  string(REPLACE "fread(" "n3ds_fread(" _dv "${_dv}")
  string(REPLACE "fclose(" "n3ds_fclose(" _dv "${_dv}")
  string(REPLACE "extern \"C\" FILE* n3ds_dvd_fopen(const char* path);"
    "extern \"C\" FILE* n3ds_dvd_fopen(const char* path);\nextern \"C\" int n3ds_fseek(FILE*, long, int);\nextern \"C\" long n3ds_ftell(FILE*);\nextern \"C\" size_t n3ds_fread(void*, size_t, size_t, FILE*);\nextern \"C\" int n3ds_fclose(FILE*);" _dv "${_dv}")
  n3ds_write_if_changed("${N3DS_DVD_PATCHED}" "${_dv}")
else()
  message(WARNING "dvd_stubs.cpp no longer matches the 3DS DVD patch patterns; using it unpatched (no [dvdprof], no seek skipping)")
  set(N3DS_DVD_PATCHED "${N3DS_DVD_SRC}")
endif()

# ---- Game audio: upstream's native mixer (pc_port/audio) + JAudio API layer (dolphin_stubs/audio_stubs.cpp) -------------------
# SDL is replaced by platform/n3ds_sdl_audio.cpp (NDSP). Two sources get tiny build-time patches (copies in the build dir):
#   pc_audio.cpp    : disambiguate the include (a stale dolphin_stubs/pc_audio.h exists) and gate pc_audio_play_stx() on nectar.ini audio_stx
#   audio_stubs.cpp : Jac_Gsync() also feeds the watchdog heartbeat (n3ds_diag_loop), which the old no-op stub used to do
# SOUND-EFFECT PORT QUEUE (README NEWEST-23/75): the game hands a sound effect to the SE sequencer through a ONE-SLOT port, so a second request that
# arrives before the sequence has read the first replaces it and the first effect is never heard - at 9 FPS (one sequencer step per frame) that is a large
# share of a busy scene's effects. The fix (queue in pc_audio.cpp + PCJamPlayer::childPortPending) used to be a patch the user had to apply by hand. If the
# repo's pc_jam.h does not have childPortPending, the tested copies in n3ds_port/audio_dropin/ are used for this build instead (no files in the repo are changed).
set(N3DS_SE_QUEUE_MODE 0)
set(N3DS_PCAUDIO_SRC "${PC_PORT_DIR}/audio/pc_audio.cpp")
set(N3DS_PCJAM_SRC "${PC_PORT_DIR}/audio/pc_jam.cpp")
file(READ "${PC_PORT_DIR}/audio/pc_jam.h" _jamh)
string(FIND "${_jamh}" "childPortPending" _jamq)
if(_jamq EQUAL -1 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_audio.cpp")
  set(N3DS_SE_QUEUE_MODE 1)
  set(N3DS_PCAUDIO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_audio.cpp")
  set(N3DS_PCJAM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_jam.cpp")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_jam.h" "${CMAKE_CURRENT_BINARY_DIR}/n3ds_se_inc/audio/pc_jam.h" COPYONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_audio.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_jam.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/audio_dropin/pc_jam.h")
  message(STATUS "sound-effect port queue: your pc_port/audio does not have it -> using n3ds_port/audio_dropin/ for this build")
else()
  message(STATUS "sound-effect port queue: already present in pc_port/audio")
endif()
set(N3DS_AUDSTUB_SRC "${PC_PORT_DIR}/dolphin_stubs/audio_stubs.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${N3DS_PCAUDIO_SRC}" "${N3DS_AUDSTUB_SRC}")
file(READ "${N3DS_PCAUDIO_SRC}" _pa)
set(_a1 "#include \"pc_audio.h\"")
set(_a2 "bool pc_audio_play_stx(const char* path) {")
string(FIND "${_pa}" "${_a1}" _pa1)
string(FIND "${_pa}" "${_a2}" _pa2)
if(_pa1 EQUAL -1 OR _pa2 EQUAL -1)
  message(FATAL_ERROR "pc_audio.cpp no longer matches the 3DS audio patch pattern; update the patch in n3ds_port/CMakeLists.txt")
endif()
string(REPLACE "${_a1}" "#include \"audio/pc_audio.h\"\nextern \"C\" int n3ds_audio_stx_enabled(void);" _pa "${_pa}")
# pc_audio_play_stx(): the 3DS version decodes + resamples on a background thread straight into the final buffer (platform/n3ds_stx.cpp)
# instead of decoding everything (~10 MB) and resampling it (~7 MB) on the game thread. The glue text lives in platform/n3ds_stx_glue.inc.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_stx_glue.inc" _stx_glue)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_stx_glue.inc")
string(REPLACE "${_a2}" "${_stx_glue}" _pa "${_pa}")
set(_a3 "void pc_audio_stop_stream(void) {")
string(FIND "${_pa}" "${_a3}" _pa3)
if(_pa3 EQUAL -1)
  message(FATAL_ERROR "pc_audio.cpp no longer has pc_audio_stop_stream(); update the 3DS audio patch in n3ds_port/CMakeLists.txt")
endif()
string(REPLACE "${_a3}" "void pc_audio_stop_stream(void) {\n    n3ds_stx_cancel();" _pa "${_pa}")
# nectar.ini audio_rate (32000 default, 22050, 16000, 11025): the mixer runs per OUTPUT sample, so its cost scales with the rate. pc_audio.cpp
# already derives every step/filter/envelope from sAudioSpec.freq; the two 32 kHz assumptions are patched here: the FX delay line (keeps its
# 128 ms length) and the AI-DMA queue (32 kHz PCM, now stepped at 32000/rate). Soft patch: a pattern mismatch only warns and keeps 32 kHz.
set(_r_ok TRUE)
foreach(_pat
    "static std::array<float, kFxDelayFrames> sFxDelayRight {};"
    "sFxDelayCursor = (sFxDelayCursor + 1) % kFxDelayFrames;"
    "            mixedLeft += static_cast<int>(sDMAQueue[sDMAReadCursor++] * gain);\n            mixedRight += static_cast<int>(sDMAQueue[sDMAReadCursor++] * gain);"
    "    desired.freq = 32000;")
  string(FIND "${_pa}" "${_pat}" _rp)
  if(_rp EQUAL -1)
    set(_r_ok FALSE)
  endif()
endforeach()
if(_r_ok)
  string(REPLACE "static std::array<float, kFxDelayFrames> sFxDelayRight {};"
    "static std::array<float, kFxDelayFrames> sFxDelayRight {};\nextern \"C\" int n3ds_audio_rate(void);\nstatic size_t sN3dsFxLen = kFxDelayFrames;\nstatic u32 sN3dsDmaStep = 65536u, sN3dsDmaPhase = 0;" _pa "${_pa}")
  string(REPLACE "sFxDelayCursor = (sFxDelayCursor + 1) % kFxDelayFrames;"
    "if (++sFxDelayCursor >= sN3dsFxLen) sFxDelayCursor = 0;" _pa "${_pa}")
  string(REPLACE "            mixedLeft += static_cast<int>(sDMAQueue[sDMAReadCursor++] * gain);\n            mixedRight += static_cast<int>(sDMAQueue[sDMAReadCursor++] * gain);"
    "            mixedLeft += static_cast<int>(sDMAQueue[sDMAReadCursor] * gain);\n            mixedRight += static_cast<int>(sDMAQueue[sDMAReadCursor + 1] * gain);\n            sN3dsDmaPhase += sN3dsDmaStep;\n            while (sN3dsDmaPhase >= 65536u) { sN3dsDmaPhase -= 65536u; sDMAReadCursor += 2; }" _pa "${_pa}")
  string(REPLACE "    desired.freq = 32000;"
    "    desired.freq = n3ds_audio_rate();\n    sN3dsFxLen = static_cast<size_t>(kFxDelayFrames) * static_cast<size_t>(desired.freq) / 32000u;\n    if (sN3dsFxLen < 1 || sN3dsFxLen > kFxDelayFrames) sN3dsFxLen = kFxDelayFrames;\n    sN3dsDmaStep = static_cast<u32>(32000ull * 65536ull / static_cast<unsigned>(desired.freq));\n    sN3dsDmaPhase = 0;" _pa "${_pa}")
else()
  message(WARNING "pc_audio.cpp no longer matches the audio_rate patch patterns; the mixer stays at 32 kHz")
endif()
# ---- always-on sound-drop counters (no nectar.ini key needed): the engine counts the sounds it cannot play, but only PRINTS them with audio_stats = 1 ----
# n3ds_audio_diag() is appended to the build-time copy and read by the "[sfx] engine drops" log line (platform/n3ds_sfx_diag.inc). Besides the engine's own
# counters it counts the two SILENT failures that were not counted at all: a wave that fails to decode (return -1) and instruments of the "oscillator"
# kind (program >= 0xF0), which the engine drops on purpose unless PIKMIN_OSC=1. Soft patch: a pattern mismatch only warns (the line then says "n/a").
set(_d1 "if (!pc_decode_wave(*info, *fresh)) return -1;")
set(_d2 "if (!oscillatorsEnabled) return -1;")
set(_d3 "static std::atomic<int> sMissWaveElsewhere{0};")
string(FIND "${_pa}" "${_d1}" _pd1)
string(FIND "${_pa}" "${_d2}" _pd2)
string(FIND "${_pa}" "${_d3}" _pd3)
string(FIND "${_pa}" "static std::mutex sWaveMissMutex;" _pd4)
string(FIND "${_pa}" "static std::unordered_map<u64, std::shared_ptr<const std::vector<s16>>> sDecodedWaveCache;" _pd5)
set(_aa1 "voice.baseStep = static_cast<double>(info->sampleRate) / sAudioSpec.freq * pitchScale;")
set(_aa2 "voice.loopSample = std::min<size_t>(info->loopSample, voice.samples->size());")
set(_aa3 "? std::min<size_t>(info->loopEndSample, voice.samples->size())")
set(_aa5 "sDecodedWaveCache.clear();")
set(_aa_ok TRUE)
foreach(_aap _aa1 _aa2 _aa3 _aa5)
  string(FIND "${_pa}" "${${_aap}}" _aaf)
  if(_aaf EQUAL -1)
    set(_aa_ok FALSE)
  endif()
endforeach()
if(NOT _pd1 EQUAL -1 AND NOT _pd2 EQUAL -1 AND NOT _pd3 EQUAL -1 AND NOT _pd4 EQUAL -1 AND NOT _pd5 EQUAL -1)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_aa_resample.inc" _aa_inc)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_aa_resample.inc")
  string(REPLACE "${_d3}" "${_d3}\nstatic int gN3dsDecodeFail = 0, gN3dsDecodeLastSamples = 0, gN3dsSilentOsc = 0, gN3dsSilentOscProg = 0;\n${_aa_inc}" _pa "${_pa}")
  set(_aa_call "")
  if(_aa_ok)
    set(_aa_call " n3dsAntiAlias(*fresh, info->sampleRate, sAudioSpec.freq, cacheKey);")
  else()
    message(WARNING "pc_audio.cpp does not match the anti-alias patterns: low audio_rate values stay hissy (waves are not low-passed before they are played at the lower rate)")
  endif()
  string(REPLACE "${_d1}" "if (!pc_decode_wave(*info, *fresh)) { ++gN3dsDecodeFail; gN3dsDecodeLastSamples = (int)info->sampleCount; return -1; }${_aa_call}" _pa "${_pa}")
  if(_aa_ok)
    string(REPLACE "${_aa1}" "double n3Ratio = 1.0; { const auto n3r = gN3dsWaveRatio.find(cacheKey); if (n3r != gN3dsWaveRatio.end()) n3Ratio = n3r->second; }\n    voice.baseStep = static_cast<double>(info->sampleRate) * n3Ratio / sAudioSpec.freq * pitchScale;" _pa "${_pa}")
    string(REPLACE "${_aa2}" "voice.loopSample = std::min<size_t>(static_cast<size_t>(info->loopSample * n3Ratio), voice.samples->size());" _pa "${_pa}")
    string(REPLACE "${_aa3}" "? std::min<size_t>(static_cast<size_t>(info->loopEndSample * n3Ratio), voice.samples->size())" _pa "${_pa}")
    string(REPLACE "${_aa5}" "sDecodedWaveCache.clear(); gN3dsWaveRatio.clear();" _pa "${_pa}")
  endif()
  string(REPLACE "${_d2}" "if (!oscillatorsEnabled) { ++gN3dsSilentOsc; gN3dsSilentOscProg = (int)program; return -1; }" _pa "${_pa}")
  string(APPEND _pa "\n// ---- 3DS sound diagnostics (n3ds_port/CMakeLists.txt): read by n3ds_sfx_report() ----\n")
  string(APPEND _pa "extern \"C\" int n3ds_audio_diag(int* out, char* misses, int missesLen) {\n")
  string(APPEND _pa "    out[0] = sMissInstrument.load(); out[1] = sMissWaveSystem.load(); out[2] = sMissWave.load(); out[3] = sMissWaveElsewhere.load();\n")
  string(APPEND _pa "    out[4] = gN3dsDecodeFail; out[5] = gN3dsDecodeLastSamples; out[6] = gN3dsSilentOsc; out[7] = gN3dsSilentOscProg;\n")
  string(APPEND _pa "    size_t bytes = 0; for (const auto& kv : sDecodedWaveCache) if (kv.second) bytes += kv.second->size() * sizeof(s16);\n")
  string(APPEND _pa "    out[8] = (int)sDecodedWaveCache.size(); out[9] = (int)(bytes / 1024); out[10] = (int)gN3dsWaveRatio.size();\n")
  string(APPEND _pa "    if (misses && missesLen > 0) { misses[0] = 0; int used = 0; std::lock_guard<std::mutex> lock(sWaveMissMutex);\n")
  string(APPEND _pa "        for (const auto& miss : sWaveMisses) { if (!miss.count) break; if (used > missesLen - 80) break;\n")
  string(APPEND _pa "            used += snprintf(misses + used, (size_t)(missesLen - used), \" [wsys %d wave %d scene %d prog %d x%d%s]\", miss.system, miss.id, miss.scene, miss.program, miss.count, miss.elsewhere ? \" in another scene\" : (miss.otherSystem >= 0 ? \" in another wsys\" : \"\")); } }\n")
  string(APPEND _pa "    return 1;\n}\n")
else()
  message(WARNING "pc_audio.cpp does not match the sound-drop counter patterns: the [sfx] engine-drops line will say n/a")
  string(APPEND _pa "\nextern \"C\" int n3ds_audio_diag(int* out, char* misses, int missesLen) { (void)out; (void)misses; (void)missesLen; return 0; }\n")
endif()
# ---- faster mixer (platform/n3ds_fastmix.inc): the new audio_callback() is inserted before the original, which stays as audio_callback_orig() ----
# Needs the audio_rate patch above (it uses sN3dsFxLen / sN3dsDmaStep / sN3dsDmaPhase). nectar.ini audio_fastmix = 0 or audio_stats = 1 runs the original.
set(_fm1 "static void audio_callback(void*, Uint8* output, int byteCount) {")
string(FIND "${_pa}" "${_fm1}" _fmf)
if(_r_ok AND NOT _fmf EQUAL -1)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_fastmix.inc" _fm_inc)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_fastmix.inc")
  string(REPLACE "${_fm1}" "${_fm_inc}\nstatic void audio_callback_orig(void*, Uint8* output, int byteCount) {" _pa "${_pa}")
  message(STATUS "sound mixer: faster mixer inserted (nectar.ini audio_fastmix = 0 switches back to the original)")
else()
  message(WARNING "pc_audio.cpp does not match the faster-mixer patch (needs the audio_rate patch and audio_callback): the original mixer is used")
endif()
# NEWEST-82: percussion envelope fix (plucking was silent) + cheaper sequencer ticks; patches pc_audio.cpp text (_pa) and makes patched copies of pc_jam.cpp / pc_instrument_bank.cpp
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_audio_opt.cmake" "${N3DS_PCJAM_SRC}" "${PC_PORT_DIR}/audio/pc_instrument_bank.cpp")
include("${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_audio_opt.cmake")
set(N3DS_PCAUDIO_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/n3ds_pc_audio_patched.cpp")
n3ds_write_if_changed("${N3DS_PCAUDIO_PATCHED}" "${_pa}")
file(READ "${N3DS_AUDSTUB_SRC}" _as)
set(_b1 "void Jac_Gsync(void) {")
string(FIND "${_as}" "${_b1}" _as1)
if(_as1 EQUAL -1)
  message(FATAL_ERROR "audio_stubs.cpp no longer matches the 3DS audio patch pattern (Jac_Gsync); update the patch in n3ds_port/CMakeLists.txt")
endif()
string(REPLACE "${_b1}" "void n3ds_diag_loop(void);\nvoid Jac_Gsync(void) {\n    n3ds_diag_loop();" _as "${_as}")
# 32-bit ARM fixes (s32 is 'long' and u32 is 'unsigned long' there; the PC build never saw these mismatches)
string(REPLACE "stickX = std::clamp(stickX, -127, 127);" "stickX = std::clamp<s32>(stickX, -127, 127);" _as "${_as}")
string(REPLACE "stickY = std::clamp(stickY, -127, 127);" "stickY = std::clamp<s32>(stickY, -127, 127);" _as "${_as}")
string(REPLACE "if (!value || !value[0]) return 0xFFFFFFFFu;" "if (!value || !value[0]) return (u32)0xFFFFFFFFu;" _as "${_as}")
string(REPLACE "return mask ? mask : 0xFFFFFFFFu;" "return mask ? mask : (u32)0xFFFFFFFFu;" _as "${_as}")
# Sound-effect diagnostics ("[sfx]" log line, platform/n3ds_sfx_diag.inc): counters around Jac_PlayOrimaSe / Jac_PlaySystemSe and the engine's send calls.
# Every pattern is a single line so CRLF checkouts match too; if one is missing the diagnostics are simply left out (no build failure).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_sfx_diag.inc")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds_sfx_diag.inc" _sfxinc)
set(_s1 "void Jac_Start(void* heap, u32 heapSize, u32 aramBase, const char* dataPath) {")
set(_s2 "void Jac_PlayOrimaSe(u32 id) {")
set(_s3 "void Jac_PlaySystemSe(s32 seID) {")
string(FIND "${_as}" "${_s1}" _sf1)
string(FIND "${_as}" "${_s2}" _sf2)
string(FIND "${_as}" "${_s3}" _sf3)
if(NOT _sf1 EQUAL -1 AND NOT _sf2 EQUAL -1 AND NOT _sf3 EQUAL -1)
  string(REPLACE "${_s2}" "void Jac_PlayOrimaSe(u32 id) {\n    n3ds_sfx_note_orima((unsigned)id);" _as "${_as}")
  string(REPLACE "${_s3}" "void Jac_PlaySystemSe(s32 seID) {\n    n3ds_sfx_note_system();" _as "${_as}")
  string(REPLACE "${_s1}" "${_sfxinc}\n${_s1}" _as "${_as}")
  # positional event sounds (Pikmin voices incl. plucking): wrap Jac_CreateEvent / Jac_PlayEventAction so their failures are counted
  set(_s4 "BOOL Jac_PlayEventAction(int index, int action) {")
  set(_s5 "int Jac_CreateEvent(u32 type, SVector_* position) {")
  string(FIND "${_as}" "${_s4}" _sf4)
  string(FIND "${_as}" "${_s5}" _sf5)
  if(NOT _sf4 EQUAL -1 AND NOT _sf5 EQUAL -1)
    string(REPLACE "${_s4}" "static BOOL n3ds_sfx_play_event_real(int index, int action);\nBOOL Jac_PlayEventAction(int index, int action) { const BOOL n3r = n3ds_sfx_play_event_real(index, action); n3ds_sfx_note_play(n3r != 0); return n3r; }\nstatic BOOL n3ds_sfx_play_event_real(int index, int action) {" _as "${_as}")
    string(REPLACE "${_s5}" "static int n3ds_sfx_create_event_real(u32 type, SVector_* position);\nint Jac_CreateEvent(u32 type, SVector_* position) { const int n3r = n3ds_sfx_create_event_real(type, position); n3ds_sfx_note_create(n3r); return n3r; }\nstatic int n3ds_sfx_create_event_real(u32 type, SVector_* position) {" _as "${_as}")
  else()
    message(WARNING "audio_stubs.cpp does not match the event-sound diagnostics patterns: no [sfx] events line in this build")
  endif()
else()
  message(WARNING "audio_stubs.cpp does not match the sound-effect diagnostics patterns: no [sfx] log line in this build")
endif()
# Pikmin voice map (log 310510ca + your Land.wav / Vo_5.wav): the Pikmin event (type 6) sends ACTION_STATUS commands 0x90 0xD9 0x29 0x8F 0xA0 0xA1 ... for
# actions 0, 1, 2, 3, 4, 5 ..., and pikise.jam plays voice key = command - 0xA0. So every Pikmin voice came out 4 places early: landing (action 22)
# played voice 18 instead of Land (voice 22), grabbing a pellet (HANG, 9) played voice 5 (the 3.3 s cry) instead of voice 9, the death cries (4-6)
# played voices 0-2. The voice bank has exactly 31 Pikmin voices (keys 0-30) = actions 0-30, so action N now sends 0xA0 + N.
# nectar.ini piki_voice_map = 0 restores the table's commands.
set(_pv1 "    if (!pc_event_command(event.type, action, command)\n        || !pc_event_status(event.type, action, status, statusIndex)) {\n        trace_event_action(index, action, \"SIN COMANDO EN LA TABLA\", event.type, 0, -1);\n        return FALSE;\n    }")
string(FIND "${_as}" "${_pv1}" _pvf)
if(NOT _pvf EQUAL -1)
  string(REPLACE "${_pv1}" "${_pv1}\n    { if (event.type == 6 && action >= 0 && action <= 30 && n3ds_piki_voice_map()) command = static_cast<u16>(0xA0 + action); }   /* n3ds: Pikmin voice N = action N */" _as "${_as}")
  set(_as "extern \"C\" int n3ds_piki_voice_map(void);\n${_as}")
  message(STATUS "sound: Pikmin voice map applied (action N plays voice N)")
else()
  message(WARNING "audio_stubs.cpp does not match the Pikmin voice-map patch: Pikmin voices keep the table's mapping")
endif()
set(N3DS_AUDSTUB_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/n3ds_audio_stubs_patched.cpp")
n3ds_write_if_changed("${N3DS_AUDSTUB_PATCHED}" "${_as}")
set(N3DS_AUDIO_SOURCES
  ${N3DS_PCAUDIO_PATCHED} ${N3DS_AUDSTUB_PATCHED}
  ${PC_PORT_DIR}/audio/pc_envelope.cpp ${N3DS_PCJAM_SRC} ${N3DS_IBANK_SRC}
  ${PC_PORT_DIR}/audio/pc_wave_bank.cpp ${PC_PORT_DIR}/audio/pc_sequence_archive.cpp
  ${NECTAR_ROOT}/src/jaudio/pikiseq.c)

# Upstream's portable Dolphin-SDK stubs, reused unmodified (GX->pc_gfx calls, DVD from assets/, saves, ...).
set(UPSTREAM_PORT_SOURCES
  ${N3DS_OS_STUBS_PATCHED}                    ${PC_PORT_DIR}/dolphin_stubs/gx_stubs.cpp
  ${N3DS_DVD_PATCHED}  ${PC_PORT_DIR}/dolphin_stubs/vi_stubs.cpp
  ${PC_PORT_DIR}/dolphin_stubs/card_stubs.cpp ${PC_PORT_DIR}/dolphin_stubs/misc_stubs.cpp
  ${PC_PORT_DIR}/timing/pc_frame_scheduler.cpp  ${PC_PORT_DIR}/timing/pc_render_phase.cpp
  ${PC_PORT_DIR}/timing/pc_visual_snapshot.cpp  ${PC_PORT_DIR}/timing/pc_camera_snapshot.cpp
  ${PC_PORT_DIR}/timing/pc_visual_runtime.cpp   ${PC_PORT_DIR}/timing/pc_render_packet.cpp
  ${PC_PORT_DIR}/timing/pc_tick_profiler.cpp)
set(N3DS_PLATFORM_SOURCES
  platform/n3ds_main.cpp platform/n3ds_ctru_io.cpp platform/n3ds_game.cpp platform/n3ds_window.cpp
  platform/n3ds_pad.cpp platform/n3ds_mman.cpp platform/n3ds_stubs_gen.cpp
  platform/n3ds_alloc_trace.cpp platform/n3ds_trace.cpp platform/n3ds_gpu.cpp platform/n3ds_gfx.cpp
  platform/n3ds_tex.cpp platform/n3ds_config.cpp platform/n3ds_texconv.cpp platform/n3ds_tev.cpp platform/n3ds_bench.cpp platform/n3ds_audio.cpp platform/n3ds_sdl_audio.cpp platform/n3ds_stx.cpp platform/n3ds_pak.cpp platform/n3ds_worker.cpp platform/n3ds_linear_lock.cpp platform/n3ds_dbgmenu.cpp)

add_library(nectar_port_3ds STATIC ${UPSTREAM_PORT_SOURCES} ${N3DS_PLATFORM_SOURCES} ${N3DS_AUDIO_SOURCES})
target_link_libraries(nectar_port_3ds PUBLIC pikmin_legacy_3ds)
target_include_directories(nectar_port_3ds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/platform ${PC_PORT_DIR}/dolphin_stubs)
if(N3DS_SE_QUEUE_MODE EQUAL 1)
  target_include_directories(nectar_port_3ds BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/n3ds_se_inc)   # audio/pc_jam.h with childPortPending, ahead of the repo's
endif()
set_source_files_properties("${N3DS_AUDSTUB_PATCHED}" PROPERTIES COMPILE_DEFINITIONS "N3DS_SE_QUEUE_MODE=${N3DS_SE_QUEUE_MODE}")  # dolphin_stubs: patched os_stubs copy lives in the build dir
target_compile_definitions(nectar_port_3ds PRIVATE N3DS_ARENA_MB=${N3DS_ARENA_MB} N3DS_RING_VERTS=${N3DS_RING_VERTS} N3DS_CMDBUF_KB=${N3DS_CMDBUF_KB}
  N3DS_LINEAR_HEAP_MB=${N3DS_LINEAR_HEAP_MB} N3DS_TEX_BUDGET_KB=${N3DS_TEX_BUDGET_KB})
target_compile_options(nectar_port_3ds PRIVATE ${NECTAR_COMPILE_OPTIONS})
if(N3DS_CHECKER_SHIM)
  target_include_directories(nectar_port_3ds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/checker_shim)
endif()

# ---- Graphics stage 2: PICA vertex shader (picasso, ships with devkitPro) embedded as a C array --------------
#   -DN3DS_STRETCH=ON stretches 4:3 over the whole 400x240 top screen instead of a centred 320x240 (pillarbox).
option(N3DS_STRETCH "Stretch the 4:3 picture over 400x240 instead of pillarboxing to 320x240" OFF)
if(N3DS_STRETCH)
  target_compile_definitions(nectar_port_3ds PRIVATE N3DS_STRETCH=1)
endif()
find_program(PICASSO picasso HINTS $ENV{DEVKITPRO}/tools/bin)
if(PICASSO)
  set(N3DS_SHBIN   ${CMAKE_CURRENT_BINARY_DIR}/passthrough.shbin)
  set(N3DS_SHBIN_C ${CMAKE_CURRENT_BINARY_DIR}/passthrough_shbin.c)
  add_custom_command(OUTPUT ${N3DS_SHBIN}
    COMMAND ${PICASSO} -o ${N3DS_SHBIN} ${CMAKE_CURRENT_SOURCE_DIR}/platform/shader/passthrough.v.pica
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/shader/passthrough.v.pica COMMENT "picasso passthrough.v.pica")
  add_custom_command(OUTPUT ${N3DS_SHBIN_C}
    COMMAND ${CMAKE_COMMAND} -DIN=${N3DS_SHBIN} -DOUT=${N3DS_SHBIN_C} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.cmake
    DEPENDS ${N3DS_SHBIN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.cmake COMMENT "embedding passthrough.shbin")
  target_sources(nectar_port_3ds PRIVATE ${N3DS_SHBIN_C})
  target_compile_definitions(nectar_port_3ds PRIVATE N3DS_HAVE_SHADER=1)
else()
  message(WARNING "picasso not found (expected in $DEVKITPRO/tools/bin): the citro3d backend will only clear the screen.")
endif()

if(DEFINED ENV{DEVKITPRO})
  include_directories($ENV{DEVKITPRO}/libctru/include)
  link_directories($ENV{DEVKITPRO}/libctru/lib)
endif()
add_executable(nectar_3ds platform/n3ds_entry.cpp)   # empty TU; real code is in the libs (see start-group below)
# The game lib and the port lib reference each other, so link them as a group.
target_link_libraries(nectar_3ds PRIVATE -Wl,--start-group pikmin_legacy_3ds nectar_port_3ds -Wl,--end-group citro3d ctru m)
# Name the caller of any big / failed allocation (see platform/n3ds_alloc_trace.cpp).
target_link_options(nectar_3ds PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=memalign)   # + linear-heap fallback
# libctru's linear allocator is not thread-safe; the render worker (core 2) and the game thread both use it (platform/n3ds_linear_lock.cpp)
target_link_options(nectar_3ds PRIVATE -Wl,--wrap=linearAlloc -Wl,--wrap=linearMemAlign -Wl,--wrap=linearRealloc -Wl,--wrap=linearFree -Wl,--wrap=linearSpaceFree)
# every svcBreak (what a Luma crash dump always ends in) goes through __wrap_svcBreak in n3ds_ctru_io.cpp, which logs the cause first
target_link_options(nectar_3ds PRIVATE -Wl,--wrap=svcBreak)
# Record OS thread / message / DVD / retrace calls in a ring buffer that the watchdog dumps on a stall (platform/n3ds_trace.cpp).
target_link_options(nectar_3ds PRIVATE -Wl,--wrap=OSCreateThread -Wl,--wrap=OSResumeThread -Wl,--wrap=OSJoinThread
                    -Wl,--wrap=OSSendMessage -Wl,--wrap=OSReceiveMessage -Wl,--wrap=OSWaitCond
                    -Wl,--wrap=DVDReadPrio -Wl,--wrap=VIWaitForRetrace)
# 1f: signal / mutex / yield / sleep / wakeup / OSGetTime (n3ds_trace.cpp). Mutex + OSGetTime are counters only (too hot for the ring).
target_link_options(nectar_3ds PRIVATE -Wl,--wrap=OSSignalCond -Wl,--wrap=OSLockMutex -Wl,--wrap=OSUnlockMutex
                    -Wl,--wrap=OSYieldThread -Wl,--wrap=OSSleepThread -Wl,--wrap=OSWakeupThread -Wl,--wrap=OSGetTime)

# ---- Packaging: .3dsx (Homebrew Launcher), .cia (HOME-menu install via FBI), or both -------------
#   -DN3DS_PACKAGE=3dsx   (default)   -DN3DS_PACKAGE=cia   -DN3DS_PACKAGE=both
# .cia additionally needs makerom and bannertool (not in the devkitPro repos; put them in
# $DEVKITPRO/tools/bin or on PATH): Project_CTR (makerom), 3DSGuy/Steveice10 bannertool.
set(N3DS_PACKAGE "3dsx" CACHE STRING "Package format: 3dsx, cia or both")
set_property(CACHE N3DS_PACKAGE PROPERTY STRINGS 3dsx cia both)
string(TOLOWER "${N3DS_PACKAGE}" _pkg)
if(NOT _pkg MATCHES "^(3dsx|cia|both)$")
  message(FATAL_ERROR "N3DS_PACKAGE must be 3dsx, cia or both (got '${N3DS_PACKAGE}')")
endif()
if(N3DS_TEST_BUILD)
  set(N3DS_TITLE "Open Nectar TEST")
else()
  set(N3DS_TITLE "Open Nectar")
endif()
set(N3DS_DESC   "Pikmin native port WIP")   # no parentheses: this string reaches /bin/sh through make
set(N3DS_AUTHOR "Open Nectar 3DS")

find_program(SMDHTOOL smdhtool HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(DSXTOOL 3dsxtool HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(MAKEROM makerom HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(BANNERTOOL bannertool HINTS $ENV{DEVKITPRO}/tools/bin)
# r30: HOME-menu banner sound. platform/banner.wav is a short soft chime; put your own platform/banner_custom.wav (16-bit PCM WAV,
# up to ~3 s) next to it to use that instead, e.g. a jingle you extracted from your game.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/banner_custom.wav)
  set(N3DS_BANNER_WAV ${CMAKE_CURRENT_SOURCE_DIR}/platform/banner_custom.wav)
else()
  set(N3DS_BANNER_WAV ${CMAKE_CURRENT_SOURCE_DIR}/platform/banner.wav)
endif()

set(_want_3dsx OFF)
set(_want_cia OFF)
if(_pkg STREQUAL "3dsx" OR _pkg STREQUAL "both")
  set(_want_3dsx ON)
endif()
if(_pkg STREQUAL "cia" OR _pkg STREQUAL "both")
  set(_want_cia ON)
endif()

# Changing the HOME-menu icon/banner/sound alone must still re-run the POST_BUILD packaging steps below (smdh, 3dsx, bnr, cia): they only run when
# the ELF is re-linked, so make the link depend on those files.
set_property(TARGET nectar_3ds APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/icon.png ${CMAKE_CURRENT_SOURCE_DIR}/platform/banner.png ${N3DS_BANNER_WAV})

if(NOT SMDHTOOL)
  message(WARNING "smdhtool not found: no .smdh, no packaging (only the ELF will be built).")
else()
  add_custom_command(TARGET nectar_3ds POST_BUILD
    COMMAND ${SMDHTOOL} --create "${N3DS_TITLE}" "${N3DS_DESC}" "${N3DS_AUTHOR}"
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/icon.png nectar_3ds.smdh
    COMMENT "Packaging nectar_3ds.smdh")
  if(_want_3dsx)
    if(DSXTOOL)
      add_custom_command(TARGET nectar_3ds POST_BUILD
        COMMAND ${DSXTOOL} $<TARGET_FILE:nectar_3ds> nectar_3ds.3dsx --smdh=nectar_3ds.smdh
        COMMENT "Packaging nectar_3ds.3dsx")
    else()
      message(WARNING "3dsxtool not found: no .3dsx will be built.")
    endif()
  endif()
  if(_want_cia)
    if(MAKEROM AND BANNERTOOL)
      add_custom_command(TARGET nectar_3ds POST_BUILD
        COMMAND ${BANNERTOOL} makebanner -i ${CMAKE_CURRENT_SOURCE_DIR}/platform/banner.png
                -a ${N3DS_BANNER_WAV} -o nectar_3ds.bnr
        COMMAND ${MAKEROM} -f cia -o nectar_3ds.cia -rsf ${CMAKE_CURRENT_BINARY_DIR}/nectar.rsf
                -target t -exefslogo -elf $<TARGET_FILE:nectar_3ds> -icon nectar_3ds.smdh -banner nectar_3ds.bnr
        COMMENT "Packaging nectar_3ds.cia")
    else()
      message(WARNING "N3DS_PACKAGE=${N3DS_PACKAGE} but makerom and/or bannertool were not found: no .cia will be built. "
                      "Put makerom(.exe) and bannertool(.exe) in $DEVKITPRO/tools/bin or on PATH, then reconfigure.")
    endif()
  endif()
endif()

# ---- nectar_pakbuild.3dsx: builds nectar.pak on the console itself (tools/pakbuild). Same file as tools/make_pak.py on a PC. ------------------
option(N3DS_BUILD_PAKBUILD "Also build nectar_pakbuild.3dsx (on-console nectar.pak builder)" ON)
if(N3DS_BUILD_PAKBUILD)
  add_executable(nectar_pakbuild tools/pakbuild/main.cpp tools/pakbuild/pak_builder.cpp)
  target_include_directories(nectar_pakbuild PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/pakbuild)
  target_link_libraries(nectar_pakbuild PRIVATE ctru m)
  if(SMDHTOOL AND DSXTOOL)
    add_custom_command(TARGET nectar_pakbuild POST_BUILD
      COMMAND ${SMDHTOOL} --create "Nectar pak builder" "Packs Open Nectar assets into nectar.pak" "${N3DS_AUTHOR}"
              ${CMAKE_CURRENT_SOURCE_DIR}/platform/icon.png nectar_pakbuild.smdh
      COMMAND ${DSXTOOL} $<TARGET_FILE:nectar_pakbuild> nectar_pakbuild.3dsx --smdh=nectar_pakbuild.smdh
      COMMENT "Packaging nectar_pakbuild.3dsx")
  endif()
endif()
# ---- nectar_installer.cia: a HOME-menu app with a step-by-step setup guide, an SD card check (game files, nectar.ini, nectar.pak, what is
# installed) and an installer for sdmc:/nectar.cia (any *nectar*.cia in the SD card root). -----
# Source + exheader in tools/cia_installer. Needs smdhtool, bannertool and makerom (same tools as the game CIA). Output: nectar_installer.cia in the build dir.
option(N3DS_BUILD_INSTALLER "Also build nectar_installer.cia (setup guide + SD check + installs the Nectar .cia from the SD card root)" ON)
if(N3DS_BUILD_INSTALLER)
  add_executable(nectar_installer tools/cia_installer/main.c tools/cia_installer/pak_c.cpp tools/pakbuild/pak_builder.cpp)
  target_include_directories(nectar_installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/pakbuild)   # the installer builds nectar.pak too (R)
  target_link_libraries(nectar_installer PRIVATE ctru m)
  set_property(TARGET nectar_installer APPEND PROPERTY LINK_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/icon.png ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/banner.png
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/installer.rsf ${N3DS_BANNER_WAV})
  if(SMDHTOOL AND MAKEROM AND BANNERTOOL)
    add_custom_command(TARGET nectar_installer POST_BUILD
      COMMAND ${SMDHTOOL} --create "Nectar Installer" "Setup guide, SD card check and installer" "${N3DS_AUTHOR}"
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/icon.png nectar_installer.smdh
      COMMAND ${BANNERTOOL} makebanner -i ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/banner.png
              -a ${N3DS_BANNER_WAV} -o nectar_installer.bnr
      COMMAND ${MAKEROM} -f cia -o nectar_installer.cia -rsf ${CMAKE_CURRENT_SOURCE_DIR}/tools/cia_installer/installer.rsf
              -target t -exefslogo -elf $<TARGET_FILE:nectar_installer> -icon nectar_installer.smdh -banner nectar_installer.bnr
      COMMENT "Packaging nectar_installer.cia")
  else()
    message(WARNING "smdhtool, makerom and/or bannertool not found: nectar_installer.cia will not be built (only its ELF).")
  endif()
endif()
message(STATUS "Game sources: ${NECTAR_ROOT}/src -> pikmin_legacy_3ds (USA Rev 1, ARM11)")
