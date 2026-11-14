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

// RAM helpers, Vita3K's side: starts the helper extension's processes, takes memory from them and
// maps it over the guest RAM. See protocol.h.

#import "ram_helpers.h"
#import "protocol.h"

#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach/mach.h>
#import <objc/runtime.h>
#import <os/proc.h>
#import <sys/mman.h>
#import <xpc/xpc.h>

#include <fmt/format.h>
#include <util/address_space.h>

// util/log.h can't be included next to Foundation (MacTypes' Ptr): logging goes through log.cpp
extern "C" void v3k_ram_helpers_log(bool warning, const char *message);
#define RAM_LOG_INFO(...) v3k_ram_helpers_log(false, fmt::format(__VA_ARGS__).c_str())
#define RAM_LOG_WARN(...) v3k_ram_helpers_log(true, fmt::format(__VA_ARGS__).c_str())

#include <algorithm>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The xpc endpoint travels inside an NSXPCListenerEndpoint (the only endpoint NSExtensionItem can
// carry); its private _endpoint property holds the raw xpc object
@interface NSXPCListenerEndpoint (V3KRamHelper)
@property (nonatomic, setter=_setEndpoint:) xpc_object_t _endpoint;
@end

static NSXPCListenerEndpoint *v3k_wrap_endpoint(xpc_endpoint_t endpoint) {
    NSXPCListenerEndpoint *wrapper = [NSXPCListenerEndpoint new];
    wrapper._endpoint = endpoint;
    return wrapper;
}

// Private NSExtension API (Foundation/ExtensionFoundation): how an app starts its own extensions. Looked up
// at run time (LiveContainer hooks the same class methods to run the extension inside it)
@protocol V3KExtensionAPI <NSObject>
@property (nonatomic, strong, readwrite) NSArray *preferredLanguages;
+ (id<V3KExtensionAPI>)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
- (void)beginExtensionRequestWithInputItems:(NSArray *)items completion:(void (^)(NSUUID *))callback;
- (void)_kill:(int)signal;
- (void)setRequestCancellationBlock:(void (^)(NSUUID *uuid, NSError *error))callback;
@end

extern "C" kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);
extern "C" kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);
extern "C" kern_return_t mach_vm_remap(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
    vm_map_t source, mach_vm_address_t source_address, boolean_t copy, vm_prot_t *current, vm_prot_t *maximum, vm_inherit_t inheritance);
extern "C" kern_return_t mach_vm_region_recurse(vm_map_read_t target, mach_vm_address_t *address, mach_vm_size_t *size, natural_t *depth,
    vm_region_recurse_info_t info, mach_msg_type_number_t *count);

#ifndef VM_FLAGS_OVERWRITE
#define VM_FLAGS_OVERWRITE 0x4000
#endif

namespace {

constexpr uint64_t MiB = 1024ull * 1024ull;
// What a helper keeps for itself: its own code and iOS's bookkeeping, so it isn't killed for lending
constexpr uint64_t HELPER_HEADROOM = 256 * MiB;
constexpr double START_TIMEOUT = 8.0;

// The JIT debugger freezes the whole app while it prepares memory; a helper lookup made during that
// times out. Wait for it to finish first (weak: not every build of the app has JIT setup)
extern "C" bool v3k_jit_preparing(void) __attribute__((weak));
// True while a debugger is attached and JIT memory is about to be prepared (the freeze is imminent)
extern "C" bool v3k_jit_can_request(void) __attribute__((weak));

namespace {
bool jit_freeze_coming() {
    return (v3k_jit_preparing && v3k_jit_preparing()) || (v3k_jit_can_request && v3k_jit_can_request());
}
// At most 30 s of waiting in all for the whole run of the app: if JIT setup never finishes (it failed),
// helpers and game boots must not keep waiting for it
std::atomic<int> g_jit_wait_budget{ 300 }; // tenths of a second
void wait_out_jit_setup(int tenths) {
    for (int waited = 0; waited < tenths && g_jit_wait_budget > 0 && jit_freeze_coming(); waited++) {
        g_jit_wait_budget--;
        [NSThread sleepForTimeInterval:0.1];
    }
}
} // namespace

struct Helper {
    xpc_connection_t connection = nil;
    uint64_t remaining = 0; // what it can still lend
    bool budget_known = false;
    int64_t pid = 0; // from its hello
    uint64_t hello_available = 0;
    int index = 0; // 1, 2, ... in the order they connected (for the log)
};

struct Backed {
    mach_vm_address_t base;
    uint64_t size;
};

std::recursive_mutex g_lock;
std::mutex g_start_lock; // one helper is started at a time (prepare and a boot can both ask)
int g_helper_serial = 0;
xpc_connection_t g_listener = nil;
dispatch_semaphore_t g_connected = nil;
NSMutableArray *g_extensions = nil; // NSExtension, one per helper process
std::vector<std::shared_ptr<Helper>> g_helpers;
std::vector<Backed> g_backed;
NSMutableArray *g_shmems = nil; // keeps the shared memory objects of this boot
std::string g_status = "Not used yet";
// Only a missing extension (or no NSExtension at all) stops the helpers for good. Everything else
// (the container answering slowly, the app frozen by the JIT debugger while iOS looked the helper up)
// is retried: one failed start used to turn the helpers off until the app was restarted, which is why
// they "worked before and then didn't"
std::atomic<bool> g_permanent_failure{ false };
std::atomic<int> g_failures{ 0 };
std::atomic<double> g_retry_after{ 0.0 };
constexpr int MAX_BACKGROUND_RETRIES = 6;

double now_seconds() {
    return NSDate.timeIntervalSinceReferenceDate;
}

bool start_blocked() {
    return g_permanent_failure || now_seconds() < g_retry_after.load();
}

void transient_failure() {
    const int failures = ++g_failures;
    // 2, 4, 8, 16, 30 s
    g_retry_after = now_seconds() + std::min(30.0, static_cast<double>(1 << std::min(failures, 5)));
}
bool g_new_boot = true; // the next range offered starts a new boot
bool g_lending = false; // the live helpers lent memory to the running game (they stay alive until it ends)
size_t g_next = 0;

void set_status(const std::string &status, bool error = false) {
    {
        std::lock_guard lock(g_lock);
        g_status = status;
    }
    if (error)
        RAM_LOG_WARN("{}", status);
    else
        RAM_LOG_INFO("{}", status);
}

int wanted_helpers();

NSString *helper_identifier() {
    static NSString *identifier;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSArray<NSURL *> *contents = [NSFileManager.defaultManager contentsOfDirectoryAtURL:NSBundle.mainBundle.builtInPlugInsURL
                                                                 includingPropertiesForKeys:nil
                                                                                    options:0
                                                                                      error:nil];
        for (NSURL *url in contents) {
            if (![url.pathExtension isEqualToString:@"appex"])
                continue;
            NSBundle *bundle = [NSBundle bundleWithURL:url];
            if ([bundle.infoDictionary[@"NSExtension"][@"NSExtensionPrincipalClass"] isEqualToString:@V3K_RAM_HELPER_PRINCIPAL])
                identifier = bundle.bundleIdentifier;
        }
    });
    return identifier;
}

void forget(xpc_connection_t connection) {
    std::lock_guard lock(g_lock);
    g_helpers.erase(std::remove_if(g_helpers.begin(), g_helpers.end(), [&](const std::shared_ptr<Helper> &h) { return h->connection == connection; }), g_helpers.end());
}

void ensure_listener() {
    std::lock_guard lock(g_lock);
    if (g_listener)
        return;
    g_connected = dispatch_semaphore_create(0);
    g_extensions = [NSMutableArray array];
    g_shmems = [NSMutableArray array];
    g_listener = xpc_connection_create(NULL, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    xpc_connection_set_event_handler(g_listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t connection = (xpc_connection_t)event;
        xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            if (xpc_get_type(message) == XPC_TYPE_ERROR) {
                RAM_LOG_INFO("a helper's connection ended");
                forget(connection);
            } else if (xpc_get_type(message) == XPC_TYPE_DICTIONARY) {
                const char *op = xpc_dictionary_get_string(message, V3K_RAM_KEY_OP);
                if (op && strcmp(op, V3K_RAM_OP_HELLO) == 0) {
                    std::lock_guard inner(g_lock);
                    for (const auto &h : g_helpers) {
                        if (h->connection == connection) {
                            h->pid = xpc_dictionary_get_int64(message, "pid");
                            h->hello_available = xpc_dictionary_get_uint64(message, V3K_RAM_KEY_AVAILABLE);
                            RAM_LOG_INFO("helper {} says hello: process {}, {} MiB available to it", h->index, h->pid, h->hello_available / MiB);
                        }
                    }
                }
            }
        });
        xpc_connection_resume(connection);
        {
            std::lock_guard inner(g_lock);
            auto helper = std::make_shared<Helper>();
            helper->connection = connection;
            helper->index = ++g_helper_serial;
            RAM_LOG_INFO("helper {} connected", helper->index);
            g_helpers.push_back(helper);
        }
        dispatch_semaphore_signal(g_connected);
    });
    xpc_connection_resume(g_listener);
}

size_t live_count() {
    std::lock_guard lock(g_lock);
    return g_helpers.size();
}

// Starts one more helper process and waits until it connects
bool start_helper() {
    std::lock_guard<std::mutex> start_guard(g_start_lock);
    if (start_blocked())
        return false;
    if (const int setup = v3k_ram_helpers_container_setup(); setup == V3K_RAM_SETUP_SEPARATE_PROCESS || setup == V3K_RAM_SETUP_OPTION_OFF || setup == V3K_RAM_SETUP_NEEDS_REINSTALL) {
        // Asking iOS can't work here: say what to change instead of timing out over and over
        set_status(v3k_ram_helpers_container_hint(), true);
        g_permanent_failure = true;
        return false;
    }
    wait_out_jit_setup(200);
    const double started_at = NSDate.timeIntervalSinceReferenceDate;
    NSString *identifier = helper_identifier();
    if (!identifier) {
        set_status("The RAM helper extension is not in this copy of the app (the signer may have removed it)", true);
        g_permanent_failure = true;
        return false;
    }
    RAM_LOG_INFO("starting a RAM helper ({}{})", identifier.UTF8String, v3k_ram_helpers_in_livecontainer() ? ", inside LiveContainer" : "");
    ensure_listener();

    Class extension_class = NSClassFromString(@"NSExtension");
    if (!extension_class) {
        dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_LAZY | RTLD_GLOBAL);
        extension_class = NSClassFromString(@"NSExtension");
    }
    if (!extension_class) {
        set_status("This iOS version has no way to start the RAM helper", true);
        g_permanent_failure = true;
        return false;
    }
    NSError *error = nil;
    id<V3KExtensionAPI> extension = nil;
    // LiveContainer answers this through its own hooks, which can take a moment to be ready at launch: try a few times
    for (int attempt = 0; attempt < 6 && !extension; attempt++) {
        if (attempt)
            [NSThread sleepForTimeInterval:0.75];
        wait_out_jit_setup(100);
        error = nil;
        extension = [(Class<V3KExtensionAPI>)extension_class extensionWithIdentifier:identifier error:&error];
    }
    if (!extension) {
        RAM_LOG_WARN("extensionWithIdentifier failed: {} (domain {}, code {})", error.localizedDescription.UTF8String ?: "no error given",
            error.domain.UTF8String ?: "none", static_cast<long>(error.code));
        transient_failure();
        if (v3k_ram_helpers_in_livecontainer())
            set_status(fmt::format("Your app container (LiveContainer, Strata, ...) did not start the RAM helper this time (try {}); trying again in a moment. If it never works, the container needs app extension support (LiveContainer 3.8.13 or newer)", g_failures.load()), true);
        else
            set_status(std::string("iOS could not find the RAM helper (") + (error.localizedDescription.UTF8String ?: "no reason given")
                    + "); trying again in a moment. If it never works, reinstall Hinoki without removing app extensions (SideStore/AltStore can strip them)",
                true);
        return false;
    }
    extension.preferredLanguages = @[];
    // A cancelled request is a failed start: stop waiting at once (the boot used to wait out the whole
    // timeout, ~6 s, after the container had already said no)
    __block volatile bool cancelled = false;
    __block bool needs_reinstall = false;
    [extension setRequestCancellationBlock:^(NSUUID *, NSError *cancel_error) {
        const char *reason = cancel_error.localizedDescription.UTF8String ?: "";
        RAM_LOG_WARN("a helper's request was cancelled ({})", reason);
        if (strstr(reason, "cannot dlopen a main executable"))
            needs_reinstall = true;
        cancelled = true;
        dispatch_semaphore_signal(g_connected);
    }];

    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = @{ @"endpoint" : v3k_wrap_endpoint(xpc_endpoint_create(g_listener)) };

    const size_t before = live_count();
    __block bool started = false;
    dispatch_semaphore_t spawned = dispatch_semaphore_create(0);
    [extension beginExtensionRequestWithInputItems:@[ item ]
                                        completion:^(NSUUID *request) {
                                            started = request != nil;
                                            dispatch_semaphore_signal(spawned);
                                        }];
    dispatch_semaphore_wait(spawned, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(START_TIMEOUT * NSEC_PER_SEC)));
    if (!started) {
        set_status("iOS did not start a RAM helper process; trying again in a moment", true);
        transient_failure();
        return false;
    }

    const double deadline = NSDate.timeIntervalSinceReferenceDate + START_TIMEOUT;
    while (live_count() <= before) {
        if (cancelled) {
            [extension _kill:SIGKILL];
            if (needs_reinstall) {
                // The container hasn't prepared the helper (apps imported before it supported helpers)
                set_status(v3k_ram_helpers_reinstall_hint(), true);
                g_permanent_failure = true;
            } else {
                set_status("The container stopped the RAM helper as it started; trying again in a moment", true);
                transient_failure();
            }
            return false;
        }
        const double left = deadline - NSDate.timeIntervalSinceReferenceDate;
        if (left <= 0) {
            set_status("A RAM helper started but never connected; trying again in a moment", true);
            [extension _kill:SIGKILL];
            transient_failure();
            return false;
        }
        dispatch_semaphore_wait(g_connected, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(left * NSEC_PER_SEC)));
    }
    g_failures = 0;
    g_retry_after = 0.0;
    std::lock_guard lock(g_lock);
    [g_extensions addObject:extension];
    RAM_LOG_INFO("helper process started in {} ms ({} of {} wanted)", static_cast<int>((NSDate.timeIntervalSinceReferenceDate - started_at) * 1000), g_helpers.size(), wanted_helpers());
    return true;
}

xpc_object_t send(xpc_connection_t connection, const char *op, uint64_t size, double timeout) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(request, V3K_RAM_KEY_OP, op);
    if (size)
        xpc_dictionary_set_uint64(request, V3K_RAM_KEY_SIZE, size);
    __block xpc_object_t answer = nil;
    dispatch_semaphore_t replied = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(connection, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
            answer = reply;
        dispatch_semaphore_signal(replied);
    });
    if (dispatch_semaphore_wait(replied, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout * NSEC_PER_SEC))) != 0)
        return nil;
    return answer;
}

int wanted_helpers() {
    NSInteger count = [NSUserDefaults.standardUserDefaults integerForKey:@"v3k.ramHelpers.count"];
    if (count <= 0)
        count = 2;
    return static_cast<int>(std::clamp<NSInteger>(count, 1, 4));
}

// A helper with room for `length` bytes, starting more helpers (up to the setting) when all are full
std::shared_ptr<Helper> helper_with_room(uint64_t length) {
    for (;;) {
        {
            std::lock_guard lock(g_lock);
            for (size_t i = 0; i < g_helpers.size(); i++) {
                const std::shared_ptr<Helper> shared = g_helpers[(g_next + i) % g_helpers.size()];
                Helper &helper = *shared;
                if (!helper.budget_known) {
                    xpc_object_t reply = send(helper.connection, V3K_RAM_OP_AVAILABLE, 0, 2.0);
                    const uint64_t available = reply ? xpc_dictionary_get_uint64(reply, V3K_RAM_KEY_AVAILABLE) : 0;
                    helper.remaining = available > HELPER_HEADROOM ? available - HELPER_HEADROOM : 0;
                    helper.budget_known = true;
                    RAM_LOG_INFO("helper {} can lend {} MiB", helper.index, helper.remaining / MiB);
                }
                if (helper.remaining >= length) {
                    g_next = (g_next + i + 1) % g_helpers.size();
                    return shared;
                }
            }
        }
        if (start_blocked() || live_count() >= static_cast<size_t>(wanted_helpers()) || !start_helper())
            return nullptr;
    }
}

bool map_chunk(Helper &helper, mach_vm_address_t target, uint64_t length) {
    xpc_object_t reply = send(helper.connection, V3K_RAM_OP_CREATE, length, 20.0);
    if (!reply) {
        set_status("A RAM helper didn't answer", true);
        helper.remaining = 0;
        return false;
    }
    if (const char *error = xpc_dictionary_get_string(reply, V3K_RAM_KEY_ERROR)) {
        set_status(std::string("A RAM helper couldn't lend memory: ") + error, true);
        helper.remaining = 0;
        return false;
    }
    xpc_object_t shmem = xpc_dictionary_get_value(reply, V3K_RAM_KEY_SHMEM);
    if (!shmem || xpc_get_type(shmem) != XPC_TYPE_SHMEM) {
        set_status("A RAM helper's answer had no memory in it", true);
        return false;
    }
    void *scratch = nullptr;
    const size_t mapped = xpc_shmem_map(shmem, &scratch);
    if (!scratch || mapped < length) {
        if (scratch)
            mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(scratch), mapped);
        set_status("The memory a RAM helper lent could not be mapped", true);
        return false;
    }
    // Move it over the guest range (replacing the reservation there)
    vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
    mach_vm_address_t destination = target;
    const kern_return_t kr = mach_vm_remap(mach_task_self(), &destination, length, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        mach_task_self(), reinterpret_cast<mach_vm_address_t>(scratch), false, &current, &maximum, VM_INHERIT_NONE);
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(scratch), mapped);
    if (kr != KERN_SUCCESS || destination != target) {
        set_status(std::string("Lent memory could not be placed in the guest range: ") + mach_error_string(kr), true);
        return false;
    }
    // Prove the lent memory works before the game relies on it: write and read back a pattern at both
    // ends and the middle, through the guest address (the pattern is cleared again)
    {
        const uint64_t spots[] = { 0, length / 2 & ~(vm_page_size - 1), length - vm_page_size };
        bool good = mprotect(reinterpret_cast<void *>(target), length, PROT_READ | PROT_WRITE) == 0;
        for (size_t i = 0; good && i < 3; i++) {
            auto *word = reinterpret_cast<volatile uint64_t *>(target + spots[i]);
            const uint64_t pattern = 0x5633'4b52'414d'0000ull + i;
            *word = pattern;
            good = *word == pattern;
            *word = 0;
        }
        if (!good) {
            RAM_LOG_WARN("lent memory at {:#x} failed its write/read check", target);
            set_status("The memory a RAM helper lent could not be used", true);
            return false;
        }
    }
    // The guest allocator expects untouched pages to be inaccessible until it hands them out
    if (mprotect(reinterpret_cast<void *>(target), length, PROT_NONE) != 0) {
        set_status("Lent memory could not be protected", true);
        return false;
    }
    std::lock_guard lock(g_lock);
    [g_shmems addObject:shmem];
    helper.remaining -= length;
    RAM_LOG_INFO("helper {} lent {} MiB at {:#x} (checked; it can lend {} MiB more)", helper.index, length / MiB, target, helper.remaining / MiB);
    return true;
}

// Ends the helper processes of this boot. Their memory stays mapped here: once its owner is gone iOS
// charges it to no process, and nothing can be killed to take it back (MeloNX found this the most
// stable way to run)
void end_helpers() {
    NSArray *extensions;
    {
        std::lock_guard lock(g_lock);
        extensions = [g_extensions copy];
        [g_extensions removeAllObjects];
        g_helpers.clear();
        g_next = 0;
    }
    for (id<V3KExtensionAPI> extension in extensions)
        [extension _kill:SIGKILL];
}

uint64_t touched_bytes(mach_vm_address_t start, uint64_t length) {
    const mach_vm_address_t end = start + length;
    mach_vm_address_t address = start;
    natural_t depth = 0;
    uint64_t pages = 0;
    while (address < end) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS || address >= end)
            break;
        if (info.is_submap) {
            depth++;
            continue;
        }
        const mach_vm_address_t region_end = std::min<mach_vm_address_t>(address + size, end);
        const uint64_t region_pages = (region_end - address) / vm_page_size;
        pages += std::min<uint64_t>(info.pages_dirtied + info.pages_swapped_out, region_pages);
        address += size;
    }
    return pages * vm_page_size;
}

} // namespace

extern "C" {

bool v3k_ram_helpers_enabled(void) {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"v3k.ramHelpers"];
}

bool v3k_ram_helpers_bundled(void) {
    return helper_identifier() != nil;
}

bool v3k_ram_helpers_in_livecontainer(void) {
    return getenv("LC_HOME_PATH") != nullptr || [NSBundle.mainBundle.bundlePath containsString:@"LiveContainer"];
}

// Inside a container app the helper is not installed with iOS: the container must start it for us.
// LiveContainer (nightly 9d6ea09, 8 Nov 2026) and Strata 4.0 do that through hooks on NSExtension,
// installed only when the app's "Helper Process Support" option is on AND the app runs in the
// container's own process (not in a separate LiveProcess: Strata's seamless switching and its
// full-screen built-in JIT launch apps there). Without the hooks iOS is asked for an extension it never
// installed and gives up after ~4 s without an error, every time.
static bool helper_needs_reinstall();

int v3k_ram_helpers_container_setup(void) {
    static const int state = [] {
        if (!v3k_ram_helpers_in_livecontainer())
            return V3K_RAM_SETUP_STANDALONE;
        // The real executable (image 0) is LiveProcess.appex/LiveProcess in a separate process
        const char *main_image = _dyld_get_image_name(0);
        const bool live_process = main_image && strstr(main_image, ".appex/") != nullptr;
        // Hooked: the class method's code is the container's, not iOS's
        bool hooked = false;
        if (!NSClassFromString(@"NSExtension"))
            dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_LAZY | RTLD_GLOBAL);
        if (Class cls = NSClassFromString(@"NSExtension")) {
            if (Method m = class_getClassMethod(cls, @selector(extensionWithIdentifier:error:))) {
                Dl_info info{};
                if (dladdr(reinterpret_cast<const void *>(method_getImplementation(m)), &info) && info.dli_fname)
                    hooked = strncmp(info.dli_fname, "/System/", 8) != 0 && strncmp(info.dli_fname, "/usr/lib/", 9) != 0;
            }
        }
        RAM_LOG_INFO("container: {} process ({}), helper hooks {}", live_process ? "separate (LiveProcess)" : "the container's own",
            main_image ? main_image : "?", hooked ? "installed" : "NOT installed");
        if (hooked) {
            if (helper_needs_reinstall()) {
                RAM_LOG_WARN("container: the helper was not prepared when Hinoki was added to the container");
                return V3K_RAM_SETUP_NEEDS_REINSTALL;
            }
            return V3K_RAM_SETUP_CONTAINER_READY;
        }
        return live_process ? V3K_RAM_SETUP_SEPARATE_PROCESS : V3K_RAM_SETUP_OPTION_OFF;
    }();
    return state;
}

const char *v3k_ram_helpers_reinstall_hint(void) {
    return "Your container hasn't prepared Hinoki's RAM helper: Hinoki was added to it before the container supported helpers "
           "(for example under Strata 3.0). Install Hinoki into the container again (as an update, keeping its data), then launch it again.";
}

// Inside a container the helper's program must have been converted to a library when the app was
// added (Strata 4.0 / LiveContainer do it for apps they import from then on); a helper still built
// as a program can't be loaded ("cannot dlopen a main executable")
static bool helper_needs_reinstall() {
    NSString *plugins = NSBundle.mainBundle.builtInPlugInsPath;
    if (!plugins)
        return false;
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:plugins error:nil]) {
        if (![name.pathExtension isEqualToString:@"appex"])
            continue;
        NSString *bundle_path = [plugins stringByAppendingPathComponent:name];
        NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:[bundle_path stringByAppendingPathComponent:@"Info.plist"]];
        if (![plist[@"CFBundleIdentifier"] hasSuffix:@"RamHelper"])
            continue;
        NSString *executable = plist[@"CFBundleExecutable"];
        if (!executable)
            return false;
        NSFileHandle *file = [NSFileHandle fileHandleForReadingAtPath:[bundle_path stringByAppendingPathComponent:executable]];
        NSData *head = [file readDataOfLength:4096];
        [file closeFile];
        if (head.length < 16)
            return false;
        const uint8_t *bytes = static_cast<const uint8_t *>(head.bytes);
        uint32_t magic;
        memcpy(&magic, bytes, 4);
        size_t offset = 0;
        if (magic == 0xBEBAFECA) { // FAT_CIGAM: big-endian fat header, first arch's offset
            if (head.length < 8 + 20)
                return false;
            uint32_t be;
            memcpy(&be, bytes + 8 + 8, 4);
            offset = __builtin_bswap32(be);
            if (offset + 16 > head.length) {
                file = [NSFileHandle fileHandleForReadingAtPath:[bundle_path stringByAppendingPathComponent:executable]];
                [file seekToFileOffset:offset];
                head = [file readDataOfLength:16];
                [file closeFile];
                if (head.length < 16)
                    return false;
                bytes = static_cast<const uint8_t *>(head.bytes);
                offset = 0;
            }
        }
        uint32_t filetype;
        memcpy(&magic, bytes + offset, 4);
        memcpy(&filetype, bytes + offset + 12, 4);
        return magic == 0xFEEDFACF && filetype == 0x2; // MH_MAGIC_64, MH_EXECUTE: not converted
    }
    return false;
}

const char *v3k_ram_helpers_container_hint(void) {
    switch (v3k_ram_helpers_container_setup()) {
    case V3K_RAM_SETUP_NEEDS_REINSTALL:
        return v3k_ram_helpers_reinstall_hint();
    case V3K_RAM_SETUP_SEPARATE_PROCESS:
        return "Hinoki is running in a separate process (LiveProcess), where your container can't start RAM helpers. "
               "In Strata: turn on HostJIT-InProcess for Hinoki (Runtime Center > HostJIT) or turn off Seamless app switching, "
               "and turn on Helper Process Support in Hinoki's app settings. Then launch Hinoki again.";
    case V3K_RAM_SETUP_OPTION_OFF:
        return "Your container isn't starting app helpers for Hinoki. Turn on Helper Process Support in Hinoki's app settings "
               "(Strata 4.0 or newer, or a LiveContainer build from 8 Nov 2026 or newer), then launch Hinoki again.";
    default:
        return "";
    }
}

uint64_t v3k_ram_helpers_back(void *host, uint64_t size, bool last) {
    if (!v3k_ram_helpers_enabled() || !host || size == 0)
        return 0;
    {
        std::lock_guard lock(g_lock);
        // A new boot: the previous one's memory went away with its guest range
        if (g_new_boot) {
            g_new_boot = false;
            g_backed.clear();
            [g_shmems removeAllObjects];
        }
    }
    // The previous game's lenders (if v3k_ram_helpers_game_ended wasn't called) hold memory nobody uses now
    if (g_lending) {
        g_lending = false;
        end_helpers();
    }
    if (start_blocked()) {
        // Failed moments ago (or can't work here): don't make this boot wait; the next one tries again
        if (last)
            g_new_boot = true;
        return 0;
    }

    const mach_vm_address_t base = reinterpret_cast<mach_vm_address_t>(host);
    RAM_LOG_INFO("guest range {:#x}, {} MiB offered to the helpers{}", base, size / MiB, last ? " (last range)" : "");
    uint64_t offset = 0;
    while (offset < size) {
        const uint64_t length = std::min<uint64_t>(V3K_RAM_HELPER_CHUNK, size - offset);
        const std::shared_ptr<Helper> helper = helper_with_room(length);
        if (!helper || !map_chunk(*helper, base + offset, length))
            break;
        offset += length;
    }
    if (offset) {
        std::lock_guard lock(g_lock);
        g_backed.push_back({ base, offset });
    }
    if (offset < size)
        RAM_LOG_WARN("range {:#x}: {} of {} MiB backed by helpers, the rest stays in this app's own memory", base, offset / MiB, size / MiB);

    if (last) {
        g_new_boot = true;
        uint64_t total = 0;
        {
            std::lock_guard lock(g_lock);
            for (const Backed &backed : g_backed)
                total += backed.size;
        }
        const size_t helpers = live_count();
        // The lenders stay alive while the game runs: the memory stays theirs (charged to them, not to Vita3K)
        // for the whole session. Ending them right away (what earlier builds did) left the memory without
        // an owner, and pages the game touched later could be counted against Vita3K again
        g_lending = total > 0;
        if (!g_lending)
            end_helpers();
        RAM_LOG_INFO("this app: {} MiB footprint, {} MiB still available to it (the lent memory is not counted here)", v3k_memory_footprint() / MiB, v3k_memory_available() / MiB);
        if (total)
            set_status(fmt::format("{} MiB of the PS Vita's memory lent by {} helper process{}", total / MiB, helpers, helpers == 1 ? "" : "es"));
        else if (!start_blocked())
            set_status("The helpers had no memory to lend", true);
    }
    return offset;
}

void v3k_ram_helpers_game_ended(void) {
    if (!g_lending)
        return;
    g_lending = false;
    {
        std::lock_guard lock(g_lock);
        g_backed.clear();
        [g_shmems removeAllObjects];
    }
    end_helpers();
    RAM_LOG_INFO("game ended: lenders released, their memory is free again");
    // Ready for the next boot
    v3k_ram_helpers_prepare();
}

void v3k_ram_helpers_prepare(void) {
    if (!v3k_ram_helpers_enabled() || g_permanent_failure || !v3k_ram_helpers_bundled())
        return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        while (!start_blocked() && live_count() < static_cast<size_t>(wanted_helpers())) {
            if (!start_helper())
                break;
        }
        // A start that failed is tried again by itself (a few times, further apart each time), so the
        // helpers are ready by the time a game boots
        if (!g_permanent_failure && g_failures > 0 && g_failures <= MAX_BACKGROUND_RETRIES
            && live_count() < static_cast<size_t>(wanted_helpers())) {
            const double delay = std::max(0.5, g_retry_after.load() - now_seconds());
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                v3k_ram_helpers_prepare();
            });
        }
    });
}

uint32_t v3k_ram_helpers_live(void) {
    return static_cast<uint32_t>(live_count());
}

uint64_t v3k_ram_helpers_backed(void) {
    std::lock_guard lock(g_lock);
    uint64_t total = 0;
    for (const Backed &backed : g_backed)
        total += backed.size;
    return total;
}

uint64_t v3k_ram_helpers_in_use(void) {
    std::vector<Backed> backed;
    {
        std::lock_guard lock(g_lock);
        backed = g_backed;
    }
    uint64_t total = 0;
    for (const Backed &range : backed)
        total += touched_bytes(range.base, range.size);
    return total;
}

uint64_t v3k_ram_helpers_probe(void) {
    if (!v3k_ram_helpers_bundled())
        return 0;
    // The user asked: try again
    g_permanent_failure = false;
    g_failures = 0;
    g_retry_after = 0.0;
    if (live_count() == 0 && !start_helper())
        return 0;
    xpc_connection_t connection;
    {
        std::lock_guard lock(g_lock);
        if (g_helpers.empty())
            return 0;
        connection = g_helpers.front()->connection;
    }
    xpc_object_t reply = send(connection, V3K_RAM_OP_AVAILABLE, 0, 3.0);
    const uint64_t available = reply ? xpc_dictionary_get_uint64(reply, V3K_RAM_KEY_AVAILABLE) : 0;
    set_status(available ? fmt::format("A helper is running and can lend about {} MiB", (available > HELPER_HEADROOM ? available - HELPER_HEADROOM : 0) / MiB)
                         : std::string("A helper started but didn't say how much it can lend"),
        available == 0);
    return available > HELPER_HEADROOM ? available - HELPER_HEADROOM : 0;
}

const char *v3k_ram_helpers_status(void) {
    static thread_local std::string copy;
    std::lock_guard lock(g_lock);
    copy = g_status;
    return copy.c_str();
}

uint64_t v3k_memory_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

uint64_t v3k_memory_available(void) {
    return os_proc_available_memory();
}

uint64_t v3k_memory_physical(void) {
    return NSProcessInfo.processInfo.physicalMemory;
}

bool v3k_extended_address_space(void) {
    return address_space::extended();
}

uint64_t v3k_largest_free_address_mib(void) {
    return address_space::largest_free_block_mib();
}

} // extern "C"
