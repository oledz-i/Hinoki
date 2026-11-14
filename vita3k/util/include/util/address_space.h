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

// iOS: whether the app has the large address space (com.apple.developer.kernel.extended-virtual-addressing).
// Free SideStore/AltStore signing drops that entitlement; the app then has only a few GiB of addresses,
// and the 4 GiB guest reservation leaves little room for everything else (JIT caches, textures, malloc).
// Container apps (LiveContainer, Strata) have it. Both the entitlement and the free address space are
// checked; the result is worked out once.

#include <cstdint>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <sys/mman.h>
#endif

namespace address_space {

struct Info {
    bool extended = true; // false: the small address space (use smaller caches)
    bool entitled = false;
    bool entitlement_checked = false;
    uint64_t largest_free_mib = 0; // when first checked
    uint64_t range_gib = 0;
};

// Largest block of free addresses right now, in MiB (to 64 MiB; at most 64 GiB)
inline uint64_t largest_free_block_mib() {
#if defined(__APPLE__)
    uint64_t low = 0, high = 64ull << 10;
    while (high - low > 64) {
        const uint64_t mid = (low + high) / 2;
        void *probe = mmap(nullptr, mid << 20, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (probe != MAP_FAILED) {
            munmap(probe, mid << 20);
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
#else
    return 64ull << 10;
#endif
}

inline const Info &info() {
    static const Info result = [] {
        Info i;
#if defined(__APPLE__)
        // Security/CoreFoundation looked up at run time: CoreFoundation's headers define a Ptr type that
        // clashes with the guest Ptr<T>
        using CreateFn = void *(*)(void *);
        using CopyFn = void *(*)(void *, void *, void *);
        using MakeStringFn = void *(*)(void *, const char *, uint32_t);
        using TypeOfFn = unsigned long (*)(void *);
        using BoolTypeFn = unsigned long (*)();
        using BoolValueFn = unsigned char (*)(void *);
        using ReleaseFn = void (*)(void *);
        auto create = reinterpret_cast<CreateFn>(dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf"));
        auto copy = reinterpret_cast<CopyFn>(dlsym(RTLD_DEFAULT, "SecTaskCopyValueForEntitlement"));
        auto make_string = reinterpret_cast<MakeStringFn>(dlsym(RTLD_DEFAULT, "CFStringCreateWithCString"));
        auto type_of = reinterpret_cast<TypeOfFn>(dlsym(RTLD_DEFAULT, "CFGetTypeID"));
        auto bool_type = reinterpret_cast<BoolTypeFn>(dlsym(RTLD_DEFAULT, "CFBooleanGetTypeID"));
        auto bool_value = reinterpret_cast<BoolValueFn>(dlsym(RTLD_DEFAULT, "CFBooleanGetValue"));
        auto release = reinterpret_cast<ReleaseFn>(dlsym(RTLD_DEFAULT, "CFRelease"));
        if (create && copy && make_string && type_of && bool_type && bool_value && release) {
            if (void *task = create(nullptr)) {
                i.entitlement_checked = true;
                for (const char *name : { "com.apple.developer.kernel.extended-virtual-addressing", "com.apple.private.kernel.extended-virtual-addressing" }) {
                    void *key = make_string(nullptr, name, 0x08000100 /* kCFStringEncodingUTF8 */);
                    if (!key)
                        continue;
                    if (void *value = copy(task, key, nullptr)) {
                        if (type_of(value) == bool_type() && bool_value(value))
                            i.entitled = true;
                        release(value);
                    }
                    release(key);
                }
                release(task);
            }
        }
        task_vm_info_data_t vm_info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS && vm_info.max_address > vm_info.min_address)
            i.range_gib = (vm_info.max_address - vm_info.min_address) >> 30;
        // What matters is the room itself: with the large address space tens of GiB are free in one
        // piece, without it only a few. Whoever has the room keeps the full-size caches
        i.largest_free_mib = largest_free_block_mib();
        i.extended = i.entitled || i.largest_free_mib >= (12ull << 10);
#endif
        return i;
    }();
    return result;
}

inline bool extended() {
    return info().extended;
}

} // namespace address_space
