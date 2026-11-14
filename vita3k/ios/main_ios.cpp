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

// iOS entry point. SDL owns the UIApplication; this function runs once UIKit has launched.
//
//   1. Initialise the emulator under the app's Documents folder (visible in the Files app, and
//      redirected to the container when running inside LiveContainer).
//   2. Install anything dropped into Documents/Import.
//   3. Loop: show the SwiftUI library until a game is picked, then run it through the shared
//      SDL frontend (the same runner Android uses). When the game exits, the library comes back.

#include "bridge/vita3k_bridge.h"
#include "cheats.h"
#include "controls/virtual_pad.h"
#include "crash_report.h"
#include "game_extras.h"
#include "ios_platform.h"
#include "ios_session.h"
#include "jit/ios_jit.h"
#include "ramhelper/ram_helpers.h"

#include <app/session_controller.h>
#include <audio/state.h>
#include <cpu/functions.h>
#include <display/state.h>
#include <emuenv/app_launch_request.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <renderer/vulkan/device_lost.h>
#include <sdl-frontend/session.h>
#include <util/address_space.h>
#include <util/gpu_memory.h>
#include <util/log.h>
#include <util/presentation.h>
#include <util/recent_imports.h>
#include <util/thermal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

// Implemented in Swift (ui/TouchControls.swift)
// The running session, for the pause button and the game menu (main thread only)
namespace {
std::atomic<app::AppSessionController *> g_session{ nullptr };
std::atomic<bool> g_user_paused{ false };
} // namespace

extern "C" void v3k_set_game_paused(int reason, bool paused) {
    app::AppSessionController *session = g_session.load();
    if (!session)
        return;
    const auto pause_reason = reason == 1 ? app::AppSessionPauseReason::User : app::AppSessionPauseReason::Menu;
    if (session->set_pause_reason(pause_reason, paused)) {
        if (reason == 1) {
            g_user_paused = paused;
            LOG_INFO("Game {}", paused ? "paused" : "resumed");
            // Paused and put down: let the screen dim and lock as usual instead of burning the battery
            // with the screen on (the game stays paused through the lock)
            ios::set_screen_always_on(!paused);
        }
    }
}

// The app left the screen or came back (ios_platform.mm, main thread): the game pauses at once,
// before iOS starts refusing GPU work. SDL's background event does the same a little later.
// The game's audio device is closed while off screen (so the audio session can be handed back to
// other apps) and reopened on return. Opening and closing it waits on iOS's audio system, which can
// take seconds while the app is changing state: done on the main thread, leaving again right after
// coming back (iPhone16,1, iOS 27) blocked the main thread inside the switch, iOS ended the app for
// not finishing its move to the background, and the player found it closed. It now runs on its own
// queue, and each run applies the latest wanted state, so quick back-and-forth settles on the last one.
static std::atomic<bool> g_audio_output_wanted{ true };
static std::atomic<bool> g_audio_output_allowed{ false }; // a game's audio exists (between launch and stop)
static std::mutex g_audio_output_mutex;

static dispatch_queue_t audio_output_queue() {
    static dispatch_queue_t queue = dispatch_queue_create("hinoki.audio-output", DISPATCH_QUEUE_SERIAL);
    return queue;
}

static void apply_audio_output(void *) {
    const std::lock_guard<std::mutex> lock(g_audio_output_mutex);
    EmuEnvState *emuenv = ios::get_emuenv();
    if (!g_audio_output_allowed.load() || !emuenv)
        return;
    if (g_audio_output_wanted.load())
        emuenv->audio.resume_output();
    else
        emuenv->audio.suspend_output();
}

static void on_app_activity(bool active) {
    LOG_INFO("Vita3K {} the screen{}", active ? "is back on" : "is leaving", active ? "" : ": the game and the GPU pause");
    app::AppSessionController *session = g_session.load();
    if (session)
        session->set_pause_reason(app::AppSessionPauseReason::Background, !active);
    g_audio_output_wanted = active;
    if (session)
        dispatch_async_f(audio_output_queue(), nullptr, apply_audio_output);
}

// Paused for any reason (pause button, game menu, off screen)
static bool session_paused() {
    const app::AppSessionController *session = g_session.load();
    return session && session->is_paused();
}

extern "C" bool v3k_game_paused_by_user(void) {
    return g_user_paused.load();
}

extern "C" void v3k_set_portrait_unlocked(bool unlocked);
extern "C" void v3k_controls_attach(void *ui_window);
extern "C" void v3k_controls_detach(void);
extern "C" void v3k_controls_show_menu(void);

namespace {

void show_message(const std::string &title, const std::string &message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(), message.c_str(), nullptr);
}

// Where this app's memory (what iOS counts against its limit) goes, for the log: CPU-side pages,
// compressed pages, memory tagged as graphics (Metal), purgeable memory it owns, and the rest (mostly
// memory wired for the GPU, e.g. game memory Metal reads directly). Plus what the RAM helpers carry.
std::string memory_breakdown() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return "unavailable";
    constexpr uint64_t MiB = 1024 * 1024;
    const uint64_t graphics = info.ledger_tag_graphics_footprint;
    const uint64_t purgeable = info.ledger_purgeable_nonvolatile;
    const uint64_t known = info.internal + info.compressed + graphics + purgeable;
    const uint64_t other = info.phys_footprint > known ? info.phys_footprint - known : 0;
    std::string text = fmt::format("footprint {} MiB = CPU pages {} + compressed {} + graphics {} + own purgeable {} + GPU-wired/other {}",
        info.phys_footprint / MiB, info.internal / MiB, info.compressed / MiB, graphics / MiB, purgeable / MiB, other / MiB);
    if (v3k_ram_helpers_backed())
        text += fmt::format("; RAM helpers: {} alive, {} MiB lent, {} MiB of it used by the game", v3k_ram_helpers_live(),
            v3k_ram_helpers_backed() / MiB, v3k_ram_helpers_in_use() / MiB);
    return text;
}

} // namespace

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    SDL_SetAppMetadata("Hinoki", nullptr, "org.vita3k.ios");

    // MoltenVK reads these when the Vulkan instance is created (the layer settings in renderer.cpp
    // say the same; this covers builds where that extension is not honoured)
    setenv("MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE", "1", 0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
    setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
    setenv("MVK_CONFIG_SEMAPHORE_SUPPORT_STYLE", "0", 0);
    setenv("MVK_CONFIG_SHADER_COMPRESSION_ALGORITHM", "1", 0);

    const fs::path documents = fs::path(ios::get_documents_path());
    if (!ios::initialize(documents)) {
        show_message("Hinoki", "The emulator failed to initialise. Check vita3k.log in the app's folder in the Files app.");
        return 1;
    }

    ios::install_fatal_signal_logger();
    ios::crash::install_terminate_handler();
    ios::observe_app_state();
    ios::set_activity_callback(on_app_activity);

    // Activate the audio session before SDL opens the audio device, so the audio unit actually runs
    // (otherwise games that wait on audio playback freeze). From Tsubomi.
    LOG_INFO("{}", ios::configure_audio_session());

    // Prepare the JIT region pool now, while StikDebug is still attached: it usually detaches within
    // a minute of launch, and new executable regions can't be prepared after that (iOS 26).
    // Same order as Tsubomi: guest memory is reserved first, inside prepare_jit_pool().
    const bool jit_capable = ios::jit_capability_enabled();
    LOG_INFO("JIT probe: capability {}, debugger attached {}", jit_capable ? "yes" : "no", ios::debugger_attached() ? "yes" : "no");
    // The library starts JIT preparation right after its first frame (LibraryHost.swift), so the app
    // appears immediately; StikDebug briefly pauses the app for each region it prepares.
    (void)jit_capable;

    // Files dropped into Documents/Import through the Files app or a computer
    const auto import = ios::import_pending_files(documents / "Import");
    LOG_INFO("Startup import: {} installed, {} failed", import.installed.size(), import.failed.size());

    // RAM helpers (Settings > Memory): helper processes lend memory for the guest's RAM. They start
    // in the background now so the first boot doesn't wait for them
    set_guest_backing_provider(v3k_ram_helpers_back);
    if (v3k_ram_helpers_enabled())
        LOG_INFO("RAM helpers: on, {} bundled{}", v3k_ram_helpers_bundled() ? "extension" : "extension NOT",
            v3k_ram_helpers_in_livecontainer() ? ", running inside LiveContainer" : "");
    v3k_ram_helpers_prepare();

    auto *emuenv = ios::get_emuenv();

    for (;;) {
        const std::string title_id = ios::run_library_until_launch();
        const auto app = ios::find_app_by_title_id(title_id);
        if (!app) {
            LOG_ERROR("Requested app {} is not installed", title_id);
            continue;
        }
        ios::crash::session_started(title_id, app->title);

        // Re-reserve guest memory before every launch, so a second session gets the same clean
        // address range as the first instead of whatever the previous session left fragmented
        prereserve_guest_memory();

        // Debug write watch and instruction trace (util/recent_imports.h): off unless set here for a game
        write_watch::reports = 0;
        write_watch::begin = 0;
        write_watch::end = 0;

        // Address space left (not memory): without the extended address space entitlement it runs out
        // long before RAM does, and every allocation fails with bad_alloc
        LOG_INFO("Launching {} (JIT {}, {} free JIT caches, {} MiB of memory available, largest free address block ~{} MiB)", title_id,
            ios::jit_pool_ready() ? "ready" : "NOT ready", ios::jit_pool_available(),
            os_proc_available_memory() / (1024 * 1024), address_space::largest_free_block_mib());

        // The library can rotate freely; games are landscape
        // (Portrait mode: upright too, with the game at the top and the controls below)
        // Games boot in landscape: started upright, the first seconds stalled the GPU (device lost) on
        // iPhone, while turning the phone during play is fine. Portrait opens up once the game is on
        // screen (see on_frame).
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        v3k_set_portrait_unlocked(false);
        v3k_set_portrait_top(-1);

        // A fresh session controller per launch (as Tsubomi does): no state carried over from the last game
        app::AppSessionController session_controller(*emuenv);
        g_user_paused = false;
        g_session = &session_controller;
        g_audio_output_wanted = true;
        g_audio_output_allowed = true;

        // Watchdog: logs memory headroom and stalls, and makes sure Quit always gets you out even if the
        // game is stuck (iOS has no other way to leave a hung game except the app switcher)
        std::atomic_bool session_over{ false };
        std::thread watchdog([emuenv, &session_over] {
            using namespace std::chrono_literals;
            // (display.frames_set counts every frame shown since boot; emuenv->frame_count is reset to 0
            // every second by the FPS counter, so it was no measure of progress or speed)
            uint64_t last_frames = emuenv->display.frames_set.load();
            auto last_progress = std::chrono::steady_clock::now();
            auto last_memory_log = last_progress;
            uint64_t frames_at_memory_log = last_frames;
            bool stall_logged = false;
            bool background = false;
            unsigned memory_warnings = ios::memory_warning_count();
            bool warned_about_memory = false;
            // For the summary at the end of the session
            const auto session_start = last_progress;
            double fps_total = 0.0, fps_min = 0.0, fps_max = 0.0;
            int fps_samples = 0, hot_samples = 0, stalls = 0, memory_samples = 0;
            uint64_t screen_shown_at_log = presentation::counters().shown.load();
            uint64_t screen_skipped_at_log = presentation::counters().skipped.load();
            uint64_t screen_wait_at_log = presentation::counters().wait_ns.load();
            uint64_t least_memory = os_proc_available_memory();
            const auto record_status = [&](double fps) {
                ios::crash::session_status(fmt::format("{} MiB of memory left{}{}{}{}",
                    os_proc_available_memory() / (1024 * 1024), fps >= 0.0 ? fmt::format(", {:.1f} FPS", fps) : std::string(), stall_logged ? ", no new frame for 8+ seconds" : "",
                    warned_about_memory ? ", iOS memory warning" : "",
                    thermal::phone_is_hot() ? ", phone hot" : ""));
            };
            while (!session_over.load()) {
                std::this_thread::sleep_for(500ms);
                const auto now = std::chrono::steady_clock::now();
                if (ios::app_in_background() != background) {
                    background = !background;
                    ios::crash::session_backgrounded(background);
                }
                const uint64_t frames = emuenv->display.frames_set.load();
                if (frames != last_frames) {
                    last_frames = frames;
                    last_progress = now;
                    if (stall_logged)
                        LOG_INFO("Watchdog: the game is presenting frames again");
                    stall_logged = false;
                } else if (session_paused() || ios::app_in_background()) {
                    // Paused or off screen: no frames is expected, not a freeze
                    last_progress = now;
                } else if (!stall_logged && now - last_progress > 8s) {
                    LOG_WARN("Watchdog: no new frame for 8 seconds (the game may be frozen). {} MiB of memory left",
                        os_proc_available_memory() / (1024 * 1024));
                    stall_logged = true;
                    stalls++;
                    record_status(-1.0);
                }
                if (ios::memory_warning_count() != memory_warnings) {
                    LOG_WARN("Watchdog: iOS warned that memory is low, {} MiB left", os_proc_available_memory() / (1024 * 1024));
                    memory_warnings = ios::memory_warning_count();
                    warned_about_memory = true;
                    record_status(-1.0);
                }
                if (now - last_memory_log > 10s) {
                    // Average frame rate over the last 10 seconds, so a log shows how a game ran
                    const double seconds = std::chrono::duration<double>(now - last_memory_log).count();
                    // frames_set starts again from 0 when a game ends: no frames, not a wrapped-around count
                    const double fps = last_frames >= frames_at_memory_log ? static_cast<double>(last_frames - frames_at_memory_log) / seconds : 0.0;
                    // What the screen took: frames shown and skipped, and how long the render thread waited
                    // on the screen per second (V-Sync on waits; long waits hold up the game's GPU work)
                    auto &screen = presentation::counters();
                    const uint64_t shown = screen.shown.load(), skipped = screen.skipped.load(), wait_ns = screen.wait_ns.load();
                    const std::string screen_text = fmt::format(", screen: {} shown, {} skipped, waited {:.0f} ms/s",
                        shown - screen_shown_at_log, skipped - screen_skipped_at_log, static_cast<double>(wait_ns - screen_wait_at_log) / 1e6 / std::max(seconds, 0.001));
                    screen_shown_at_log = shown;
                    screen_skipped_at_log = skipped;
                    screen_wait_at_log = wait_ns;
                    LOG_INFO("Watchdog: {} MiB of memory left, {:.1f} FPS{}{}{}{}", os_proc_available_memory() / (1024 * 1024), fps,
                        screen_text, session_paused() ? " (paused)" : "", thermal::phone_is_hot() ? ", phone hot" : "",
                        thermal::low_power_mode() ? ", Low Power Mode" : "");
                    record_status(fps);
                    if (!session_paused() && !ios::app_in_background() && fps > 0.0) {
                        fps_total += fps;
                        fps_min = fps_samples == 0 ? fps : std::min(fps_min, fps);
                        fps_max = std::max(fps_max, fps);
                        fps_samples++;
                        if (thermal::phone_is_hot())
                            hot_samples++;
                    }
                    least_memory = std::min<uint64_t>(least_memory, os_proc_available_memory());
                    // Every 30 seconds (and under memory pressure) say where the memory goes
                    if (++memory_samples % 3 == 1 || os_proc_available_memory() < 600ull * 1024 * 1024) {
#if defined(__aarch64__)
                        // Caches threads gave back while the game runs (finished threads) hold code nobody runs
                        trim_ios_jit_code_cache_pool();
#endif
                        LOG_INFO("Memory: {}; textures {} MiB in {} (budget {} MiB)", memory_breakdown(),
                            gpu_memory::texture_bytes().load() >> 20, gpu_memory::texture_count().load(), gpu_memory::texture_budget().load() >> 20);
                    }
                    last_memory_log = now;
                    frames_at_memory_log = last_frames;
                }
                const uint64_t quit_at = v3k_pad_quit_requested_ms();
                const uint64_t now_ms = v3k_pad_now_ms();
                if (quit_at && now_ms > quit_at && now_ms - quit_at > 20000) {
                    LOG_CRITICAL("Watchdog: the game did not close 20 seconds after Quit, closing Vita3K");
                    // Which guest threads were still alive tells us what blocked the shutdown
                    std::string threads;
                    // try_lock: a thread stuck while holding the kernel lock must not stop us from exiting
                    if (std::unique_lock<std::mutex> lock(emuenv->kernel.mutex, std::try_to_lock); lock.owns_lock()) {
                        // Names only: reading a thread's status takes its lock, which a stuck thread may hold
                        for (const auto &[id, thread] : emuenv->kernel.threads) {
                            if (thread)
                                threads += fmt::format("{}{}", threads.empty() ? "" : ", ", thread->name);
                        }
                    }
                    LOG_CRITICAL("Watchdog: guest threads still active: {}", threads.empty() ? "none" : threads);
                    // The player asked to leave and is leaving: that is not a crash, so no crash report
                    // (the log above keeps the details)
                    ios::crash::session_ended();
                    if (auto logger = spdlog::default_logger())
                        logger->flush();
                    _exit(0);
                }
            }
            const double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - session_start).count() / 60.0;
            if (fps_samples > 0) {
                LOG_INFO("Session summary: {:.1f} min played, {:.1f} FPS on average ({:.1f} lowest, {:.1f} highest, over {} ten-second samples), "
                         "{} stall{}, {} sample{} with a hot phone, least memory left {} MiB",
                    minutes, fps_total / fps_samples, fps_min, fps_max, fps_samples, stalls, stalls == 1 ? "" : "s", hot_samples, hot_samples == 1 ? "" : "s",
                    least_memory / (1024 * 1024));
            } else {
                LOG_INFO("Session summary: {:.1f} min, no full ten-second sample of gameplay, {} stall{}, least memory left {} MiB",
                    minutes, stalls, stalls == 1 ? "" : "s", least_memory / (1024 * 1024));
            }
        });

        // On-screen touch controls: a virtual gamepad plus an overlay on the game window
        const sdl_frontend::Hooks hooks{
            .on_window = [](SDL_Window *window) {
                if (window) {
                    if (!v3k_pad_attach())
                        LOG_WARN("Touch controls unavailable: {}", SDL_GetError());
                    void *ui_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
                    v3k_controls_attach(ui_window);
                    ios::set_screen_always_on(true);
                } else {
                    ios::set_screen_always_on(false);
                    v3k_controls_detach();
                    v3k_pad_detach();
                } },
            // Controller guide / PS button: the in-game menu
            .on_guide_button = [] { v3k_controls_show_menu(); },
            .on_stop = [emuenv] {
                // No audio device switches once the game's audio is being torn down
                {
                    const std::lock_guard<std::mutex> lock(g_audio_output_mutex);
                    g_audio_output_allowed = false;
                }
                ios::cheats::session_ended(*emuenv);
                ios::extras::session_ended(*emuenv); },
            .on_frame = [emuenv, cheats_started = false, first_frame_at = uint64_t(0), portrait_open = false]() mutable {
                if (!cheats_started) {
                    cheats_started = true;
                    ios::cheats::session_started(*emuenv);
                    ios::extras::session_started(*emuenv);
                }
                // Portrait mode: allowed 2 s after the game first showed a frame of its own
                if (!portrait_open && ios::portrait_mode_enabled()) {
                    if (!first_frame_at && emuenv->frame_count > 0)
                        first_frame_at = SDL_GetTicks();
                    if (first_frame_at && SDL_GetTicks() - first_frame_at > 2000) {
                        portrait_open = true;
                        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
                        v3k_set_portrait_unlocked(true);
                        LOG_INFO("Portrait mode available");
                    }
                }
                ios::cheats::frame(*emuenv);
                ios::extras::frame(*emuenv); },
        };
        // The GPU failing over and over (MoltenVK device lost) ends the game instead of freezing it
        renderer::vulkan::device_lost_count() = 0;
        renderer::vulkan::on_persistent_device_lost() = +[] { v3k_pad_request_exit(); };
        thermal::keep_cool() = ios::keep_cool_enabled();
        // Accurate CPU, for games whose fix asks for it (game fixes list: accurate-cpu=on)
        set_ios_accurate_cpu(v3k_game_fix_flag(title_id.c_str(), "accurate-cpu") == 1);
        // Read when the renderer starts: vertex attribute realignment for Metal (on unless a game's fix
        // says vertex-realign=off) and precise shader math (a fix can ask for it: precise-math=on)
        // (the player can also turn realignment off for one game: V3K_VERTEX_REALIGN_USER_OFF, set by the library)
        const char *user_realign_off = getenv("V3K_VERTEX_REALIGN_USER_OFF");
        const bool realign_off = v3k_game_fix_flag(title_id.c_str(), "vertex-realign") == 0
            || (user_realign_off && user_realign_off[0] == '1');
        setenv("V3K_VERTEX_REALIGN", realign_off ? "0" : "1", 1);
        if (v3k_game_fix_flag(title_id.c_str(), "precise-math") == 1)
            setenv("V3K_PRECISE_SHADER_MATH", "1", 1);
        const bool mapping_was_on = v3k_game_uses_memory_mapping(title_id.c_str());
        const bool async_was_on = v3k_game_uses_async_shaders(title_id.c_str());
        const int result = sdl_frontend::run(*emuenv, session_controller, AppLaunchRequest{ .app_path = title_id }, hooks);
        session_over = true;
        watchdog.join();
        {
            const std::lock_guard<std::mutex> lock(g_audio_output_mutex);
            g_audio_output_allowed = false;
        }
        g_session = nullptr;
        g_user_paused = false;
        renderer::vulkan::on_persistent_device_lost() = nullptr;
        const uint32_t gpu_errors = renderer::vulkan::device_lost_count();
        ios::crash::session_ended();
        v3k_pad_clear_quit_request();
        LOG_INFO("{} exited with code {}, {} free JIT caches", title_id, result, ios::jit_pool_available());
        v3k_ram_helpers_game_ended();
#if defined(__aarch64__)
        // The game's code caches went back to the pool: their memory too
        if (const std::size_t released = trim_ios_jit_code_cache_pool())
            LOG_INFO("JIT: gave {} MiB of code cache memory back to iOS after the game", released / (1024 * 1024));
#endif
        if (gpu_errors >= renderer::vulkan::PERSISTENT_DEVICE_LOST_ERRORS) {
            // GPU memory mapping lets shaders read game memory directly, which is the usual way a
            // Vita game can fault the GPU: turn it off for this game and say so
            std::string message = fmt::format("The GPU stopped responding while {} was running, so the game was closed.", title_id);
            // Compiling shaders before drawing is the other usual cause (it hung Madden NFL 13), and the
            // cheaper one to give up; memory mapping stays on for games whose fix needs it
            if (!async_was_on && v3k_set_game_setting(title_id.c_str(), "async-shaders", "true")) {
                LOG_INFO("Turned on background shader compilation for {} after repeated GPU errors", title_id);
                message += " Shaders now compile in the background for this game (Game settings), which usually avoids this. Start it again to try.";
            } else if (mapping_was_on && v3k_game_fix_flag(title_id.c_str(), "memory-mapping") != 1
                && v3k_set_game_setting(title_id.c_str(), "memory-mapping", "false")) {
                LOG_INFO("Turned off GPU memory mapping for {} after repeated GPU errors", title_id);
                message += " GPU memory mapping is now off for this game (Game settings), which usually avoids this. Start it again to try.";
            } else {
                message += " Try a lower resolution, or send the log from Settings › About.";
            }
            v3k_set_session_message(message.c_str());
        }
    }
}
