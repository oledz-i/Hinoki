// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <mem/util.h> // Address.
#include <util/types.h>

#include <cpu/common.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

struct MemState;

CPUStatePtr init_cpu(bool cpu_opt, SceUID thread_id, std::size_t processor_id, MemState &mem);
// Guest code changed (a module loaded or unloaded, code patched): a JIT kept for reuse must forget
// what it compiled before another thread gets it
void note_guest_code_changed();
// Destroys the JITs kept for reuse (game shutdown: their code caches go back to the pool)
void flush_cpu_pool();
CPUStop run(CPUState &state);
CPUStop step(CPUState &state);
// Sets up a call of fn with args: the first four in r0 to r3, the rest on the stack, and the
// return address in LR
void place_call(CPUState &state, Address fn, std::initializer_list<uint32_t> args);
void stop(CPUState &state);
uint32_t read_reg(CPUState &state, size_t index);
float read_float_reg(CPUState &state, size_t index);
void write_float_reg(CPUState &state, size_t index, float value);
uint32_t read_sp(CPUState &state);
uint32_t read_pc(CPUState &state);
uint32_t read_lr(CPUState &state);
uint32_t read_tpidruro(CPUState &state);
uint32_t read_tpidrurw(CPUState &state);
void write_reg(CPUState &state, size_t index, uint32_t value);
void write_sp(CPUState &state, uint32_t value);
void write_pc(CPUState &state, uint32_t value);
void write_lr(CPUState &state, uint32_t value);
void write_tpidruro(CPUState &state, uint32_t value);
void write_tpidrurw(CPUState &state, uint32_t value);
CPUContext save_context(CPUState &state);
void load_context(CPUState &state, const CPUContext &ctx);
std::size_t get_processor_id(CPUState &state);
void invalidate_jit_cache(CPUState &state, Address start, size_t length);

uint32_t read_fpscr(CPUState &state);
void write_fpscr(CPUState &state, uint32_t value);
uint32_t read_cpsr(CPUState &state);
void write_cpsr(CPUState &state, uint32_t value);

uint32_t stack_alloc(CPUState &state, size_t size);
void stack_free(CPUState &state, size_t size);

void clear_exclusive(CPUState &state);

// Debugging helpers
std::string disassemble(CPUState &state, uint64_t at, bool thumb, uint16_t *insn_size = nullptr);
std::string disassemble(CPUState &state, uint64_t at, uint16_t *insn_size = nullptr);
bool hit_breakpoint(CPUState &state);
void trigger_breakpoint(CPUState &state);
void set_log_code(CPUState &state, bool log);

void set_log_mem(CPUState &state, bool log);
bool get_log_mem(CPUState &state);

// The CPU state running on this host thread, for signal handlers
void set_current_cpu_state(CPUState *state);

#if defined(VITA3K_PLATFORM_IOS)
// JIT code caches on iOS. Each guest thread starts with a tiny one; a thread that fills it (and
// would otherwise wipe and recompile everything) moves to a small one, then to a big one.
// Most threads compile well under 4 MiB (Dynarmic only needs 1 MiB free to emit a block); in the
// logs about one thread per game fills 16 MiB. Games with worker pools keep 50+ threads alive
// (Dynasty Warriors 8 ran out of caches at 50 and could not start its workers).
inline constexpr std::size_t IOS_TINY_CODE_CACHE_SIZE = 4 * 1024 * 1024;
inline constexpr std::size_t IOS_SMALL_CODE_CACHE_SIZE = 16 * 1024 * 1024;
inline constexpr std::size_t IOS_BIG_CODE_CACHE_SIZE = 64 * 1024 * 1024;
// iOS 26+ prepares them all at launch, and the app is frozen while that happens (about 1.5 ms per
// MiB): 80 tiny + 16 small + 2 big = 98 threads in 704 MiB (was 64 + 32 + 2 = 896 MiB for the same
// 98 threads; logs show a game rarely moves more than a couple of threads past 4 MiB).
inline constexpr std::size_t IOS_TINY_CODE_CACHE_COUNT = 80;
inline constexpr std::size_t IOS_SMALL_CODE_CACHE_COUNT = 16;
inline constexpr std::size_t IOS_BIG_CODE_CACHE_COUNT = 2;
// The prepared caches count against the app's memory limit, which on 4 GB devices left about 1.1 GiB
// for the game (FIFA 15 was killed for memory on an iPhone 11). Devices with a low limit prepare fewer:
// these return the counts actually used, chosen once from the memory available at launch.
std::size_t ios_tiny_code_cache_count();
std::size_t ios_small_code_cache_count();
std::size_t ios_big_code_cache_count();
#endif
#if defined(VITA3K_PLATFORM_IOS) && defined(__aarch64__)
// iOS 26+ JIT (from Tsubomi): prepares the code caches above through the attached StikDebug
// universal.js script. Returns true when all of them are ready.
bool prewarm_ios_jit_code_cache_pool();
// How many caches the running prewarm has prepared so far
std::size_t ios_jit_code_cache_pool_progress();
// Prepared code caches of a size currently free in the pool
std::size_t ios_jit_code_cache_pool_available(std::size_t cache_size);
// Gives the physical memory of the caches waiting unused in the pool back to iOS (they keep their
// place and come back zeroed when used). Returns the bytes released (0 if trimming is off).
std::size_t trim_ios_jit_code_cache_pool();
#endif
#if defined(VITA3K_PLATFORM_IOS)
// "Fast CPU": lets the recompiler take floating-point shortcuts (NaN handling, reciprocal precision,
// FPCR). Faster, and almost always invisible. Applies to threads created afterwards.
void set_ios_fast_cpu(bool enabled);
void set_ios_accurate_cpu(bool enabled);
#endif
CPUState *get_current_cpu_state();
