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

#include "cpu/common.h"
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>
#include <util/recent_imports.h>

#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

// iOS JIT region pool, from Tsubomi by 125hz (https://github.com/125hz/Tsubomi).
// oaknut is dynarmic's arm64 assembler; its pool only exists on arm64 devices.
#if defined(VITA3K_PLATFORM_IOS) && defined(__aarch64__)
#include <oaknut/code_block.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(VITA3K_PLATFORM_IOS) && defined(__aarch64__)
namespace {

std::atomic<std::size_t> ios_jit_regions_prepared{ 0 };

// Games that start and end threads all the time (Hotline Miami: about 2,200 in a minute and a half)
// filled the log with three lines per thread. The first ones and then every 256th are enough.
static bool log_jit_churn() {
    static std::atomic<uint32_t> count{ 0 };
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
    return n < 48 || n % 256 == 0;
}

void log_ios_jit_region_pool_event(oaknut::JitRegionPoolEvent event, std::size_t size, std::size_t available) {
    switch (event) {
    case oaknut::JitRegionPoolEvent::Take:
        if (log_jit_churn())
            LOG_INFO("iOS JIT region pool take: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Return:
        if (log_jit_churn())
            LOG_INFO("iOS JIT region pool return: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Miss:
        LOG_INFO("iOS JIT region pool miss: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Prewarm:
        // (one cache prepared by the one-by-one path; progress is counted there)
        ios_jit_regions_prepared++;
        break;
    case oaknut::JitRegionPoolEvent::SessionLost: {
        // After launch the JIT script is detached, so this means every prepared cache is in use (the game
        // has more threads alive than there are caches), not that StikDebug went away. Said once.
        static std::atomic_bool logged{ false };
        if (!logged.exchange(true)) {
            LOG_ERROR("No JIT code cache left for a new {} MiB cache: all prepared caches are in use (too many game threads at once)",
                size / (1024 * 1024));
            if (auto logger = spdlog::default_logger())
                logger->flush();
        }
        break;
    }
    }
}

} // namespace

struct IosCacheTier {
    std::size_t size;
    std::size_t count;
};

namespace {

// The app's own memory use (what iOS counts against its limit), in MiB
std::size_t ios_footprint_mib() {
    task_vm_info_data_t vm_info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<std::size_t>(vm_info.phys_footprint / (1024 * 1024));
}

// Whether iOS gave this process the large address space (com.apple.developer.kernel.extended-virtual-addressing).
// Free SideStore signing drops it: the app then gets a few GiB of addresses, and the 4 GiB guest
// reservation plus a full JIT pool (mapped twice: RX and RW) leaves almost nothing for malloc
bool ios_has_extended_va() {
    static const bool extended = [] {
        // Looked up at run time: CoreFoundation's headers define a Ptr type that clashes with the guest Ptr<T>
        using CreateFn = void *(*)(void *);
        using CopyFn = void *(*)(void *, void *, void *);
        using MakeStringFn = void *(*)(void *, const char *, std::uint32_t);
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
        bool entitled = false;
        const bool can_check = create && copy && make_string && type_of && bool_type && bool_value && release;
        if (create && copy && make_string && type_of && bool_type && bool_value && release) {
            if (void *task = create(nullptr)) {
                for (const char *name : { "com.apple.developer.kernel.extended-virtual-addressing", "com.apple.private.kernel.extended-virtual-addressing" }) {
                    void *key = make_string(nullptr, name, 0x08000100 /* kCFStringEncodingUTF8 */);
                    if (!key)
                        continue;
                    if (void *value = copy(task, key, nullptr)) {
                        if (type_of(value) == bool_type() && bool_value(value))
                            entitled = true;
                        release(value);
                    }
                    release(key);
                }
                release(task);
            }
        }
        task_vm_info_data_t vm_info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        std::uint64_t range_gib = 0;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS && vm_info.max_address > vm_info.min_address)
            range_gib = (vm_info.max_address - vm_info.min_address) >> 30;
        // What actually matters is the free address space, so measure it too: with the large address
        // space there are tens of GiB free in one piece, without it only a few. A process that has the
        // room (whatever the entitlement check says, e.g. inside a container app) keeps the full pool
        std::uint64_t low = 0, high = 64ull << 10; // MiB
        while (high - low > 256) {
            const std::uint64_t mid = (low + high) / 2;
            void *probe = mmap(nullptr, mid << 20, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (probe != MAP_FAILED) {
                munmap(probe, mid << 20);
                low = mid;
            } else {
                high = mid;
            }
        }
        const bool roomy = low >= (12ull << 10);
        // If the entitlement can't be checked at all, the measurement decides
        const bool extended_va = entitled || roomy;
        LOG_INFO("iOS memory: extended address space {} (entitlement {}, largest free address block ~{} GiB, address range {} GiB)",
            extended_va ? "yes" : "NO", can_check ? (entitled ? "yes" : "no") : "unknown", low >> 10, range_gib);
        if (!extended_va)
            LOG_WARN("iOS memory: no extended address space (usual with free SideStore signing). Using a smaller JIT pool so the app doesn't run out of addresses");
        return extended_va;
    }();
    return extended;
}

struct IosCacheCounts {
    std::size_t tiny;
    std::size_t small;
    std::size_t big;
};

// Chosen once, before any cache is prepared, from what iOS lets the app use. Measured before the
// prewarm: about 2 GiB on 4 GB devices (iPhone 11), 6 GiB on 8 GB devices
const IosCacheCounts &ios_cache_counts() {
    static const IosCacheCounts counts = [] {
        const std::size_t available_mib = os_proc_available_memory() / (1024 * 1024);
        IosCacheCounts chosen{ IOS_TINY_CODE_CACHE_COUNT, IOS_SMALL_CODE_CACHE_COUNT, IOS_BIG_CODE_CACHE_COUNT };
        const char *tier = "full";
        if ((available_mib != 0 && available_mib < 1600) || !ios_has_extended_va()) {
            // 3 GB devices: 40 x 4 + 14 x 16 + 1 x 64 = 448 MiB (55 threads)
            chosen = { 40, 14, 1 };
            tier = ios_has_extended_va() ? "very low memory" : "small address space";
        } else if (available_mib != 0 && available_mib < 2600) {
            // 4 GB devices: 48 x 4 + 18 x 16 + 2 x 64 = 608 MiB (68 threads; FIFA 15 keeps about 25 alive)
            chosen = { 48, 18, 2 };
            tier = "low memory";
        }
        LOG_INFO("iOS JIT: {} MiB of memory available before the code caches: {} pool ({} x {} MiB, {} x {} MiB, {} x {} MiB)", available_mib, tier,
            chosen.tiny, IOS_TINY_CODE_CACHE_SIZE / (1024 * 1024), chosen.small, IOS_SMALL_CODE_CACHE_SIZE / (1024 * 1024), chosen.big,
            IOS_BIG_CODE_CACHE_SIZE / (1024 * 1024));
        return chosen;
    }();
    return counts;
}

std::vector<IosCacheTier> ios_cache_tiers() {
    return {
        { IOS_TINY_CODE_CACHE_SIZE, ios_cache_counts().tiny },
        { IOS_SMALL_CODE_CACHE_SIZE, ios_cache_counts().small },
        { IOS_BIG_CODE_CACHE_SIZE, ios_cache_counts().big },
    };
}

} // namespace

std::size_t ios_tiny_code_cache_count() {
    return ios_cache_counts().tiny;
}

std::size_t ios_small_code_cache_count() {
    return ios_cache_counts().small;
}

std::size_t ios_big_code_cache_count() {
    return ios_cache_counts().big;
}

// Prepares every missing cache with a single request to the JIT script: one large RX mapping, one
// writable alias, then cut into the small and big caches for the pool. StikDebug pauses the app for
// each request (32 separate requests froze it for ~3.5 s at launch), and the pause grows with the
// size prepared. Returns false (and leaves the pool untouched) if the script refuses the large
// request, so the one-by-one path can take over.
static bool prewarm_ios_jit_pool_in_one_request() {
    using namespace oaknut::detail;
    if (!is_process_traced())
        return false;
    std::vector<std::pair<std::size_t, std::size_t>> missing; // size, count
    std::size_t total = 0;
    std::size_t have_all = 0;
    for (const auto &tier : ios_cache_tiers()) {
        const std::size_t have = matching_region_count(tier.size);
        have_all += std::min(have, tier.count);
        if (have < tier.count) {
            missing.emplace_back(tier.size, tier.count - have);
            total += (tier.count - have) * tier.size;
        }
    }
    if (missing.empty())
        return true;

    const auto started = std::chrono::steady_clock::now();
    void *mapping = mmap(nullptr, total, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED)
        return false;
    auto *rx = static_cast<std::uint32_t *>(prepare_universal_jit_region(mapping, total));
    if (!rx) {
        munmap(mapping, total);
        LOG_WARN("iOS JIT: the script did not prepare a {} MiB region in one request, preparing caches one by one", total / (1024 * 1024));
        return false;
    }
    vm_prot_t current_protection = 0, maximum_protection = 0;
    vm_address_t writable = 0;
    if (vm_remap(mach_task_self(), &writable, total, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR, mach_task_self(),
            reinterpret_cast<vm_address_t>(rx), false, &current_protection, &maximum_protection, VM_INHERIT_NONE)
            != KERN_SUCCESS
        || vm_protect(mach_task_self(), writable, total, false, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        LOG_WARN("iOS JIT: could not map the batched region writable, preparing caches one by one");
        return false;
    }
    auto *rw = reinterpret_cast<std::uint32_t *>(writable);
    auto &pool = jit_region_pool();
    std::size_t offset_words = 0;
    std::size_t prepared = have_all;
    for (const auto &[size, count] : missing) {
        const std::size_t words = size / sizeof(std::uint32_t);
        for (std::size_t i = 0; i < count; i++) {
            {
                const std::lock_guard lock(pool.mutex);
                pool.regions.push_back(PreparedJitRegion{ rx + offset_words, rw + offset_words, size });
            }
            offset_words += words;
            ios_jit_regions_prepared = ++prepared;
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    LOG_INFO("iOS JIT: prepared {} MiB of code caches in one request, {} ms", total / (1024 * 1024), ms);
    return true;
}

bool prewarm_ios_jit_code_cache_pool() {
    oaknut::set_jit_region_pool_log_callback(&log_ios_jit_region_pool_event);
    const std::size_t available_before = os_proc_available_memory() / (1024 * 1024);
    const std::size_t footprint_before = ios_footprint_mib();
    LOG_INFO("iOS JIT: preparing {} x {} MiB, {} x {} MiB and {} x {} MiB code caches", ios_tiny_code_cache_count(),
        IOS_TINY_CODE_CACHE_SIZE / (1024 * 1024), ios_small_code_cache_count(), IOS_SMALL_CODE_CACHE_SIZE / (1024 * 1024),
        ios_big_code_cache_count(), IOS_BIG_CODE_CACHE_SIZE / (1024 * 1024));
    prewarm_ios_jit_pool_in_one_request();
    // Anything still missing (or everything, if the batched request was refused) one cache at a time
    bool complete = true;
    for (const auto &tier : ios_cache_tiers()) {
        const std::size_t available = oaknut::prewarm_jit_region_pool(tier.size, tier.count);
        if (available < tier.count) {
            LOG_CRITICAL("iOS JIT: only {} of {} code caches of {} MiB are ready", available, tier.count, tier.size / (1024 * 1024));
            complete = false;
        }
    }
    LOG_INFO("iOS JIT: code caches {}", complete ? "ready" : "incomplete");
    // What the caches really cost: they count against the memory limit only if their pages are resident
    const std::size_t available_after = os_proc_available_memory() / (1024 * 1024);
    const std::size_t footprint_after = ios_footprint_mib();
    LOG_INFO("iOS JIT: memory available {} -> {} MiB, app footprint {} -> {} MiB", available_before, available_after, footprint_before, footprint_after);
    return complete;
}

std::size_t ios_jit_code_cache_pool_progress() {
    return ios_jit_regions_prepared;
}

// RAM: a prepared code cache costs physical memory from the moment it is prepared (the JIT app touches
// every page), used or not: about 900 MiB for the whole pool, while games use a few dozen MiB of it. The
// pages of the caches waiting in the pool are given back to iOS: the cache keeps its place and its JIT
// permission, and pages come back zeroed when a thread takes it (dynarmic writes its code from scratch
// anyway). Whether the JIT app's prepared memory takes this is checked once per install on one cache:
// a marker file records the test, and a crash during it (the next launch finds the marker) turns the
// trimming off for good.
static std::string jit_trim_marker(const char *name) {
    const char *home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Library/Caches/" + name;
}

static bool marker_exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

static void create_marker(const std::string &path) {
    if (FILE *file = fopen(path.c_str(), "w")) {
        fputs("1", file);
        fflush(file);
        fsync(fileno(file));
        fclose(file);
    }
}

static bool jit_trim_works() {
    static int result = -1;
    if (result >= 0)
        return result == 1;
    const std::string broken = jit_trim_marker("v3k-jit-trim-broken");
    const std::string testing = jit_trim_marker("v3k-jit-trim-testing");
    const std::string ok = jit_trim_marker("v3k-jit-trim-ok");
    if (marker_exists(broken)) {
        result = 0;
        return false;
    }
    if (marker_exists(testing)) {
        // the test killed the app last time
        rename(testing.c_str(), broken.c_str());
        LOG_WARN("iOS JIT: giving unused code cache memory back crashed last time: it stays off");
        result = 0;
        return false;
    }
    if (marker_exists(ok)) {
        result = 1;
        return true;
    }
    oaknut::detail::PreparedJitRegion region;
    if (!oaknut::detail::take_prepared_jit_region(IOS_TINY_CODE_CACHE_SIZE, region))
        return false; // try again once the pool has one
    create_marker(testing);
    bool works = madvise(region.rw, region.size, MADV_FREE_REUSABLE) == 0;
    if (works) {
        // mov w0, #42 ; ret, written through the writable alias of the released pages, run from the executable one
        region.rw[0] = 0x52800540;
        region.rw[1] = 0xd65f03c0;
        sys_icache_invalidate(region.rx, 8);
        works = reinterpret_cast<int (*)()>(region.rx)() == 42;
        madvise(region.rw, region.size, MADV_FREE_REUSABLE);
    }
    oaknut::detail::return_prepared_jit_region(region);
    remove(testing.c_str());
    create_marker(works ? ok : broken);
    LOG_INFO("iOS JIT: giving unused code cache memory back {}", works ? "works here: on" : "is not possible here: off");
    result = works ? 1 : 0;
    return works;
}

std::size_t trim_ios_jit_code_cache_pool() {
    if (!oaknut::detail::requires_universal_jit() || !jit_trim_works())
        return 0;
    auto &pool = oaknut::detail::jit_region_pool();
    const std::lock_guard lock(pool.mutex);
    std::size_t released = 0;
    for (const auto &region : pool.regions) {
        if (madvise(region.rw, region.size, MADV_FREE_REUSABLE) == 0)
            released += region.size;
    }
    return released;
}

std::size_t ios_jit_code_cache_pool_available(std::size_t cache_size) {
    return oaknut::detail::matching_region_count(cache_size);
}

// Whether a code cache of this size can be made now: from the prepared pool on iOS 26+, always
// before (oaknut maps it itself)
static bool ios_code_cache_available(std::size_t cache_size) {
    return !oaknut::detail::requires_universal_jit() || oaknut::detail::matching_region_count(cache_size) > 0;
}
#endif

// A coprocessor instruction nobody handles: dynarmic's ARM64 backend compiles a "coprocessor
// exception" as ASSERT_FALSE, which ended the whole app (FIFA 15 hit an STC). Compile a call that
// does nothing instead (reads give 0) and log the instruction once when it is compiled. The game
// may misbehave afterwards, but the app stays up and the log names the instruction.
static std::uint64_t ignored_coprocessor_access(void *, std::uint32_t, std::uint32_t) {
    return 0;
}

static Dynarmic::A32::Coprocessor::Callback ignored_coprocessor_callback(int coproc, const char *what) {
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 32)
        LOG_WARN("Unhandled coprocessor instruction ({} on p{}): ignored", what, coproc);
    return Dynarmic::A32::Coprocessor::Callback{ &ignored_coprocessor_access, nullptr };
}

// Every coprocessor other than CP15 (the decoder sends VFP/NEON elsewhere)
class ArmDynarmicUnknownCoprocessor : public Dynarmic::A32::Coprocessor {
    int number;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ArmDynarmicUnknownCoprocessor(int number)
        : number(number) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return ignored_coprocessor_callback(number, "CDP");
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return ignored_coprocessor_callback(number, "MCR");
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return ignored_coprocessor_callback(number, "MCRR");
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return ignored_coprocessor_callback(number, "MRC");
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return ignored_coprocessor_callback(number, "MRRC");
    }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return ignored_coprocessor_callback(number, "LDC");
    }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return ignored_coprocessor_callback(number, "STC");
    }
};

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    uint32_t tpidruro = 0;
    uint32_t tpidrurw = 0;
    uint32_t sctlr = 0;
    uint32_t dacr = 0;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return ignored_coprocessor_callback(15, "CDP");
    }

    // MCR p15, 0, Rt, CRn, c0, opc2 for TPIDRURO (c13, 3), TPIDRURW (c13, 2), SCTLR (c1, 0) and DACR (c3, 0)
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        if (opc1 == 0 && CRm == CoprocReg::C0) {
            if (CRn == CoprocReg::C13 && opc2 == 3)
                return &tpidruro;
            if (!two && CRn == CoprocReg::C13 && opc2 == 2)
                return &tpidrurw;
            if (!two && CRn == CoprocReg::C1 && opc2 == 0)
                return &sctlr;
            if (!two && CRn == CoprocReg::C3 && opc2 == 0)
                return &dacr;
        }
        LOG_WARN("Unhandled CP15 MCR or MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, static_cast<int>(CRn), static_cast<int>(CRm), opc2);
        return ignored_coprocessor_callback(15, "MCR/MRC");
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return ignored_coprocessor_callback(15, "MCRR");
    }

    // MRC reads the same registers
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return CompileSendOneWord(two, opc1, CRn, CRm, opc2);
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return ignored_coprocessor_callback(15, "MRRC");
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return ignored_coprocessor_callback(15, "LDC");
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return ignored_coprocessor_callback(15, "STC");
    }
};

static constexpr bool LOG_INVALID_ACCESS = false;

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    CPUState *parent;
    DynarmicCPU *cpu;

public:
    explicit ArmDynarmicCallback(CPUState &parent, DynarmicCPU &cpu)
        : parent(&parent)
        , cpu(&cpu) {}

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu->log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        if (addr < KiB(4)) {
            report_trace(fmt::format("jump to 0x{:08X}", addr).c_str());
            return std::nullopt;
        }
        MemState &mem = *parent->mem;
        if (addr >= mem.host_page_size && !is_valid_addr(mem, addr)) {
            // A thread jumped into memory that holds nothing. Reading zeros there ran "andeq r0, r0, r0"
            // through megabytes of empty address space (Uncharted: Golden Abyss logged 50,000 invalid
            // reads and burned a core): stop it at the jump instead, and say once where it came from
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set())
                report_wild_jump(addr);
            report_trace(fmt::format("jump to 0x{:08X}", addr).c_str());
            return std::nullopt;
        }
        return MemoryRead32(addr);
    }

    // Everything a wild jump leaves behind: the code before the return address (in the caller's own
    // mode), the memory the registers point at, the stack, and the thread's last system calls
    void report_wild_jump(uint32_t addr) {
        MemState &mem = *parent->mem;
        const CPUContext ctx = cpu->save_context();
        const uint32_t lr = cpu->get_lr();
        const bool thumb_caller = lr & 1;
        std::string out = fmt::format("Thread {} jumped to unmapped address 0x{:08X}; LR 0x{:08X} ({} caller)\n{}", parent->thread_id, addr, lr,
            thumb_caller ? "Thumb" : "ARM", ctx.description());

        // The caller's code: 0x30 bytes up to the return address, decoded from a point that lines up
        const uint32_t ret = lr & ~1u;
        out += "Code before the return address:\n";
        for (uint32_t start_back : { 0x30u, 0x2Eu }) {
            if (ret < start_back)
                continue;
            uint32_t at = ret - start_back;
            std::string block;
            bool lined_up = false;
            for (int n = 0; n < 32 && at < ret + 4; ++n) {
                if (!is_valid_addr(mem, at)) {
                    block += fmt::format("  {:08X}: (unmapped)\n", at);
                    break;
                }
                uint16_t size = 0;
                const std::string insn = disassemble(*parent, at, thumb_caller, &size);
                block += fmt::format("  {}{:08X}: {}\n", at == ret ? "> " : "  ", at, insn);
                if (at == ret)
                    lined_up = true;
                at += size ? size : (thumb_caller ? 2 : 4);
            }
            if (lined_up || !thumb_caller) {
                out += block;
                break;
            }
        }

        const auto dump = [&](const char *what, uint32_t base, int before, int words) {
            const uint32_t from = (base & ~3u) - static_cast<uint32_t>(before) * 4;
            out += fmt::format("{} 0x{:08X}:", what, base);
            for (int i = 0; i < words; ++i) {
                const uint32_t a = from + i * 4;
                if (i % 8 == 0)
                    out += fmt::format("\n  {:08X}:", a);
                if (a >= mem.host_page_size && is_valid_addr(mem, a))
                    out += fmt::format(" {:08X}", *Ptr<uint32_t>(a).get(mem));
                else
                    out += " --------";
            }
            out += "\n";
        };
        dump("Stack at SP", ctx.get_sp(), 4, 48);
        for (int r = 0; r < 13; ++r) {
            const uint32_t v = ctx.cpu_registers[r];
            if (v >= mem.host_page_size && is_valid_addr(mem, v))
                dump(fmt::format("Memory at r{}", r).c_str(), v, 8, 24);
        }
        out += fmt::format("Last system calls of thread {} (oldest first):\n{}", parent->thread_id, recent_imports::describe());
        LOG_CRITICAL("{}", out);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        const bool valid = is_valid_addr(*self.parent->mem, static_cast<Address>(address));
        const std::string disassembly = valid ? disassemble(*self.parent, address) : "invalid address";
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.parent->thread_id, log_hex(address), disassembly);
    }

    // Write watch: the address of the guest instruction now running (memory callbacks don't see PC),
    // and the thread's last instructions with SP and LR as each one started
    uint32_t watch_pc = 0;
    bool watch_thumb = false;
    static constexpr uint32_t TRACE_LENGTH = 384;
    struct TraceEntry {
        uint32_t pc;
        uint32_t sp;
        uint32_t lr;
        bool thumb;
    };
    std::array<TraceEntry, TRACE_LENGTH> trace{};
    uint32_t trace_next = 0;

    static void NoteWatchPc(uint64_t self_, uint64_t pc_thumb, uint64_t sp_lr) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);
        self.watch_pc = static_cast<uint32_t>(pc_thumb);
        self.watch_thumb = (pc_thumb >> 32) != 0;
        self.trace[self.trace_next++ % TRACE_LENGTH] = { self.watch_pc, static_cast<uint32_t>(sp_lr), static_cast<uint32_t>(sp_lr >> 32), self.watch_thumb };
    }

    // The trace, oldest first, with each instruction decoded; only when the watch is on
    std::string describe_trace() {
        if (!write_watch::active() || trace_next == 0)
            return {};
        MemState &mem = *parent->mem;
        std::string out = fmt::format("Last {} instructions of thread {} (address, SP and LR before it):\n", std::min(trace_next, TRACE_LENGTH), parent->thread_id);
        const uint32_t first = trace_next > TRACE_LENGTH ? trace_next - TRACE_LENGTH : 0;
        for (uint32_t i = first; i < trace_next; i++) {
            const TraceEntry &e = trace[i % TRACE_LENGTH];
            const std::string insn = e.pc >= mem.host_page_size && is_valid_addr(mem, e.pc) ? disassemble(*parent, e.pc, e.thumb, nullptr) : std::string("?");
            out += fmt::format("  {:08X}{} sp={:08X} lr={:08X}  {}\n", e.pc, e.thumb ? "t" : "a", e.sp, e.lr, insn);
        }
        return out;
    }

    // Raw bytes, to decode offline
    std::string dump_bytes(uint32_t from, uint32_t size) {
        MemState &mem = *parent->mem;
        std::string out;
        for (uint32_t a = from & ~15u; a < from + size; a += 16) {
            out += fmt::format("  {:08X}:", a);
            for (uint32_t b = 0; b < 16; b += 4)
                out += (a + b >= mem.host_page_size && is_valid_addr(mem, a + b)) ? fmt::format(" {:08X}", *Ptr<uint32_t>(a + b).get(mem)) : std::string(" --------");
            out += "\n";
        }
        return out;
    }

    // Once per game: the trace and the code around it, when a watched game's thread goes wrong
    void report_trace(const char *why) {
        if (!write_watch::active())
            return;
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (reported.test_and_set())
            return;
        std::string out = fmt::format("Trace ({}):\n{}", why, describe_trace());
        // The code the trace ran through, as words, for an offline disassembler
        uint32_t lo = UINT32_MAX, hi = 0;
        const uint32_t first = trace_next > TRACE_LENGTH ? trace_next - TRACE_LENGTH : 0;
        for (uint32_t i = first; i < trace_next; i++) {
            const uint32_t pc = trace[i % TRACE_LENGTH].pc;
            if (pc < 0x81000000u || pc >= 0x82000000u)
                continue;
            lo = std::min(lo, pc);
            hi = std::max(hi, pc);
        }
        if (lo <= hi && hi - lo < 0x2000)
            out += fmt::format("Code 0x{:08X}..0x{:08X}:\n{}", lo - 0x40, hi + 0x40, dump_bytes(lo - 0x40, hi - lo + 0x80));
        out += fmt::format("Last system calls of thread {} (oldest first):\n{}", parent->thread_id, recent_imports::describe(40));
        LOG_CRITICAL("{}", out);
    }

    template <typename T>
    void report_watched_write(Dynarmic::A32::VAddr addr, T value) {
        if (!write_watch::may_report())
            return;
        MemState &mem = *parent->mem;
        const std::string insn = watch_pc >= mem.host_page_size && is_valid_addr(mem, watch_pc) ? disassemble(*parent, watch_pc, watch_thumb, nullptr) : std::string("?");
        LOG_CRITICAL("Watch: thread {} wrote {}-bit 0x{:X} at 0x{:08X} from 0x{:08X} ({}): {} | LR 0x{:08X}", parent->thread_id, sizeof(T) * 8,
            static_cast<uint64_t>(value), addr, watch_pc, watch_thumb ? "Thumb" : "ARM", insn, cpu->get_lr());
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (write_watch::active())
            ir.CallHostFunction(&NoteWatchPc, ir.Imm64(reinterpret_cast<uint64_t>(this)), ir.Imm64(pc | (static_cast<uint64_t>(is_thumb) << 32)),
                ir.Pack2x32To1x64(ir.GetRegister(Dynarmic::A32::Reg::SP), ir.GetRegister(Dynarmic::A32::Reg::LR)));
        if (cpu->log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64(reinterpret_cast<uint64_t>(this)), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
    }

    // Logs and returns true if guest code may not access addr
    bool invalid_access(const char *access, Dynarmic::A32::VAddr addr) {
        MemState &mem = *parent->mem;
        if (addr >= mem.host_page_size && is_valid_addr(mem, addr))
            return false;
        const uint32_t pc = cpu->get_pc();
        if (LOG_INVALID_ACCESS) {
            LOG_ERROR("Invalid {} at 0x{:x}\n{}", access, addr, cpu->save_context().description());
            if (pc < mem.host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
        } else if (!mem.invalid_access_logged.test_and_set()) {
            LOG_ERROR("Invalid {} at 0x{:x} by thread {} (pc 0x{:x}). Set LOG_INVALID_ACCESS for full reports", access, addr, parent->thread_id, pc);
            report_trace(fmt::format("invalid {} at 0x{:08X}", access, addr).c_str());
        } else {
            LOG_ERROR("Invalid {} at 0x{:x} by thread {} (pc 0x{:x})", access, addr, parent->thread_id, pc);
        }
        return true;
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        if (invalid_access("read", addr))
            return 0;
        const T ret = *Ptr<T>(addr).get(*parent->mem);
        if (cpu->log_mem) {
            LOG_TRACE("Read uint{}_t at address: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, ret);
        }
        return ret;
    }

    uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint8_t>(addr);
    }

    uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint16_t>(addr);
    }

    uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint32_t>(addr);
    }

    uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint64_t>(addr);
    }

    template <typename T>
    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {
        if (invalid_access("write", addr))
            return;
        if (write_watch::hits(addr, sizeof(T)))
            report_watched_write(addr, value);
        *Ptr<T>(addr).get(*parent->mem) = value;
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, value);
        }
    }

    void MemoryWrite8(Dynarmic::A32::VAddr addr, uint8_t value) override {
        MemoryWrite<uint8_t>(addr, value);
    }

    void MemoryWrite16(Dynarmic::A32::VAddr addr, uint16_t value) override {
        MemoryWrite<uint16_t>(addr, value);
    }

    void MemoryWrite32(Dynarmic::A32::VAddr addr, uint32_t value) override {
        MemoryWrite<uint32_t>(addr, value);
    }

    void MemoryWrite64(Dynarmic::A32::VAddr addr, uint64_t value) override {
        MemoryWrite<uint64_t>(addr, value);
    }

    template <typename T>
    bool MemoryWriteExclusive(Dynarmic::A32::VAddr addr, T value, T expected) {
        if (invalid_access("exclusive write", addr))
            return false;
        if (write_watch::hits(addr, sizeof(T)))
            report_watched_write(addr, value);
        const bool result = Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}", sizeof(T) * 8, addr, value, expected);
        }
        return result;
    }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr addr, uint8_t value, uint8_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr addr, uint16_t value, uint16_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr addr, uint32_t value, uint32_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr addr, uint64_t value, uint64_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), save_context(*parent).description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint:
            cpu->break_ = true;
            cpu->jit->HaltExecution();
            cpu->set_pc(cpu->is_thumb_mode() ? pc | 1 : pc);
            break;
        case Dynarmic::A32::Exception::NoExecuteFault:
            LOG_CRITICAL("PC is 0x{:x}, stopping thread {}\n{}", pc, parent->thread_id, cpu->save_context().description());
            [[fallthrough]];
        case Dynarmic::A32::Exception::WaitForInterrupt:
            cpu->halted = true;
            cpu->jit->HaltExecution();
            break;
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError:
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
        }
    }

    void CallSVC(uint32_t) override {
        cpu->svc_called = true;
        cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
    }

    void AddTicks(uint64_t ticks) override {}

    uint64_t GetTicksRemaining() override {
        return 1ull << 60;
    }
};

static Dynarmic::ExclusiveMonitor shared_monitor(MAX_CORE_COUNT);

#if defined(VITA3K_PLATFORM_IOS)
static std::atomic_bool ios_fast_cpu{ false };
// Accurate CPU (per game): Dynarmic's IR optimisations (constant propagation, get/set elimination,
// miscellaneous IR passes) are left out; the control-flow ones (block linking, return stack buffer,
// fast dispatch) stay, so the cost is moderate. For games whose CPU-side math comes out wrong with
// them (Madden NFL 13's faces, a best guess: the body skins fine, the separately animated face melts).
static std::atomic_bool ios_accurate_cpu{ false };
void set_ios_accurate_cpu(bool enabled) {
    ios_accurate_cpu = enabled;
    LOG_INFO("Accurate CPU (no IR optimisations): {}", enabled ? "on" : "off");
}

void set_ios_fast_cpu(bool enabled) {
    ios_fast_cpu = enabled;
    LOG_INFO("Fast CPU (floating-point shortcuts): {}", enabled ? "on" : "off");
}
#endif

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    // A write watch needs every guest write to come through the callbacks
    const bool slow_memory = log_mem || write_watch::active();
    if (parent->mem->use_page_table) {
        config.page_table = (slow_memory || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(parent->mem->page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!slow_memory && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(parent->mem->memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    static const auto unknown_coprocessors = [] {
        std::array<std::shared_ptr<Dynarmic::A32::Coprocessor>, 15> coprocessors;
        for (int i = 0; i < 15; i++)
            coprocessors[i] = std::make_shared<ArmDynarmicUnknownCoprocessor>(i);
        return coprocessors;
    }();
    for (int i = 0; i < 15; i++)
        config.coprocessors[i] = unknown_coprocessors[i];
    config.coprocessors[15] = cp15;
    config.processor_id = core_id;
    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
#if defined(VITA3K_PLATFORM_IOS)
    if (cpu_opt && ios_accurate_cpu)
        config.optimizations = Dynarmic::OptimizationFlag::BlockLinking | Dynarmic::OptimizationFlag::ReturnStackBuffer
            | Dynarmic::OptimizationFlag::FastDispatch;
    if (cpu_opt && ios_fast_cpu)
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP
            | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    // No return stack buffer. Our caches are small (4 MiB to start), so they fill up in the middle of a
    // run, and the ARM64 backend then wipes the cache and writes new blocks over the old ones while
    // the return stack buffer (on the host stack of that run) still holds host addresses of the old
    // blocks. The next return the buffer predicted then jumped into whatever block now sits there.
    // Uncharted: Golden Abyss: a function return landed in the middle of another function (no
    // prologue, r8 still 0xDEADBEEF), its epilogue popped above the top of the stack and the game
    // ended up calling a float. Returns now go through the dispatcher, which looks the block up.
    config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
#endif
    config.enable_cycle_counting = false;
#if defined(VITA3K_PLATFORM_IOS)
    // From Tsubomi: Vita3K owns one Dynarmic JIT per guest thread. On iOS 26+ each cache also has a
    // same-sized writable vm_remap alias, so the 128 MiB default scales poorly. Threads start with a
    // tiny cache; one that fills it moves up a size (see promote_code_cache).
    if (code_cache_size == 0)
        code_cache_size = IOS_TINY_CODE_CACHE_SIZE;
#if defined(__aarch64__)
    // None of this size left in the prepared pool: the next size up rather than no thread at all
    if (code_cache_size == IOS_TINY_CODE_CACHE_SIZE && !ios_code_cache_available(IOS_TINY_CODE_CACHE_SIZE))
        code_cache_size = IOS_SMALL_CODE_CACHE_SIZE;
    if (code_cache_size == IOS_SMALL_CODE_CACHE_SIZE && !ios_code_cache_available(IOS_SMALL_CODE_CACHE_SIZE)
        && ios_code_cache_available(IOS_BIG_CODE_CACHE_SIZE))
        code_cache_size = IOS_BIG_CODE_CACHE_SIZE;
#endif
    config.code_cache_size = code_cache_size;
#endif

#if defined(VITA3K_PLATFORM_IOS) && defined(__aarch64__)
    // StikDebug services oaknut's BRK #0xf00d while the JIT constructor maps its cache. Serialize this
    // so simultaneous guest-thread creation cannot issue overlapping debugger requests.
    static std::mutex jit_creation_mutex;
    static uint64_t jit_creation_count = 0;
    const std::lock_guard lock(jit_creation_mutex);
    const uint64_t allocation_id = ++jit_creation_count;
    const bool log_it = allocation_id <= 16 || allocation_id % 256 == 0 || config.code_cache_size > IOS_TINY_CODE_CACHE_SIZE;
    if (log_it)
        LOG_INFO("iOS Dynarmic JIT cache #{} allocating: core={} size={} MiB", allocation_id, core_id, config.code_cache_size / (1024 * 1024));
    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    if (log_it)
        LOG_INFO("iOS Dynarmic JIT cache #{} ready", allocation_id);
    return jit;
#else
    return std::make_unique<Dynarmic::A32::Jit>(config);
#endif
}

DynarmicCPU::DynarmicCPU(CPUState *state, std::size_t processor_id, bool cpu_opt)
    : parent(state)
    , cb(std::make_unique<ArmDynarmicCallback>(*state, *this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , core_id(processor_id)
    , cpu_opt(cpu_opt) {
    jit = make_jit();
}

DynarmicCPU::~DynarmicCPU() = default;

CPUStop DynarmicCPU::run() {
    halted = false;
    break_ = false;
    svc_called = false;
#if defined(VITA3K_PLATFORM_IOS)
    {
        // A stop asked for before this run is taken by this run
        const std::lock_guard lock(jit_swap_mutex);
        halt_pending = false;
    }
#endif
    // Run until the JIT stops for something other than a step or a cache invalidation
    Dynarmic::HaltReason reason;
    do {
        reason = jit->Run();
    } while (reason == Dynarmic::HaltReason::Step || reason == Dynarmic::HaltReason::CacheInvalidation);
    // The import's NID is the word after the SVC, and a halted run stopped at the return address
    const uint32_t nid = svc_called ? *Ptr<uint32_t>(get_pc() + 4).get(*parent->mem) : 0;
#if defined(VITA3K_PLATFORM_IOS) && defined(__aarch64__)
    if (code_cache_size < IOS_BIG_CODE_CACHE_SIZE && !promotion_logged && jit->CodeCacheFullClears() > 0)
        promote_code_cache();
#endif
    return { .kernel_call = svc_called, .nid = nid, .returned = halted };
}

#if defined(VITA3K_PLATFORM_IOS)
// This thread filled its code cache: carry on in the next size up, with the same registers. Done between
// two runs (the thread is in Vita3K's own code here, not in guest code), under the lock stop() takes.
void DynarmicCPU::promote_code_cache() {
#if defined(__aarch64__)
    std::size_t next_size = IOS_BIG_CODE_CACHE_SIZE;
    if (code_cache_size < IOS_SMALL_CODE_CACHE_SIZE && ios_code_cache_available(IOS_SMALL_CODE_CACHE_SIZE))
        next_size = IOS_SMALL_CODE_CACHE_SIZE;
    if (!ios_code_cache_available(next_size)) {
        if (!promotion_logged)
            LOG_WARN("A thread keeps filling its {} MiB JIT cache, and no bigger cache is left: it recompiles when it fills",
                code_cache_size / (1024 * 1024));
        promotion_logged = true;
        return;
    }
    const auto regs = jit->Regs();
    const auto ext_regs = jit->ExtRegs();
    const uint32_t cpsr = jit->Cpsr();
    const uint32_t fpscr = jit->Fpscr();
    const std::size_t old_size = code_cache_size;
    code_cache_size = next_size;
    std::unique_ptr<Dynarmic::A32::Jit> bigger;
    try {
        bigger = make_jit();
    } catch (const std::exception &e) {
        code_cache_size = old_size;
        LOG_WARN("Could not move a thread to a bigger JIT cache: {}", e.what());
        promotion_logged = true;
        return;
    }
    bigger->Regs() = regs;
    bigger->ExtRegs() = ext_regs;
    bigger->SetCpsr(cpsr);
    bigger->SetFpscr(fpscr);
    {
        const std::lock_guard lock(jit_swap_mutex);
        jit.swap(bigger);
        // A stop that reached the old JIT after it returned must still stop the next run
        if (halt_pending)
            jit->HaltExecution();
    }
    LOG_INFO("A thread filled its {} MiB JIT cache: it now has {} MiB (no more full-cache recompiles)", old_size / (1024 * 1024),
        code_cache_size / (1024 * 1024));
    // `bigger` now holds the old JIT; its cache goes back to the pool here
#endif
}
#endif

CPUStop DynarmicCPU::step() {
    svc_called = false;
    jit->Step();
    const uint32_t nid = svc_called ? *Ptr<uint32_t>(get_pc() + 4).get(*parent->mem) : 0;
    return { .kernel_call = svc_called, .nid = nid };
}

bool DynarmicCPU::hit_breakpoint() {
    return break_;
}

void DynarmicCPU::trigger_breakpoint() {
    break_ = true;
    stop();
}

void DynarmicCPU::set_log_code(bool log) {
    if (log_code == log)
        return;

    log_code = log;
    jit = make_jit();
}

void DynarmicCPU::set_log_mem(bool log) {
    if (log_mem == log)
        return;

    log_mem = log;
    jit = make_jit();
}

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::stop() {
#if defined(VITA3K_PLATFORM_IOS)
    const std::lock_guard lock(jit_swap_mutex);
    halt_pending = true;
#endif
    jit->HaltExecution();
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    return jit->Regs()[idx];
}

uint32_t DynarmicCPU::get_sp() {
    return jit->Regs()[13];
}

uint32_t DynarmicCPU::get_pc() {
    return jit->Regs()[15];
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->tpidruro;
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->tpidruro = val;
}

uint32_t DynarmicCPU::get_tpidrurw() {
    return cp15->tpidrurw;
}

void DynarmicCPU::set_tpidrurw(uint32_t val) {
    cp15->tpidrurw = val;
}

void DynarmicCPU::set_pc(uint32_t val) {
    if (val & 1) {
        set_cpsr(get_cpsr() | 0x20);
        val = val & 0xFFFFFFFE;
    } else {
        set_cpsr(get_cpsr() & 0xFFFFFFDF);
        val = val & 0xFFFFFFFC;
    }
    jit->Regs()[15] = val;
}

void DynarmicCPU::set_lr(uint32_t val) {
    jit->Regs()[14] = val;
}

void DynarmicCPU::set_sp(uint32_t val) {
    jit->Regs()[13] = val;
}

uint32_t DynarmicCPU::get_cpsr() {
    return jit->Cpsr();
}

uint32_t DynarmicCPU::get_fpscr() {
    return jit->Fpscr();
}

void DynarmicCPU::set_fpscr(uint32_t val) {
    jit->SetFpscr(val);
}

CPUContext DynarmicCPU::save_context() {
    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    ctx.fpu_registers = std::bit_cast<std::array<float, 64>>(jit->ExtRegs());
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();
    return ctx;
}

void DynarmicCPU::load_context(const CPUContext &ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->ExtRegs() = std::bit_cast<std::array<uint32_t, 64>>(ctx.fpu_registers);
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
}

uint32_t DynarmicCPU::get_lr() {
    return jit->Regs()[14];
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

bool DynarmicCPU::is_thumb_mode() {
    return jit->Cpsr() & 0x20;
}

std::size_t DynarmicCPU::processor_id() const {
    return core_id;
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    jit->InvalidateCacheRange(start, length);
}

void DynarmicCPU::reset_for_reuse() {
#if defined(VITA3K_PLATFORM_IOS)
    const std::lock_guard lock(jit_swap_mutex);
    halt_pending = false;
#endif
    // Every halt reason but a pending cache invalidation: take_pooled_cpu asks for one when guest code
    // changed since this JIT last ran, and clearing it too threw the request away, so a reused JIT ran
    // blocks compiled for code that was no longer there (Danganronpa 2: a thread of the second video
    // player ran the first player's unloaded code, then the app crashed)
    jit->ClearHalt(static_cast<Dynarmic::HaltReason>(0xFFFFFFFFu & ~static_cast<uint32_t>(Dynarmic::HaltReason::CacheInvalidation)));
    halted = false;
    break_ = false;
    svc_called = false;
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(core_id);
}
