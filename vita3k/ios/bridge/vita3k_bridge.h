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

// Plain C interface between the SwiftUI frontend and the Vita3K core.
// It plays the same role as the JNI functions in vita3k/android/jni/ do for the Android UI.
// Strings returned as char * are heap-allocated: release them with v3k_free_string.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Status ----
const char *v3k_version(void);
// True once the JIT arena is prepared and passed its self-test
bool v3k_is_jit_enabled(void);
// Why JIT is or isn't available, for the UI (free with v3k_free_string)
char *v3k_jit_status_text(void);
// True while JIT is being checked or set up
bool v3k_jit_preparing(void);
// True when a JIT script is attached and v3k_jit_request can set up JIT (iOS 26)
bool v3k_jit_can_request(void);
// JIT regions prepared so far, and how many are wanted
int v3k_jit_progress(void);
int v3k_jit_target(void);
// Asks the JIT script for executable memory; the app pauses while the debugger works
void v3k_jit_request(void);
// Logs the JIT state (iOS version, debugger, pool) with a reason, for diagnosing attach problems
void v3k_log_jit_state(const char *why);
const char *v3k_storage_path(void);
// Folder that holds ux0, vs0, pd0 ... (free with v3k_free_string)
char *v3k_pref_path(void);
// Decodes a Vita .at9 file (home music) into a WAV file. Blocking: call off the main thread.
bool v3k_decode_at9(const char *input_path, const char *output_path);

// Bit 0: preinstalled package, bit 1: main firmware, bit 2: font package
int v3k_firmware_state(void);

// ---- Library ----
// JSON array: [{"titleId","title","version","category","iconPath","path","lastPlayed","playTime","customConfig"}]
char *v3k_apps_json(void);
void v3k_refresh_apps(void);
bool v3k_delete_app(const char *title_id);
void v3k_free_string(char *str);

// Deletes the save data of an installed app
bool v3k_delete_save_data(const char *title_id);

// ---- Storage location ----
// JSON: {"path","custom":bool,"freeBytes","totalBytes","note"}
char *v3k_storage_info_json(void);
// Moves the emulator to a picked folder (device or external drive), copying firmware there.
// Blocking: call off the main thread. *message gets a summary to free with v3k_free_string.
bool v3k_set_storage_folder(const char *folder, char **message);
bool v3k_use_internal_storage(char **message);

// ---- Installing ----
typedef void (*v3k_progress_fn)(void *context, float percent);

// What kind of thing a path is: 0 unknown, 1 firmware (.PUP), 2 archive (.vpk/.zip), 3 package (.pkg), 4 folder,
// 5 license (work.bin / .rif of 512 bytes, or a .zrif text file). v3k_install installs any of them.
int v3k_install_kind(const char *path);

// Adds a game's license from its zRIF key (ux0/license/<title ID>/<content ID>.rif), so its .pkg or a
// dump without its work.bin installs afterwards. *message gets a summary to free with v3k_free_string.
bool v3k_install_zrif(const char *zrif, char **message);

// Installs firmware, a .vpk/.zip, a .pkg (zrif may be NULL or empty when a license is already present),
// or a folder (a game folder, or a folder holding any of the above). Blocking: call it off the main thread.
// On return *message (if not NULL) holds a human-readable summary to free with v3k_free_string.
bool v3k_install(const char *path, const char *zrif, v3k_progress_fn progress, void *context, char **message);

// What a .pkg is, without installing it: JSON {"kind": "game"|"update"|"dlc"|"theme"|"unknown",
// "titleId","title","contentId","zrif" (a license already on the device, or ""), "gameInstalled"}
char *v3k_pkg_info_json(const char *path);

// Reads the zRIF stored next to a package (GAME.pkg -> GAME.zrif), or NULL
char *v3k_zrif_next_to(const char *pkg_path);

// ---- Settings ----
// JSON object with the values used by the settings screen
char *v3k_settings_json(void);
// Keys: "resolution-multiplier" (float), "anisotropic-filtering" (int), "audio-volume" (int),
// "screen-filter" (string), and booleans "v-sync", "cpu-opt", "ngs-enable", "high-accuracy",
// "stretch-display", "performance-overlay", "texture-cache", "turbo-mode", "pstv-mode"
bool v3k_set_setting_number(const char *key, double value);
bool v3k_set_setting_bool(const char *key, bool value);
bool v3k_set_setting_string(const char *key, const char *value);

// Ad hoc over the internet: settings keys "adhoc-relay" (bool), "adhoc-relay-server",
// "adhoc-relay-room", "adhoc-relay-key" (strings). Status is a line like
// "connected to host:27360, room ..., virtual IP 10.77.0.2, 1 other player(s), ping 210 ms" or "off".
char *v3k_adhoc_relay_status(void);

// ---- Controller mapping ----
// The core reads each Vita button from one physical (SDL gamepad) button. Slots and values are
// SDL_GamepadButton numbers: slot = the button's default position, value = physical button used.
// JSON array of 15 numbers.
char *v3k_controller_binds_json(void);
bool v3k_set_controller_bind(int slot, int physical_button);
void v3k_reset_controller_binds(void);

// "Fast CPU": floating-point shortcuts in the recompiler, for games started afterwards
void v3k_set_fast_cpu(bool enabled);

// ---- Performance overlay ----
// JSON {"fps","avgFps","minFps","maxFps","frameMs","cpu","memoryUsed","memoryAvailable","resolutionScale",
// "width","height","shadersCompiled"}. cpu is the app's CPU use (100 = one core).
char *v3k_perf_json(void);

// ---- Misc ----
// Writes a line from the Swift UI to vita3k.log
void v3k_log_ui(const char *message);
// Path of vita3k.log, for sharing
const char *v3k_log_path(void);

// ---- Launching ----
// Asks the app loop to close the library and boot this title
void v3k_request_launch(const char *title_id);
// Title ID from a vita3k://launch/<ID> link or a Home Screen quick action, or NULL (free the string).
// A "V3KLaunchRequest" notification is posted when one arrives.
char *v3k_take_launch_request(void);
// Path of a file opened with "Open in Vita3K", or NULL (free the string). Same notification.
char *v3k_take_import_request(void);

// ---- In-game extras (ios/game_extras.cpp), carried out on the next frame ----
// Saves the current frame to Photos and to <Vita3K folder>/screenshots
void v3k_request_screenshot(void);
// Touches on the game go to the rear touchpad instead of the front screen
void v3k_set_rear_touch(bool rear);
bool v3k_rear_touch(void);
// Portrait mode: pixels from the top of the screen to the game picture (negative: centred)
void v3k_set_portrait_top(float pixels);

// ---- Crash reports ----
// JSON {"report","log"} describing how the previous session ended, or NULL if it ended normally.
// Returned once per launch.
char *v3k_take_crash_report(void);

// ---- Updates and DLC of an installed game ----
// JSON {"update": installed update version or "", "updateBytes", "dlc": number of DLC, "dlcBytes"}
char *v3k_game_content_json(const char *title_id);
bool v3k_delete_game_content(const char *title_id, bool update);

// ---- Shader cache ----
double v3k_shader_cache_bytes(const char *title_id);
bool v3k_delete_shader_cache(const char *title_id);

// ---- Per-game settings (override the global settings for one game) ----
// JSON {"custom":bool, "resolutionMultiplier", "anisotropicFiltering", "screenFilter", "memoryMapping",
// "highAccuracy", "textureCache", "cpuOpt", "vSync", "fpsHack", "disableSurfaceSync", "stretchDisplay",
// "asyncShaders", "shaderCache"}
char *v3k_game_settings_json(const char *title_id);
// Keys: "resolution-multiplier", "anisotropic-filtering", "screen-filter", and booleans ("true"/"false")
// "memory-mapping", "high-accuracy", "texture-cache", "cpu-opt", "v-sync", "fps-hack",
// "disable-surface-sync", "stretch-display", "async-shaders", "shader-cache"
bool v3k_set_game_setting(const char *title_id, const char *key, const char *value);
// Whether the game runs with GPU memory mapping (its own setting, else the global one)
bool v3k_game_uses_async_shaders(const char *title_id);
bool v3k_game_uses_memory_mapping(const char *title_id);
// A message for the library about how the last game session ended (NULL if none; free the string)
char *v3k_take_session_message(void);
void v3k_set_session_message(const char *message);
bool v3k_reset_game_settings(const char *title_id);

// ---- Save data backups ----
// Zips the save into Documents/Save Backups and returns the zip path (free it), or NULL
char *v3k_export_save_data(const char *title_id, char **message);
// Replaces the save with one from a zip or a folder: a save exported decrypted from a modded Vita
// (VitaShell, Vita Save Manager, Apollo) or another Vita3K, in any of the usual layouts (the save's
// files, its PCSE00120/ folder, savedata/, a whole ux0/). The current save is copied to
// Documents/Save Backups first; encrypted saves and other games' saves are refused with a reason.
bool v3k_import_save_data(const char *title_id, const char *path, char **message);

// ---- Game patches and cheats (github.com/Vita3K/vita3k-patches) ----
// Downloads the patch list and the cheat files in one go. Blocking: run off the main thread.
bool v3k_download_patches(char **message);
bool v3k_patches_downloaded(void);
// JSON array [{"key","name","author","note","version","appVersion","enabled","versionMatches"}] for one game
char *v3k_patches_json(const char *title_id);
// Every patch, grouped by game: [{"title","titleIds":[],"installedTitleId","installedVersion","patches":[...]}]
char *v3k_all_patches_json(void);
bool v3k_set_patch_enabled(const char *key, bool enabled);

bool v3k_cheats_downloaded(void);
// JSON array [{"key","name","author","file","gameVersion","supported","enabled","versionMatches"}]
char *v3k_cheats_json(const char *title_id);
bool v3k_set_cheat_enabled(const char *title_id, const char *key, bool enabled);
// Folder for the user's own .psv cheat files (free the string)
char *v3k_cheats_folder(void);

// ---- Mods (files in ux0/rePatch/<title ID>/ replace the game's files, like rePatch on a Vita) ----
// JSON {"path","files","bytes","enabled"}
char *v3k_mod_info_json(const char *title_id);
bool v3k_import_mod(const char *title_id, const char *zip_path, char **message);
bool v3k_set_mods_enabled(const char *title_id, bool enabled);
bool v3k_delete_mods(const char *title_id);

// ---- Compatibility list (Vita3K/compatibility), game fixes list, problem reports ----
// Blocking downloads: call off the main thread
bool v3k_update_compat_db(void);
bool v3k_update_game_fixes(void);
// JSON {"note","settings"} of this game's entry in the game fixes list, or NULL (free the string)
char *v3k_game_fix_json(const char *title_id);
// A on/off setting from the game fixes list: 1 on, 0 off, -1 not set (fast-cpu is read this way)
int v3k_game_fix_flag(const char *title_id, const char *key);
// Writes the game's recommended settings into its own settings (for games that already have some)
bool v3k_apply_game_fix(const char *title_id);
// Zips the logs, the last crash report, device info and settings for sharing; returns its path
char *v3k_make_problem_report(char **message);

#ifdef __cplusplus
}
#endif
