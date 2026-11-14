// Vita3K project
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

// RAM helpers (see protocol.h): C interface for the emulator and the Swift frontend

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Settings (UserDefaults): v3k.ramHelpers (on/off), v3k.ramHelpers.count (1 ... 4 helper processes)
bool v3k_ram_helpers_enabled(void);
// The helper extension is inside the app (a signer can strip extensions)
bool v3k_ram_helpers_bundled(void);
// Running inside LiveContainer (it needs "helper processes" turned on for the app)
bool v3k_ram_helpers_in_livecontainer(void);

// Whether a container app (LiveContainer, Strata) will start the helpers for us
enum {
    V3K_RAM_SETUP_STANDALONE = 0, // not in a container: iOS starts them
    V3K_RAM_SETUP_CONTAINER_READY = 1, // the container's helper hooks are installed
    V3K_RAM_SETUP_SEPARATE_PROCESS = 2, // running in a LiveProcess: helpers can't start
    V3K_RAM_SETUP_OPTION_OFF = 3, // the app's "Helper Process Support" option is off (or too old a container)
    V3K_RAM_SETUP_NEEDS_REINSTALL = 4, // added to the container before it supported helpers: install again
};
int v3k_ram_helpers_container_setup(void);
// What to change in the container for helpers to work ("" when nothing)
const char *v3k_ram_helpers_container_hint(void);
const char *v3k_ram_helpers_reinstall_hint(void);

// Backs [host, host + size) with helper memory, from the start, a chunk at a time; returns the bytes
// backed (0 when off or no helper could lend any). The range must be reserved, inaccessible memory;
// it stays inaccessible (the guest allocator opens pages as before). `last`: this is the last range of
// this boot (the helpers' job is done)
uint64_t v3k_ram_helpers_back(void *host, uint64_t size, bool last);

// Starts helpers in the background so the next boot doesn't wait for them
void v3k_ram_helpers_prepare(void);
// After a game: ends the helpers that lent it memory (they stay alive while it runs) and prepares new ones
void v3k_ram_helpers_game_ended(void);

// RAM checker
uint32_t v3k_ram_helpers_live(void);
uint64_t v3k_ram_helpers_backed(void); // bytes of guest memory backed by helpers this boot
uint64_t v3k_ram_helpers_in_use(void); // of those, bytes the game has touched
// Starts a helper if none runs and asks how much it can lend (blocks: not on the main thread)
uint64_t v3k_ram_helpers_probe(void);
// Last thing that happened, for the checker and the log; never NULL
const char *v3k_ram_helpers_status(void);

// This process
uint64_t v3k_memory_footprint(void);
uint64_t v3k_memory_available(void);
uint64_t v3k_memory_physical(void);
// The large address space (com.apple.developer.kernel.extended-virtual-addressing, or room measured)
bool v3k_extended_address_space(void);
// Largest free block of addresses right now, in MiB
uint64_t v3k_largest_free_address_mib(void);

#ifdef __cplusplus
}
#endif
