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

#include <module/module.h>

#include "../SceNet/SceNet.h"

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNetInternal);

#include <random>

EXPORT(int, sceNetInternalInetPton, int af, const char *src, void *dst) {
    return CALL_EXPORT(sceNetInetPton, af, src, dst);
}

// Unnamed in the NID databases. The ad hoc matching module (adhoc_matching.suprx) calls it with a
// pointer to a 32-bit word in two places: to seed the sequence number it puts in every matching
// packet (a 0 result is turned into 1), and as rand() for a random retry delay, scaled with
// value / (0x7FFFFFFF / n + 1). So it writes a random number in 0..0x7FFFFFFF.
EXPORT(int, SceNetInternal_689B9D7D, uint32_t *out) {
    TRACY_FUNC(SceNetInternal_689B9D7D, out);
    thread_local std::random_device device;
    thread_local std::mt19937 generator(device());
    if (!out)
        return SCE_NET_ERROR_EINVAL;
    *out = generator() & 0x7FFFFFFF;
    return 0;
}

EXPORT(int, sceNetInternalIcmConnect, int sid, int flags) {
    TRACY_FUNC(sceNetInternalIcmConnect, sid, flags);
    // call sceNetSyscallIcmConnect(sid, flags)
    return UNIMPLEMENTED();
}
