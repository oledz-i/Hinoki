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

#include "vita3k_bridge.h"

#include "../cheats.h"
#include "../crash_report.h"
#include "../ios_platform.h"
#include "../ios_session.h"
#include "../jit/ios_jit.h"
#include "archive.h"

#include <app/functions.h>
#include <app/state.h>
#include <codec/at9_file.h>
#include <compat/functions.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/settings.h>
#include <config/state.h>
#include <config/version.h>
#include <cpu/functions.h>
#include <io/state.h>
#include <miniz.h>
#include <net/relay.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <patch/patch.h>
#include <renderer/state.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

// Installs touch the app list and the emulated file system, so only one runs at a time
std::mutex install_mutex;

char *dup_string(const std::string &value) {
    char *out = static_cast<char *>(std::malloc(value.size() + 1));
    if (out)
        std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
                out += fmt::format("\\u{:04x}", c);
            else
                out += static_cast<char>(c);
        }
    }
    return out;
}

std::string json_quote(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}

std::string lower_extension(const fs::path &path) {
    return string_utils::tolower(path.extension().string());
}

// A license file: a work.bin / .rif (a 512-byte NpDrm license), or a .zrif text file holding a zRIF
bool is_license_file(const fs::path &path) {
    const auto ext = lower_extension(path);
    if (ext == ".zrif")
        return true;
    if (ext != ".bin" && ext != ".rif")
        return false;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return !ec && (size == sizeof(SceNpDrmLicense) || size == 1024);
}

int kind_of(const fs::path &path) {
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return 4;
    const auto ext = lower_extension(path);
    if (ext == ".pup")
        return 1;
    if (ext == ".vpk" || ext == ".zip")
        return 2;
    if (ext == ".pkg")
        return 3;
    if (is_license_file(path))
        return 5;
    return 0;
}

std::string trimmed_zrif(std::string zrif) {
    std::erase_if(zrif, [](unsigned char c) { return std::isspace(c); });
    return zrif;
}

// Installs a license into ux0/license/<title ID>/<content ID>.rif, from a zRIF or a license file.
// Games installed later (zip/vpk dumps without their work.bin, .pkg files) find it there.
bool install_license_unchecked(EmuEnvState &emuenv, const std::string &zrif, const fs::path &file, std::string &result);

bool install_license(EmuEnvState &emuenv, const std::string &zrif, const fs::path &file, std::string &result) {
    // A damaged license can make the parsers throw (a short content ID, say)
    try {
        return install_license_unchecked(emuenv, zrif, file, result);
    } catch (const std::exception &e) {
        LOG_ERROR("License install failed: {}", e.what());
        result = "The license could not be read.";
        return false;
    }
}

bool install_license_unchecked(EmuEnvState &emuenv, const std::string &zrif, const fs::path &file, std::string &result) {
    emuenv.license_title_id.clear();
    emuenv.license_content_id.clear();
    bool ok = false;
    if (!zrif.empty()) {
        if (!validate_zrif(zrif)) {
            result = "That is not a valid zRIF. It starts with KO5i and is about 50 to 150 characters long.";
            return false;
        }
        ok = create_license(emuenv, zrif);
    } else if (lower_extension(file) == ".zrif") {
        std::ifstream in(file);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return install_license(emuenv, trimmed_zrif(text), {}, result);
    } else {
        ok = copy_license(emuenv, file);
    }
    if (!ok || emuenv.license_title_id.empty()) {
        result = "The license could not be read. A work.bin is 512 bytes; take it from the game's sce_sys/package folder.";
        return false;
    }
    const bool game_installed = fs::exists(emuenv.vita_fs_path / "ux0/app" / emuenv.license_title_id);
    result = fmt::format("License for {} added ({}).{}", emuenv.license_title_id, emuenv.license_content_id,
        game_installed ? "" : " Now import the game: its .pkg, or a .zip/.vpk/folder dump without a work.bin.");
    LOG_INFO("License installed: {} ({})", emuenv.license_title_id, emuenv.license_content_id);
    return true;
}

std::string read_zrif(const fs::path &pkg_path) {
    fs::path zrif_path = pkg_path;
    zrif_path.replace_extension(".zrif");
    std::ifstream file(zrif_path);
    if (!file)
        return {};
    std::string zrif;
    std::getline(file, zrif);
    std::erase_if(zrif, [](unsigned char c) { return std::isspace(c); });
    return zrif;
}

struct InstallOutcome {
    int installed = 0;
    int failed = 0;
    std::string firmware_version;
    std::vector<std::string> errors; // readable reasons, shown to the user
    std::vector<std::string> notes; // what was done besides installing a game (licenses added)
};

bool install_license(EmuEnvState &emuenv, const std::string &zrif, const fs::path &file, std::string &result);

// Licenses packed in a zip (work.bin anywhere, .rif, .zrif): installed before the dump in it, so a
// dump whose work.bin sits outside its sce_sys/package folder still installs
void install_licenses_in_zip(EmuEnvState &emuenv, const fs::path &zip_path) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0))
        return;
    const fs::path temp = emuenv.cache_path / "zip-license";
    std::error_code ec;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string lower = string_utils::tolower(name);
        // A work.bin inside sce_sys/package belongs to the dump and is used from there
        if (lower.contains("__macosx") || lower.contains("sce_sys/package/"))
            continue;
        const bool license = lower.ends_with(".zrif")
            || ((lower.ends_with(".bin") || lower.ends_with(".rif")) && (stat.m_uncomp_size == sizeof(SceNpDrmLicense) || stat.m_uncomp_size == 1024));
        if (!license)
            continue;
        fs::create_directories(temp, ec);
        const fs::path out = temp / fs::path(name).filename();
        if (mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0)) {
            std::string result;
            install_license(emuenv, {}, out, result);
        }
    }
    mz_zip_reader_end(&zip);
    fs::remove_all(temp, ec);
}

bool install_file(EmuEnvState &emuenv, const fs::path &file, const std::string &zrif_in, v3k_progress_fn progress, void *context, InstallOutcome &outcome);

// Games first, then updates, then DLC: an update or DLC needs its game installed
int pkg_order(const fs::path &pkg) {
    PkgInfo info;
    if (!read_pkg_info(pkg, info))
        return 3;
    return info.kind == "game" ? 0 : info.kind == "update" ? 1
        : info.kind == "dlc"                               ? 2
                                                           : 3;
}

// Extracts the .pkg files of a zip (with any .zrif next to them) and installs them. Returns how
// many installed; failures are counted in outcome.
int install_pkgs_in_zip(EmuEnvState &emuenv, const fs::path &zip_path, v3k_progress_fn progress, void *context, InstallOutcome &outcome) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0))
        return 0;
    std::vector<mz_uint> entries;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        char name[1024];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        const std::string lower = string_utils::tolower(name);
        if (!mz_zip_reader_is_file_a_directory(&zip, i) && (lower.ends_with(".pkg") || lower.ends_with(".zrif"))
            && !lower.starts_with("__macosx"))
            entries.push_back(i);
    }
    if (std::none_of(entries.begin(), entries.end(), [&](mz_uint i) {
            char name[1024];
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            return string_utils::tolower(name).ends_with(".pkg");
        })) {
        mz_zip_reader_end(&zip);
        return 0;
    }

    const fs::path temp = emuenv.cache_path / "zip-pkg" / zip_path.stem();
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    std::vector<fs::path> pkgs;
    for (const mz_uint i : entries) {
        char name[1024];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        const fs::path out = temp / fs::path(name).filename();
        if (mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0) && string_utils::tolower(out.extension().string()) == ".pkg")
            pkgs.push_back(out);
    }
    mz_zip_reader_end(&zip);

    std::ranges::stable_sort(pkgs, [](const fs::path &a, const fs::path &b) { return pkg_order(a) < pkg_order(b); });
    int installed = 0;
    for (const auto &pkg : pkgs) {
        LOG_INFO("Installing {} from {}", pkg.filename().string(), zip_path.filename().string());
        if (install_file(emuenv, pkg, {}, progress, context, outcome))
            installed++;
        else
            outcome.failed++;
    }
    fs::remove_all(temp, ec);
    return installed;
}

// Installs one file. Folders are handled by install_path.
bool install_file(EmuEnvState &emuenv, const fs::path &file, const std::string &zrif_in, v3k_progress_fn progress, void *context, InstallOutcome &outcome) {
    const auto report = [&](float percent) {
        if (progress)
            progress(context, percent);
    };

    switch (kind_of(file)) {
    case 1: {
        const auto version = install_pup(emuenv.vita_fs_path, file, [&](uint32_t p) { report(static_cast<float>(p)); });
        if (version.empty())
            return false;
        outcome.firmware_version = version;
        return true;
    }
    case 2: {
        // .pkg files inside the zip (often updates or DLC): extracted and installed like loose ones
        const int pkg_results = install_pkgs_in_zip(emuenv, file, progress, context, outcome);
        const auto reinstall = [](const std::string &, const std::string &) { return true; };
        const auto progress_fn = [&](ArchiveContents contents) {
            if (contents.progress)
                report(*contents.progress);
            else if (contents.count && contents.current && *contents.count > 0)
                report(*contents.current / *contents.count * 100.f);
        };
        // A license (work.bin / .rif / .zrif) packed in the zip next to a dump: installed first, so
        // the dump finds it
        install_licenses_in_zip(emuenv, file);
        const auto contents = install_archive(emuenv, file, progress_fn, reinstall);
        int archive_installed = 0;
        for (const auto &content : contents) {
            if (content.state) {
                archive_installed++;
            } else if (pkg_results == 0 || !content.title_id.empty()) {
                // (a zip of .pkg files has no game folder: that is not an error)
                const std::string name = content.title_id.empty() ? file.filename().string() : content.title_id;
                outcome.errors.push_back(content.error.empty() ? fmt::format("{}: installation failed (see vita3k.log).", name)
                                                               : fmt::format("{}: {}", name, content.error));
            }
        }
        // Several games in one zip: count the extra ones
        if (archive_installed > 1)
            outcome.installed += archive_installed - 1;
        return archive_installed > 0 || pkg_results > 0;
    }
    case 3: {
        std::string zrif = zrif_in.empty() ? read_zrif(file) : zrif_in;
        PkgInfo info;
        const bool have_info = read_pkg_info(file, info);
        if (zrif.empty() && have_info) {
            // Updates use the game's license; a game or DLC may already have its own on the device
            zrif = find_title_zrif(emuenv.vita_fs_path, info.title_id, info.content_id);
        }
        if (have_info && info.kind == "update" && !fs::exists(emuenv.vita_fs_path / "ux0/app" / info.title_id)) {
            outcome.errors.push_back(fmt::format("{}: install the game ({}) before its update", file.filename().string(), info.title_id));
            return false;
        }
        LOG_INFO("Installing {} ({} {}), license {}", file.filename().string(), info.kind, info.title_id, zrif.empty() ? "not found" : "found");
        return install_pkg(file, emuenv, zrif, [&](float p) { report(p); });
    }
    case 5: {
        std::string result;
        const bool ok = install_license(emuenv, zrif_in, file, result);
        if (ok)
            outcome.notes.push_back(result);
        else
            outcome.errors.push_back(fmt::format("{}: {}", file.filename().string(), result));
        return ok;
    }
    default: {
        const auto ext = lower_extension(file);
        if (ext == ".7z" || ext == ".rar")
            outcome.errors.push_back(fmt::format("{}: .7z and .rar archives aren't supported. Extract it first (for example with an unarchiver app) and import the folder, or re-pack it as .zip.", file.filename().string()));
        else if (ext == ".bin")
            outcome.errors.push_back(fmt::format("{}: not a license file (a work.bin is exactly 512 bytes).", file.filename().string()));
        else
            outcome.errors.push_back(fmt::format("{}: Hinoki doesn't know this kind of file. Import .vpk, .zip, .pkg, .pup, work.bin/.rif/.zrif licenses, or a game folder.", file.filename().string()));
        return false;
    }
    }
}

void install_path(EmuEnvState &emuenv, const fs::path &path, const std::string &zrif, v3k_progress_fn progress, void *context, InstallOutcome &outcome) {
    if (kind_of(path) != 4) {
        if (install_file(emuenv, path, zrif, progress, context, outcome))
            outcome.installed++;
        else
            outcome.failed++;
        return;
    }

    // Licenses (work.bin, .rif, .zrif) at the top of the folder first: a dump in it may need one
    std::error_code license_ec;
    for (const auto &entry : fs::directory_iterator(path, license_ec)) {
        if (entry.is_regular_file() && kind_of(entry.path()) == 5) {
            std::string result;
            if (install_license(emuenv, {}, entry.path(), result))
                outcome.notes.push_back(result);
        }
    }

    // A dumped game folder (sce_sys/param.sfo inside), or several of them
    const uint32_t contents = install_contents(emuenv, path);
    if (contents > 0) {
        outcome.installed += static_cast<int>(contents);
        return;
    }

    // Otherwise a folder of files to install: firmware first, so apps installed next find it
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(path, ec)) {
        // (licenses were installed above)
        if (entry.is_regular_file() && kind_of(entry.path()) != 0 && kind_of(entry.path()) != 5)
            files.push_back(entry.path());
    }
    // Firmware first, then everything else, with packages ordered game > update > DLC
    std::ranges::stable_sort(files, [](const fs::path &a, const fs::path &b) {
        const auto rank = [](const fs::path &f) { return kind_of(f) == 1 ? -1 : (kind_of(f) == 3 ? pkg_order(f) : 0); };
        return rank(a) < rank(b);
    });
    for (const auto &file : files) {
        if (install_file(emuenv, file, {}, progress, context, outcome))
            outcome.installed++;
        else
            outcome.failed++;
    }
}

} // namespace

extern "C" {

const char *v3k_version(void) {
    static const std::string version = fmt::format("{}-{}-{}", app_version, app_number, app_hash);
    return version.c_str();
}

namespace {
std::atomic_bool jit_preparing{ false };
}

bool v3k_is_jit_enabled(void) {
    return ios::jit_pool_ready();
}

char *v3k_jit_status_text(void) {
    if (jit_preparing)
        return dup_string(fmt::format("Preparing JIT ({}/{})...", ios::jit_pool_progress(), ios::jit_pool_target()));
    return dup_string(ios::jit_status_text());
}

bool v3k_jit_preparing(void) {
    return jit_preparing;
}

int v3k_jit_progress(void) {
    return ios::jit_pool_progress();
}

int v3k_jit_target(void) {
    return ios::jit_pool_target();
}

bool v3k_jit_can_request(void) {
    return ios::jit_can_prepare();
}

void v3k_log_jit_state(const char *why) {
    LOG_INFO("JIT state ({}): {}", why ? why : "", ios::jit_state_line());
}

void v3k_jit_request(void) {
    if (jit_preparing.exchange(true))
        return;
    LOG_INFO("JIT: preparing the region pool. {}", ios::jit_state_line());
    std::thread([] {
        const auto started = std::chrono::steady_clock::now();
        const bool ok = ios::prepare_jit_pool();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (ok)
            LOG_INFO("JIT region pool prepared in {} ms", ms);
        else
            LOG_WARN("JIT region pool preparation failed after {} ms: {}. {}", ms, ios::jit_status_text(), ios::jit_state_line());
        jit_preparing = false;
    }).detach();
}

const char *v3k_storage_path(void) {
    static const std::string path = ios::get_documents_path();
    return path.c_str();
}

char *v3k_pref_path(void) {
    auto *emuenv = ios::get_emuenv();
    return dup_string(emuenv ? emuenv->vita_fs_path.string() : std::string());
}

bool v3k_decode_at9(const char *input_path, const char *output_path) {
    return input_path && output_path && decode_at9_file_to_wav(input_path, output_path);
}

int v3k_firmware_state(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return 0;
    const auto state = app::get_firmware_state(*emuenv);
    return (state.preinstalled_package ? 1 : 0) | (state.main_firmware ? 2 : 0) | (state.font_package ? 4 : 0);
}

namespace {
// The compatibility list is replaced from a background download while the library reads it
std::mutex compat_mutex;
} // namespace

char *v3k_apps_json(void) {
    std::string json = "[";
    bool first = true;
    auto *emuenv = ios::get_emuenv();
    const auto times = emuenv ? app::get_user_app_times(*emuenv) : std::map<std::string, app::AppTime>{};
    for (const auto &app : ios::list_apps()) {
        if (!first)
            json += ",";
        first = false;
        const auto time = times.find(app.path);
        const long long last_played = time != times.end() ? static_cast<long long>(time->second.last_time_used) : 0;
        const long long play_time = time != times.end() ? static_cast<long long>(time->second.time_used) : 0;
        const bool custom = emuenv && config::has_custom_config(emuenv->config_path, app.path);
        int compat_state = -1;
        uint32_t compat_issue = 0;
        if (emuenv) {
            const std::lock_guard compat_lock(compat_mutex);
            const auto it = emuenv->compat.app_compat_db.find(app.title_id);
            if (it != emuenv->compat.app_compat_db.end()) {
                compat_state = static_cast<int>(it->second.state);
                compat_issue = it->second.issue_id;
            }
        }
        json += fmt::format(R"({{"titleId":{},"title":{},"version":{},"category":{},"iconPath":{},"path":{},"lastPlayed":{},"playTime":{},"customConfig":{},"compat":{},"compatIssue":{}}})",
            json_quote(app.title_id), json_quote(app.title), json_quote(app.app_ver), json_quote(app.category), json_quote(app.icon_path), json_quote(app.path), last_played,
            play_time, custom ? "true" : "false", compat_state, compat_issue);
    }
    json += "]";
    return dup_string(json);
}

void v3k_refresh_apps(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return;
    std::lock_guard lock(install_mutex);
    app::scan_apps(*emuenv);
}

bool v3k_delete_app(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    const auto app = ios::find_app_by_title_id(title_id);
    if (!app)
        return false;
    std::lock_guard lock(install_mutex);
    app::delete_app(*emuenv, app->path);
    app::scan_apps(*emuenv);
    return true;
}

bool v3k_delete_save_data(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    const auto app = ios::find_app_by_title_id(title_id);
    if (!app || app->savedata.empty())
        return false;
    const fs::path savedata = emuenv->vita_fs_path / "ux0/user" / emuenv->io.user_id / "savedata" / app->savedata;
    std::error_code ec;
    fs::remove_all(savedata, ec);
    LOG_INFO("Deleted save data of {}: {}", title_id, ec ? ec.message() : "ok");
    return !ec;
}

char *v3k_storage_info_json(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return dup_string("{}");
    // Same numbers as Settings > General > iPhone Storage: free space counts what iOS frees up by itself
    uint64_t total = 0, available = 0;
    if (!ios::volume_capacity(emuenv->vita_fs_path.string(), total, available)) {
        std::error_code ec;
        const auto space = fs::space(emuenv->vita_fs_path, ec);
        total = ec ? 0 : space.capacity;
        available = ec ? 0 : space.available;
    }
    return dup_string(fmt::format(R"({{"path":{},"custom":{},"freeBytes":{},"totalBytes":{},"note":{}}})",
        json_quote(emuenv->vita_fs_path.string()), ios::using_custom_storage() ? "true" : "false",
        available, total, json_quote(ios::storage_note())));
}

bool v3k_set_storage_folder(const char *folder, char **message) {
    std::string text;
    bool ok = false;
    if (folder) {
        std::lock_guard lock(install_mutex);
        ok = ios::switch_storage(ios::vita_root_for(fs::path(folder)), true, text);
    }
    if (message)
        *message = dup_string(text);
    return ok;
}

bool v3k_use_internal_storage(char **message) {
    std::string text;
    bool ok;
    {
        std::lock_guard lock(install_mutex);
        ok = ios::switch_storage(ios::internal_vita_root(), false, text);
    }
    if (message)
        *message = dup_string(text);
    return ok;
}

void v3k_free_string(char *str) {
    std::free(str);
}

int v3k_install_kind(const char *path) {
    return path ? kind_of(fs::path(path)) : 0;
}

bool v3k_install_zrif(const char *zrif, char **message) {
    auto *emuenv = ios::get_emuenv();
    std::string result = "The emulator is not initialised.";
    bool ok = false;
    if (emuenv && zrif) {
        std::lock_guard lock(install_mutex);
        ok = install_license(*emuenv, trimmed_zrif(zrif), {}, result);
    }
    if (message)
        *message = dup_string(result);
    return ok;
}

char *v3k_zrif_next_to(const char *pkg_path) {
    if (!pkg_path)
        return nullptr;
    const auto zrif = read_zrif(fs::path(pkg_path));
    return zrif.empty() ? nullptr : dup_string(zrif);
}

bool v3k_install(const char *path, const char *zrif, v3k_progress_fn progress, void *context, char **message) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !path) {
        if (message)
            *message = dup_string("The emulator is not initialised.");
        return false;
    }

    InstallOutcome outcome;
    {
        std::lock_guard lock(install_mutex);
        try {
            install_path(*emuenv, fs::path(path), zrif ? zrif : "", progress, context, outcome);
        } catch (const std::exception &e) {
            LOG_ERROR("Install of {} threw: {}", path, e.what());
            outcome.failed++;
        }
        if (outcome.installed > 0)
            app::scan_apps(*emuenv);
    }

    if (message) {
        std::string text;
        if (!outcome.firmware_version.empty())
            text = fmt::format("Firmware {} installed.", outcome.firmware_version);
        else if (!outcome.notes.empty() && outcome.installed == static_cast<int>(outcome.notes.size()))
            text = outcome.notes.front(); // only licenses
        else if (outcome.installed > 0)
            text = outcome.installed == 1 ? "Installed." : fmt::format("Installed {} items.", outcome.installed);
        if (outcome.failed > 0) {
            if (!text.empty())
                text += " ";
            text += outcome.installed == 0 && outcome.failed == 1
                ? (outcome.errors.empty() ? "Installation failed. Check that the file is valid, and that a game or DLC .pkg has its zRIF license." : "")
                : fmt::format("{} item(s) failed.", outcome.failed);
        }
        for (const auto &error : outcome.errors)
            text += (text.empty() ? "" : "\n") + error;
        if (text.empty())
            text = "Nothing to install was found there.";
        *message = dup_string(text);
    }
    return outcome.failed == 0 && outcome.installed > 0;
}

// The memory mapping methods offered on iOS (Settings › Graphics › Memory mapping). External host
// needs 4 KiB host pointer alignment, iOS has 16 KiB, so Vita3K refuses it here; anything unknown
// becomes the default.
static std::string mapping_mode(const std::string &mode) {
    if (mode == "disabled" || mode == "page-table")
        return mode;
    return "double-buffer";
}

char *v3k_settings_json(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return dup_string("{}");
    const auto &cfg = emuenv->cfg;
    const auto b = [](bool v) { return v ? "true" : "false"; };
    const std::string json = fmt::format(
        R"({{"resolutionMultiplier":{},"anisotropicFiltering":{},"audioVolume":{},"screenFilter":{},)"
        R"("vSync":{},"cpuOpt":{},"ngsEnable":{},"highAccuracy":{},"stretchDisplay":{},)"
        R"("performanceOverlay":{},"textureCache":{},"turboMode":{},"pstvMode":{},"memoryMapping":{},"fpsHack":{},"asyncShaders":{},)"
        R"("motion":{},"surfaceSync":{},"showCompileShaders":{},"shaderCache":{},"importTextures":{},"sysButton":{},"sysLang":{},"nativeBuffer":{},"mappingMode":{},)"
        R"("adhocRelay":{},"adhocRelayServer":{},"adhocRelayRoom":{},"adhocRelayKey":{},"userName":{}}})",
        cfg.resolution_multiplier, cfg.anisotropic_filtering, cfg.audio_volume, json_quote(cfg.screen_filter),
        b(cfg.v_sync), b(cfg.cpu_opt), b(cfg.ngs_enable), b(cfg.high_accuracy), b(cfg.stretch_the_display_area),
        b(cfg.performance_overlay), b(cfg.texture_cache), b(cfg.turbo_mode), b(cfg.pstv_mode),
        b(cfg.memory_mapping != "disabled"), b(cfg.fps_hack), b(cfg.async_pipeline_compilation),
        b(!cfg.disable_motion), b(!cfg.disable_surface_sync), b(cfg.show_compile_shaders), b(cfg.shader_cache),
        b(cfg.import_textures), cfg.sys_button, cfg.sys_lang, b(cfg.memory_mapping == "page-table"), json_quote(mapping_mode(cfg.memory_mapping)),
        b(cfg.adhoc_relay), json_quote(cfg.adhoc_relay_server), json_quote(cfg.adhoc_relay_room), json_quote(cfg.adhoc_relay_key), json_quote(emuenv->io.user_name));
    return dup_string(json);
}

namespace {

// Settings are written to both the global config and the active copy, then saved to config.yml
bool save_settings(EmuEnvState &emuenv) {
    return config::serialize_config(emuenv.cfg, emuenv.cfg.config_path) == Success;
}

} // namespace

bool v3k_set_setting_number(const char *key, double value) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !key)
        return false;
    auto &cfg = emuenv->cfg;
    const std::string k = key;
    if (k == "resolution-multiplier") {
        cfg.resolution_multiplier = static_cast<float>(value);
        cfg.current_config.resolution_multiplier = cfg.resolution_multiplier;
    } else if (k == "anisotropic-filtering") {
        cfg.anisotropic_filtering = static_cast<int>(value);
        cfg.current_config.anisotropic_filtering = cfg.anisotropic_filtering;
    } else if (k == "audio-volume") {
        cfg.audio_volume = static_cast<int>(value);
        cfg.current_config.audio_volume = cfg.audio_volume;
    } else if (k == "sys-button") {
        cfg.sys_button = cfg.current_config.sys_button = static_cast<int>(value);
    } else if (k == "sys-lang") {
        cfg.sys_lang = cfg.current_config.sys_lang = static_cast<int>(value);
    } else {
        return false;
    }
    return save_settings(*emuenv);
}

bool v3k_set_setting_bool(const char *key, bool value) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !key)
        return false;
    auto &cfg = emuenv->cfg;
    const std::string k = key;
    if (k == "v-sync") {
        cfg.v_sync = cfg.current_config.v_sync = value;
    } else if (k == "cpu-opt") {
        cfg.cpu_opt = cfg.current_config.cpu_opt = value;
    } else if (k == "ngs-enable") {
        cfg.ngs_enable = cfg.current_config.ngs_enable = value;
    } else if (k == "high-accuracy") {
        cfg.high_accuracy = cfg.current_config.high_accuracy = value;
    } else if (k == "stretch-display") {
        cfg.stretch_the_display_area = cfg.current_config.stretch_the_display_area = value;
    } else if (k == "texture-cache") {
        cfg.texture_cache = cfg.current_config.texture_cache = value;
    } else if (k == "performance-overlay") {
        cfg.performance_overlay = value;
    } else if (k == "turbo-mode") {
        cfg.turbo_mode = value;
    } else if (k == "pstv-mode") {
        cfg.pstv_mode = value;
    } else if (k == "fps-hack") {
        cfg.fps_hack = cfg.current_config.fps_hack = value;
    } else if (k == "async-shaders") {
        cfg.async_pipeline_compilation = cfg.current_config.async_pipeline_compilation = value;
    } else if (k == "show-compile-shaders") {
        cfg.show_compile_shaders = value;
    } else if (k == "shader-cache") {
        cfg.shader_cache = cfg.current_config.shader_cache = value;
    } else if (k == "import-textures") {
        cfg.import_textures = cfg.current_config.import_textures = value;
    } else if (k == "surface-sync") {
        cfg.disable_surface_sync = cfg.current_config.disable_surface_sync = !value;
    } else if (k == "motion") {
        cfg.disable_motion = !value;
    } else if (k == "adhoc-relay") {
        cfg.adhoc_relay = value;
        if (!value)
            net_relay::stop();
    } else if (k == "memory-mapping") {
        // turning mapping on keeps the native buffer if that was chosen
        const bool native = cfg.memory_mapping == "page-table";
        cfg.memory_mapping = cfg.current_config.memory_mapping = value ? (native ? "page-table" : "double-buffer") : "disabled";
    } else if (k == "native-buffer") {
        // Native buffer: the game's memory and the GPU share the same pages (Vita3K's page-table
        // mapping), instead of a GPU copy kept in sync (double buffer). Needs memory mapping on.
        cfg.memory_mapping = cfg.current_config.memory_mapping = value ? "page-table" : "double-buffer";
    } else {
        return false;
    }
    return save_settings(*emuenv);
}

bool v3k_set_setting_string(const char *key, const char *value) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !key || !value)
        return false;
    auto &cfg = emuenv->cfg;
    if (std::string(key) == "screen-filter") {
        cfg.screen_filter = cfg.current_config.screen_filter = value;
        return save_settings(*emuenv);
    }
    if (std::string(key) == "memory-mapping-mode") {
        cfg.memory_mapping = cfg.current_config.memory_mapping = mapping_mode(value);
        return save_settings(*emuenv);
    }
    if (std::string(key) == "user-name") {
        // The PS Vita user name, which games also use as the player's PSN online ID: 3-16 of
        // A-Z a-z 0-9 - _ (anything else is dropped)
        std::string name;
        for (const char c : std::string(value))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                name += c;
        if (name.size() > 16)
            name.resize(16);
        if (name.size() < 3)
            return false;
        if (!app::ensure_current_user(*emuenv))
            return false;
        auto &users = emuenv->app.user_list.users;
        const auto it = users.find(emuenv->io.user_id);
        if (it == users.end())
            return false;
        it->second.name = name;
        app::save_user(*emuenv, emuenv->io.user_id);
        emuenv->io.user_name = name;
        return true;
    }
    if (std::string(key) == "adhoc-relay-server" || std::string(key) == "adhoc-relay-room" || std::string(key) == "adhoc-relay-key") {
        std::string text = value;
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        if (std::string(key) == "adhoc-relay-server")
            cfg.adhoc_relay_server = text;
        else if (std::string(key) == "adhoc-relay-room")
            cfg.adhoc_relay_room = text;
        else
            cfg.adhoc_relay_key = text;
        return save_settings(*emuenv);
    }
    return false;
}

char *v3k_adhoc_relay_status(void) {
    return dup_string(net_relay::status());
}

namespace {
void ensure_binds(EmuEnvState &emuenv) {
    if (emuenv.cfg.controller_binds.size() < 15 || emuenv.cfg.controller_axis_binds.size() < 6)
        app::reset_controller_binding(emuenv);
}
} // namespace

char *v3k_controller_binds_json(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return dup_string("[]");
    ensure_binds(*emuenv);
    std::string json = "[";
    for (size_t i = 0; i < 15; i++)
        json += (i ? "," : "") + std::to_string(emuenv->cfg.controller_binds[i]);
    return dup_string(json + "]");
}

bool v3k_set_controller_bind(int slot, int physical_button) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || slot < 0 || slot >= 15 || physical_button < 0 || physical_button >= 15)
        return false;
    ensure_binds(*emuenv);
    emuenv->cfg.controller_binds[slot] = static_cast<short>(physical_button);
    return save_settings(*emuenv);
}

void v3k_reset_controller_binds(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return;
    app::reset_controller_binding(*emuenv);
    save_settings(*emuenv);
}

const char *v3k_log_path(void) {
    static const std::string path = (fs::path(ios::get_documents_path()) / "vita3k.log").string();
    return path.c_str();
}

void v3k_request_launch(const char *title_id) {
    if (title_id)
        ios::set_pending_launch(title_id);
}

} // extern "C"

// ======================================================================================
// Crash reports, shader cache, per-game settings, save backups, game patches
// ======================================================================================

namespace {

std::optional<app::AppEntry> app_for(const char *title_id) {
    if (!title_id)
        return std::nullopt;
    return ios::find_app_by_title_id(title_id);
}

uint64_t folder_size(const fs::path &folder) {
    std::error_code ec;
    uint64_t total = 0;
    if (!fs::exists(folder, ec))
        return 0;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec))
            total += it->file_size(ec);
    }
    return total;
}

fs::path save_folder(EmuEnvState &emuenv, const app::AppEntry &app) {
    return emuenv.vita_fs_path / "ux0/user" / emuenv.io.user_id / "savedata" / app.savedata;
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H%M%S", std::localtime(&now));
    return buffer;
}

// The settings a game would run with: the global ones plus its custom config file, if any.
// Config can't be copied, so this borrows the global current_config and puts it back.
Config::CurrentConfig effective_config(EmuEnvState &emuenv, const std::string &app_path) {
    const Config::CurrentConfig saved = emuenv.cfg.current_config;
    config::set_current_config(emuenv.cfg, emuenv.config_path, app_path);
    Config::CurrentConfig result = emuenv.cfg.current_config;
    emuenv.cfg.current_config = saved;
    return result;
}

std::string safe_file_name(const std::string &name) {
    std::string out;
    for (const char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') ? c : '_';
    return out.empty() ? std::string("game") : out;
}

} // namespace

char *v3k_take_crash_report(void) {
    const std::string report = ios::crash::take_previous_report();
    if (report.empty())
        return nullptr;
    return dup_string(fmt::format(R"({{"report":{},"log":{}}})", json_quote(report), json_quote(ios::crash::previous_log_path())));
}

// ---- Shader cache ----

double v3k_shader_cache_bytes(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return 0;
    return static_cast<double>(folder_size(emuenv->cache_path / "shaders" / title_id));
}

bool v3k_delete_shader_cache(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    std::error_code ec;
    fs::remove_all(emuenv->cache_path / "shaders" / title_id, ec);
    LOG_INFO("Deleted the shader cache of {}: {}", title_id, ec ? ec.message() : "ok");
    return !ec;
}

// ---- Per-game settings ----

char *v3k_game_settings_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    if (!emuenv || !app)
        return dup_string("{}");
    const Config::CurrentConfig cc = effective_config(*emuenv, app->path);
    const bool custom = config::has_custom_config(emuenv->config_path, app->path);
    const auto b = [](bool v) { return v ? "true" : "false"; };
    return dup_string(fmt::format(
        R"({{"custom":{},"resolutionMultiplier":{},"anisotropicFiltering":{},"screenFilter":{},"memoryMapping":{},)"
        R"("highAccuracy":{},"textureCache":{},"cpuOpt":{},"vSync":{},"fpsHack":{},"disableSurfaceSync":{},)"
        R"("stretchDisplay":{},"widescreen":{},"asyncShaders":{},"shaderCache":{},"nativeBuffer":{},"mappingMode":{}}})",
        b(custom), cc.resolution_multiplier, cc.anisotropic_filtering, json_quote(cc.screen_filter), b(cc.memory_mapping != "disabled"),
        b(cc.high_accuracy), b(cc.texture_cache), b(cc.cpu_opt), b(cc.v_sync), b(cc.fps_hack), b(cc.disable_surface_sync),
        b(cc.stretch_the_display_area), b(cc.widescreen_hack), b(cc.async_pipeline_compilation), b(cc.shader_cache), b(cc.memory_mapping == "page-table"), json_quote(mapping_mode(cc.memory_mapping))));
}

bool v3k_game_uses_async_shaders(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    if (!emuenv || !app)
        return true;
    return effective_config(*emuenv, app->path).async_pipeline_compilation;
}

bool v3k_game_uses_memory_mapping(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    if (!emuenv || !app)
        return false;
    return effective_config(*emuenv, app->path).memory_mapping != "disabled";
}

namespace {
std::mutex session_message_mutex;
std::string session_message;
} // namespace

void v3k_set_session_message(const char *message) {
    const std::lock_guard lock(session_message_mutex);
    session_message = message ? message : "";
}

char *v3k_take_session_message(void) {
    const std::lock_guard lock(session_message_mutex);
    if (session_message.empty())
        return nullptr;
    char *out = dup_string(session_message);
    session_message.clear();
    return out;
}

bool v3k_set_game_setting(const char *title_id, const char *key, const char *value) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    if (!emuenv || !app || !key || !value)
        return false;
    Config::CurrentConfig cc = effective_config(*emuenv, app->path);

    const std::string k = key;
    const std::string v = value;
    const bool on = v == "true" || v == "1";
    if (k == "resolution-multiplier")
        cc.resolution_multiplier = std::stof(v);
    else if (k == "anisotropic-filtering")
        cc.anisotropic_filtering = std::stoi(v);
    else if (k == "screen-filter")
        cc.screen_filter = v;
    else if (k == "memory-mapping")
        cc.memory_mapping = on ? (cc.memory_mapping == "page-table" ? "page-table" : "double-buffer") : "disabled";
    else if (k == "native-buffer")
        cc.memory_mapping = on ? "page-table" : "double-buffer";
    else if (k == "memory-mapping-mode")
        cc.memory_mapping = mapping_mode(v);
    else if (k == "high-accuracy")
        cc.high_accuracy = on;
    else if (k == "texture-cache")
        cc.texture_cache = on;
    else if (k == "cpu-opt")
        cc.cpu_opt = on;
    else if (k == "v-sync")
        cc.v_sync = on;
    else if (k == "fps-hack")
        cc.fps_hack = on;
    else if (k == "disable-surface-sync")
        cc.disable_surface_sync = on;
    else if (k == "stretch-display")
        cc.stretch_the_display_area = on;
    else if (k == "widescreen")
        cc.widescreen_hack = on;
    else if (k == "async-shaders")
        cc.async_pipeline_compilation = on;
    else if (k == "shader-cache")
        cc.shader_cache = on;
    else
        return false;
    return config::save_custom_config(cc, emuenv->config_path, app->path);
}

bool v3k_reset_game_settings(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    if (!emuenv || !app)
        return false;
    return config::delete_custom_config(emuenv->config_path, app->path);
}

// ---- Save data backups ----

char *v3k_export_save_data(const char *title_id, char **message) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    const auto fail = [&](const std::string &text) -> char * {
        if (message)
            *message = dup_string(text);
        return nullptr;
    };
    if (!emuenv || !app)
        return fail("This game is not installed.");
    const fs::path source = save_folder(*emuenv, *app);
    std::error_code ec;
    if (app->savedata.empty() || !fs::exists(source, ec) || fs::is_empty(source, ec))
        return fail(app->title + " has no save data yet.");

    const fs::path exports = fs::path(ios::get_documents_path()) / "Save Backups";
    fs::create_directories(exports, ec);
    const fs::path zip_path = exports / fmt::format("{} ({}) {}.zip", safe_file_name(app->title), app->title_id, timestamp());

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0))
        return fail("Could not create the backup file.");
    bool ok = true;
    for (auto it = fs::recursive_directory_iterator(source, ec); ok && !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = fs::relative(it->path(), source, ec).generic_string();
        ok = mz_zip_writer_add_file(&zip, name.c_str(), it->path().string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok) {
        fs::remove(zip_path, ec);
        return fail("Could not write the backup.");
    }
    LOG_INFO("Exported the save data of {} to {}", app->title_id, zip_path.string());
    if (message)
        *message = dup_string("Saved to Files › Hinoki › Save Backups.");
    return dup_string(zip_path.string());
}

namespace {

bool is_save_junk(const std::string &name) {
    const std::string lower = string_utils::tolower(name);
    return lower == "__macosx" || lower.starts_with("._") || lower == ".ds_store" || lower == "thumbs.db" || lower == "desktop.ini";
}

// A Vita title ID: four capital letters and five digits (PCSE00120)
bool looks_like_title_id(const std::string &name) {
    return name.size() == 9 && std::all_of(name.begin(), name.begin() + 4, [](unsigned char c) { return std::isupper(c); })
        && std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isdigit(c); });
}

// Unpacks a zip into a folder: either slash, junk skipped, nothing outside the folder
bool unpack_save_zip(const fs::path &zip_path, const fs::path &out, std::string &error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
        error = "That file is not a readable zip archive.";
        return false;
    }
    const fs::path base = out.lexically_normal();
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    for (mz_uint i = 0; ok && i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        const fs::path relative = fs::path(name).lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            continue;
        if (std::any_of(relative.begin(), relative.end(), [](const fs::path &part) { return is_save_junk(part.string()); }))
            continue;
        const fs::path file = base / relative;
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        ok = mz_zip_reader_extract_to_file(&zip, i, file.string().c_str(), 0);
        if (!ok)
            error = fmt::format("{} could not be extracted: the archive may be damaged.", name);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// Where the save itself sits in what the player picked. Exports come in many shapes: the save's own
// files (sce_sys/ and the game's files), its folder (PCSE00120/), a savedata/ folder with several
// games, or a whole ux0 (ux0/user/00/savedata/PCSE00120/), possibly wrapped in one more folder.
fs::path find_save_root(const fs::path &source, const std::vector<std::string> &names, std::string &other_title) {
    std::error_code ec;
    // A folder named after the game's save, anywhere a few levels down
    std::vector<std::pair<fs::path, int>> queue{ { source, 0 } };
    for (size_t i = 0; i < queue.size(); i++) {
        const auto [folder, depth] = queue[i];
        for (const auto &entry : fs::directory_iterator(folder, ec)) {
            if (!entry.is_directory(ec) || is_save_junk(entry.path().filename().string()))
                continue;
            const std::string name = entry.path().filename().string();
            for (const auto &wanted : names) {
                if (string_utils::tolower(name) == string_utils::tolower(wanted))
                    return entry.path();
            }
            if (depth < 6)
                queue.push_back({ entry.path(), depth + 1 });
        }
    }

    // No such folder: the picked folder is the save, or wraps it in folders without files beside them
    fs::path root = source;
    for (int depth = 0; depth < 6; depth++) {
        if (fs::exists(root / "sce_sys", ec))
            break;
        std::vector<fs::path> folders;
        bool files = false;
        for (const auto &entry : fs::directory_iterator(root, ec)) {
            if (is_save_junk(entry.path().filename().string()))
                continue;
            if (entry.is_directory(ec))
                folders.push_back(entry.path());
            else
                files = true;
        }
        if (files || folders.size() != 1)
            break;
        root = folders.front();
    }
    // Another game's save folder (or a savedata/ folder without this game in it)
    const std::string root_name = root.filename().string();
    if (looks_like_title_id(root_name)) {
        other_title = root_name;
        return {};
    }
    return root;
}

size_t count_files(const fs::path &folder) {
    size_t files = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec))
            files++;
    }
    return files;
}

} // namespace

bool v3k_import_save_data(const char *title_id, const char *path, char **message) {
    auto *emuenv = ios::get_emuenv();
    const auto app = app_for(title_id);
    const auto result = [&](bool ok, const std::string &text) {
        if (message)
            *message = dup_string(text);
        return ok;
    };
    if (!emuenv || !app || !path)
        return result(false, "This game is not installed.");
    if (app->savedata.empty())
        return result(false, app->title + " does not use save data.");

    std::lock_guard lock(install_mutex);
    std::error_code ec;
    const fs::path picked(path);
    const fs::path staging = emuenv->cache_path / "save-import";
    fs::remove_all(staging, ec);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{ staging };

    // A zip is unpacked first; a folder is read where it is
    fs::path source = picked;
    if (!fs::is_directory(picked, ec)) {
        std::string error;
        if (!unpack_save_zip(picked, staging / "unpacked", error))
            return result(false, error);
        source = staging / "unpacked";
    }

    std::vector<std::string> names{ app->savedata };
    if (app->title_id != app->savedata)
        names.push_back(app->title_id);
    std::string other_title;
    const fs::path root = find_save_root(source, names, other_title);
    if (root.empty())
        return result(false, fmt::format("This is the save of {}, not of {} ({}). Pick the save of this game.", other_title, app->title, app->savedata));
    if (!fs::exists(root, ec) || fs::is_empty(root, ec))
        return result(false, "No save data was found in it.");

    // Saves copied straight off a Vita are encrypted for that console and can't be read here
    if (fs::exists(root / "sce_pfs", ec) || fs::exists(root / "sce_sys/sealedkey", ec))
        return result(false, "This save is still encrypted for the Vita it came from. Export it decrypted on the Vita (VitaShell: \"Open decrypted\" on the save folder, then copy; or Vita Save Manager / Apollo), then import that.");

    // Copied next to the current save first, then swapped in, so a failed copy changes nothing
    const fs::path target = save_folder(*emuenv, *app);
    const fs::path incoming = target.parent_path() / (app->savedata + ".importing");
    fs::remove_all(incoming, ec);
    fs::create_directories(incoming, ec);
    ec.clear();
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (is_save_junk(entry.path().filename().string()))
            continue;
        fs::copy(entry.path(), incoming / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
            break;
    }
    if (ec) {
        const std::string reason = ec.message();
        fs::remove_all(incoming, ec);
        return result(false, "The save could not be copied: " + reason);
    }

    // The current save is kept where the player can find it: Files › Vita3KiOS › Save Backups
    std::string kept;
    if (fs::exists(target, ec) && !fs::is_empty(target, ec)) {
        const fs::path backups = fs::path(ios::get_documents_path()) / "Save Backups";
        const fs::path backup = backups / fmt::format("{} ({}) before import {}", safe_file_name(app->title), app->title_id, timestamp());
        fs::create_directories(backups, ec);
        fs::copy(target, backup, fs::copy_options::recursive, ec);
        if (ec) {
            const std::string reason = ec.message();
            fs::remove_all(incoming, ec);
            return result(false, "The current save could not be backed up, so nothing was changed: " + reason);
        }
        kept = " The previous save is in Files › Hinoki › Save Backups.";
    }
    fs::remove_all(target, ec);
    fs::rename(incoming, target, ec);
    if (ec)
        return result(false, "The save could not be put in place: " + ec.message());

    const size_t files = count_files(target);
    LOG_INFO("Imported the save data of {} from {} ({} files)", app->title_id, picked.string(), files);
    return result(true, fmt::format("Save data imported ({} file{}).{}", files, files == 1 ? "" : "s", kept));
}

// ---- Game patches and cheats (github.com/Vita3K/vita3k-patches) ----

namespace {

constexpr const char *PATCH_REPO_ZIP = "https://codeload.github.com/Vita3K/vita3k-patches/zip/refs/heads/main";

std::string zip_entry_text(const std::string &zip_data, const std::string &suffix) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zip_data.data(), zip_data.size(), 0))
        return {};
    std::string text;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (!std::string(name).ends_with(suffix))
            continue;
        size_t size = 0;
        if (void *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0)) {
            text.assign(static_cast<const char *>(data), size);
            mz_free(data);
        }
        break;
    }
    mz_zip_reader_end(&zip);
    return text;
}

bool version_matches(const std::string &wanted, const std::string &have) {
    if (wanted.empty())
        return true;
    return patch_versions_equal(wanted, have);
}

} // namespace

bool v3k_download_patches(char **message) {
    auto *emuenv = ios::get_emuenv();
    const auto result = [&](bool ok, const std::string &text) {
        if (message)
            *message = dup_string(text);
        return ok;
    };
    if (!emuenv)
        return result(false, "The emulator is not ready.");

    std::string zip_data, error;
    if (!ios::download(PATCH_REPO_ZIP, zip_data, error) || zip_data.empty()) {
        LOG_ERROR("Patch and cheat download failed: {}", error);
        return result(false, "Could not download patches and cheats (" + error + "). Check your connection and try again.");
    }

    // Patches: patch/patchlist.xml
    const std::string xml = zip_entry_text(zip_data, "/patch/patchlist.xml");
    PatchEntries entries;
    if (xml.empty() || !parse_patch_entries(xml, entries))
        return result(false, "The downloaded patch list is not valid.");
    std::error_code ec;
    fs::create_directories(emuenv->patch_path, ec);
    if (!fs_utils::dump_data(patchlist_xml_path(emuenv->patch_path), xml.data(), static_cast<std::streamsize>(xml.size())))
        return result(false, "Could not save the patch list.");
    std::set<std::string> patched_games;
    for (const auto &entry : entries)
        patched_games.insert(entry.title);

    // Cheats: cheat/*.psv
    const int cheat_files = ios::cheats::install_database(emuenv->patch_path, zip_data, error);
    LOG_INFO("Downloaded {} patches for {} games and {} cheat files", entries.size(), patched_games.size(), cheat_files);
    return result(true, fmt::format("Downloaded {} patches for {} games, and {} cheat files.", entries.size(), patched_games.size(), cheat_files));
}

char *v3k_patches_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return dup_string("[]");
    const auto enabled = load_enabled_patches(emuenv->patch_path);
    const std::string id = title_id;
    const auto app = ios::find_app_by_title_id(id);
    const std::string game_version = app ? app->app_ver : std::string();
    std::string json = "[";
    bool first = true;
    for (const auto &entry : load_patch_entries(emuenv->patch_path)) {
        if (std::find(entry.title_ids.begin(), entry.title_ids.end(), id) == entry.title_ids.end())
            continue;
        json += fmt::format(R"({}{{"key":{},"name":{},"author":{},"note":{},"version":{},"appVersion":{},"enabled":{},"versionMatches":{}}})",
            first ? "" : ",", json_quote(entry.key()), json_quote(entry.name.empty() ? entry.title : entry.name), json_quote(entry.author),
            json_quote(entry.note), json_quote(entry.patch_ver), json_quote(entry.app_ver), enabled.contains(entry.key()) ? "true" : "false",
            version_matches(entry.app_ver, game_version) ? "true" : "false");
        first = false;
    }
    json += "]";
    return dup_string(json);
}

bool v3k_patches_downloaded(void) {
    auto *emuenv = ios::get_emuenv();
    std::error_code ec;
    return emuenv && fs::exists(patchlist_xml_path(emuenv->patch_path), ec);
}

bool v3k_set_patch_enabled(const char *key, bool enabled) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !key)
        return false;
    auto set = load_enabled_patches(emuenv->patch_path);
    if (enabled)
        set.insert(key);
    else
        set.erase(key);
    return save_enabled_patches(emuenv->patch_path, set);
}

char *v3k_all_patches_json(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return dup_string("[]");
    const auto enabled = load_enabled_patches(emuenv->patch_path);
    const auto installed = ios::list_apps();
    // One group per game (entries of one game share their title)
    std::vector<std::string> order;
    std::map<std::string, std::vector<const PatchEntry *>> groups;
    const auto entries = load_patch_entries(emuenv->patch_path);
    for (const auto &entry : entries) {
        if (!groups.contains(entry.title))
            order.push_back(entry.title);
        groups[entry.title].push_back(&entry);
    }
    std::sort(order.begin(), order.end());
    std::string json = "[";
    bool first_group = true;
    for (const auto &title : order) {
        const auto &group = groups[title];
        std::set<std::string> ids;
        for (const auto *entry : group)
            ids.insert(entry->title_ids.begin(), entry->title_ids.end());
        const app::AppEntry *owned = nullptr;
        for (const auto &app : installed) {
            if (ids.contains(app.title_id)) {
                owned = &app;
                break;
            }
        }
        std::string id_list;
        for (const auto &id : ids)
            id_list += (id_list.empty() ? "" : ",") + json_quote(id);
        std::string patches;
        for (const auto *entry : group) {
            patches += fmt::format(R"({}{{"key":{},"name":{},"author":{},"note":{},"version":{},"appVersion":{},"enabled":{},"versionMatches":{}}})",
                patches.empty() ? "" : ",", json_quote(entry->key()), json_quote(entry->name), json_quote(entry->author), json_quote(entry->note),
                json_quote(entry->patch_ver), json_quote(entry->app_ver), enabled.contains(entry->key()) ? "true" : "false",
                owned && version_matches(entry->app_ver, owned->app_ver) ? "true" : "false");
        }
        json += fmt::format(R"({}{{"title":{},"titleIds":[{}],"installedTitleId":{},"installedVersion":{},"patches":[{}]}})",
            first_group ? "" : ",", json_quote(title), id_list, json_quote(owned ? owned->title_id : ""), json_quote(owned ? owned->app_ver : ""), patches);
        first_group = false;
    }
    json += "]";
    return dup_string(json);
}

// ---- Cheats ----

bool v3k_cheats_downloaded(void) {
    auto *emuenv = ios::get_emuenv();
    return emuenv && ios::cheats::database_installed(emuenv->patch_path);
}

char *v3k_cheats_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return dup_string("[]");
    const auto app = app_for(title_id);
    const std::string game_version = app ? app->app_ver : std::string();
    std::string json = "[";
    for (const auto &cheat : ios::cheats::list(emuenv->patch_path, title_id)) {
        json += fmt::format(R"({}{{"key":{},"name":{},"author":{},"file":{},"gameVersion":{},"supported":{},"enabled":{},"versionMatches":{}}})",
            json.size() > 1 ? "," : "", json_quote(cheat.key), json_quote(cheat.name), json_quote(cheat.author), json_quote(cheat.file),
            json_quote(cheat.game_version), cheat.supported ? "true" : "false", cheat.enabled ? "true" : "false",
            version_matches(cheat.game_version, game_version) ? "true" : "false");
    }
    json += "]";
    return dup_string(json);
}

bool v3k_set_cheat_enabled(const char *title_id, const char *key, bool enabled) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id || !key)
        return false;
    return ios::cheats::set_enabled(emuenv->patch_path, title_id, key, enabled);
}

char *v3k_cheats_folder(void) {
    auto *emuenv = ios::get_emuenv();
    return dup_string(emuenv ? (emuenv->patch_path / "cheats").string() : std::string());
}

// ---- Mods (rePatch layout: ux0/rePatch/<title ID>/ replaces files of the game) ----

namespace {

fs::path mod_folder(EmuEnvState &emuenv, const std::string &title_id, bool enabled) {
    return emuenv.vita_fs_path / "ux0" / (enabled ? "rePatch" : "rePatch-disabled") / title_id;
}

} // namespace

char *v3k_mod_info_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return dup_string("{}");
    std::error_code ec;
    const fs::path on = mod_folder(*emuenv, title_id, true);
    const fs::path off = mod_folder(*emuenv, title_id, false);
    const bool disabled = !fs::exists(on, ec) && fs::exists(off, ec);
    const fs::path folder = disabled ? off : on;
    int files = 0;
    if (fs::exists(folder, ec)) {
        for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            files += it->is_regular_file(ec) ? 1 : 0;
    }
    return dup_string(fmt::format(R"({{"path":{},"files":{},"bytes":{},"enabled":{}}})",
        json_quote(on.string()), files, folder_size(folder), disabled ? "false" : "true"));
}

bool v3k_import_mod(const char *title_id, const char *zip_path, char **message) {
    auto *emuenv = ios::get_emuenv();
    const auto result = [&](bool ok, const std::string &text) {
        if (message)
            *message = dup_string(text);
        return ok;
    };
    if (!emuenv || !title_id || !zip_path)
        return result(false, "This game is not installed.");
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path, 0))
        return result(false, "That file is not a zip archive.");

    const std::string id = title_id;
    const fs::path target = mod_folder(*emuenv, id, true);
    std::error_code ec;
    // A disabled mod folder comes back on, so the new files join it
    if (fs::exists(mod_folder(*emuenv, id, false), ec) && !fs::exists(target, ec)) {
        fs::create_directories(target.parent_path(), ec);
        fs::rename(mod_folder(*emuenv, id, false), target, ec);
    }
    fs::create_directories(target, ec);

    // Mods come as rePatch/<ID>/..., <ID>/..., or just the game's files: strip the wrappers
    const auto relative_name = [&](std::string name) {
        for (const std::string &prefix : { std::string("ux0/"), std::string("rePatch/"), std::string("repatch/"), id + "/" }) {
            const auto at = name.find(prefix);
            if (at != std::string::npos && (at == 0 || name[at - 1] == '/'))
                name = name.substr(at + prefix.size());
        }
        return name;
    };

    int written = 0;
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; ok && i < count; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        char name[1024];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        const std::string entry = relative_name(name);
        if (entry.empty() || entry.find("..") != std::string::npos || entry.starts_with("__MACOSX"))
            continue;
        const fs::path out = target / entry;
        fs::create_directories(out.parent_path(), ec);
        ok = mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0);
        written += ok ? 1 : 0;
    }
    mz_zip_reader_end(&zip);
    LOG_INFO("Installed a mod for {}: {} files", id, written);
    if (!ok)
        return result(false, "Some files of the mod could not be extracted.");
    return result(true, fmt::format("Mod installed ({} files). It applies the next time the game starts.", written));
}

bool v3k_set_mods_enabled(const char *title_id, bool enabled) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    const fs::path from = mod_folder(*emuenv, title_id, !enabled);
    const fs::path to = mod_folder(*emuenv, title_id, enabled);
    std::error_code ec;
    if (!fs::exists(from, ec))
        return true;
    fs::create_directories(to.parent_path(), ec);
    fs::remove_all(to, ec);
    fs::rename(from, to, ec);
    return !ec;
}

bool v3k_delete_mods(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    std::error_code ec;
    fs::remove_all(mod_folder(*emuenv, title_id, true), ec);
    fs::remove_all(mod_folder(*emuenv, title_id, false), ec);
    return !ec;
}

void v3k_log_ui(const char *message) {
    if (message)
        LOG_INFO("{}", message);
}

void v3k_set_fast_cpu(bool enabled) {
    set_ios_fast_cpu(enabled);
}

char *v3k_perf_json(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return dup_string("{}");
    const auto stats = ios::process_stats();
    const auto &cc = emuenv->cfg.current_config;
    const uint32_t compiled = emuenv->renderer ? emuenv->renderer->shaders_count_compiled : 0;
    return dup_string(fmt::format(
        R"({{"fps":{},"avgFps":{},"minFps":{},"maxFps":{},"frameMs":{},"cpu":{:.0f},"memoryUsed":{},"memoryAvailable":{},)"
        R"("resolutionScale":{},"width":{},"height":{},"shadersCompiled":{}}})",
        emuenv->fps, emuenv->avg_fps, emuenv->min_fps, emuenv->max_fps, emuenv->ms_per_frame, stats.cpu_percent,
        stats.memory_used, stats.memory_available, cc.resolution_multiplier,
        static_cast<int>(960 * cc.resolution_multiplier), static_cast<int>(544 * cc.resolution_multiplier), compiled));
}

char *v3k_pkg_info_json(const char *path) {
    auto *emuenv = ios::get_emuenv();
    PkgInfo info;
    if (!emuenv || !path || !read_pkg_info(fs::path(path), info))
        return nullptr;
    const std::string zrif = find_title_zrif(emuenv->vita_fs_path, info.title_id, info.content_id);
    std::error_code ec;
    const bool game_installed = fs::exists(emuenv->vita_fs_path / "ux0/app" / info.title_id, ec);
    return dup_string(fmt::format(R"({{"kind":{},"titleId":{},"title":{},"contentId":{},"zrif":{},"gameInstalled":{}}})",
        json_quote(info.kind), json_quote(info.title_id), json_quote(info.title), json_quote(info.content_id), json_quote(zrif),
        game_installed ? "true" : "false"));
}

char *v3k_game_content_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return dup_string("{}");
    std::error_code ec;
    const auto patch_dir = emuenv->vita_fs_path / "ux0/patch" / title_id;
    std::string update_version;
    if (fs::exists(patch_dir / "sce_sys/param.sfo", ec)) {
        std::vector<uint8_t> content;
        if (fs_utils::read_data(patch_dir / "sce_sys/param.sfo", content)) {
            SfoFile sfo_file;
            if (sfo::load(sfo_file, content))
                sfo::get_data_by_key(update_version, sfo_file, "APP_VER");
        }
        if (update_version.empty())
            update_version = "installed";
    }
    int dlc = 0;
    const auto addcont_dir = emuenv->vita_fs_path / "ux0/addcont" / title_id;
    if (fs::is_directory(addcont_dir, ec)) {
        for (const auto &entry : fs::directory_iterator(addcont_dir, ec))
            dlc += entry.is_directory(ec) ? 1 : 0;
    }
    return dup_string(fmt::format(R"({{"update":{},"updateBytes":{},"dlc":{},"dlcBytes":{}}})",
        json_quote(update_version), folder_size(patch_dir), dlc, folder_size(addcont_dir)));
}

bool v3k_delete_game_content(const char *title_id, bool update) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    std::error_code ec;
    fs::remove_all(emuenv->vita_fs_path / "ux0" / (update ? "patch" : "addcont") / title_id, ec);
    LOG_INFO("Deleted the {} of {}: {}", update ? "update" : "DLC", title_id, ec ? ec.message() : "ok");
    return !ec;
}

// ---- Compatibility list, game fixes list, problem reports ----

namespace {
constexpr const char *COMPAT_DB_ZIP = "https://github.com/Vita3K/compatibility/releases/download/compat_db/app_compat_db.xml.zip";
constexpr const char *GAME_FIXES_URL = "https://raw.githubusercontent.com/oledz-i/Hinoki/main/dist/game-fixes.txt";
} // namespace

bool v3k_update_compat_db(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return false;
    std::string data, error;
    if (!ios::download(COMPAT_DB_ZIP, data, error) || data.empty()) {
        LOG_WARN("Compatibility list download failed: {}", error);
        return false;
    }
    const std::lock_guard lock(compat_mutex);
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    const bool ok = compat::install_db(emuenv->compat, fs::path(emuenv->cache_path.string()), bytes, "");
    LOG_INFO("Compatibility list {}: {} games", ok ? "updated" : "could not be installed", emuenv->compat.app_compat_db.size());
    return ok;
}

bool v3k_update_game_fixes(void) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv)
        return false;
    std::string data, error;
    if (!ios::download(GAME_FIXES_URL, data, error) || data.empty() || data.size() > 1024 * 1024 || data.find('\0') != std::string::npos) {
        LOG_WARN("Game fixes list download failed: {}", error.empty() ? "not a text file" : error);
        return false;
    }
    const fs::path file = fs::path(emuenv->cache_path.string()) / "game-fixes.txt";
    const fs::path temp = file.string() + ".new";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!fs_utils::dump_data(temp, data.data(), static_cast<std::streamsize>(data.size())))
        return false;
    fs::rename(temp, file, ec);
    return !ec;
}

char *v3k_game_fix_json(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return nullptr;
    app::GameFix fix;
    if (!app::find_game_fix(fs::path(emuenv->cache_path.string()) / "game-fixes.txt", title_id, fix))
        return nullptr;
    std::string settings;
    for (const auto &[key, value] : fix.settings)
        settings += (settings.empty() ? "" : ", ") + key + " " + value;
    // Whether the game's settings already are the recommended ones (so the screen can say "applied")
    bool matches = true;
    if (const auto app = app_for(title_id)) {
        const Config::CurrentConfig cc = effective_config(*emuenv, app->path);
        for (const auto &[key, value] : fix.settings) {
            const bool on = value == "on" || value == "true" || value == "1" || value == "yes";
            if (key == "memory-mapping")
                matches &= (cc.memory_mapping != "disabled") == on;
            else if (key == "native-buffer")
                matches &= (cc.memory_mapping == "page-table") == on;
            else if (key == "surface-sync")
                matches &= !cc.disable_surface_sync == on;
            else if (key == "async-shaders")
                matches &= cc.async_pipeline_compilation == on;
            else if (key == "high-accuracy")
                matches &= cc.high_accuracy == on;
            else if (key == "texture-cache")
                matches &= cc.texture_cache == on;
            else if (key == "shader-cache")
                matches &= cc.shader_cache == on;
            else if (key == "fps-hack")
                matches &= cc.fps_hack == on;
            else if (key == "cpu-opt")
                matches &= cc.cpu_opt == on;
            else if (key == "v-sync")
                matches &= cc.v_sync == on;
            else if (key == "stretch")
                matches &= cc.stretch_the_display_area == on;
            else if (key == "screen-filter")
                matches &= cc.screen_filter == value;
        }
    } else
        matches = false;
    return dup_string(fmt::format(R"({{"note":{},"settings":{},"matches":{}}})", json_quote(fix.note), json_quote(settings), matches ? "true" : "false"));
}

bool v3k_apply_game_fix(const char *title_id) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id)
        return false;
    app::GameFix fix;
    if (!app::find_game_fix(fs::path(emuenv->cache_path.string()) / "game-fixes.txt", title_id, fix))
        return false;
    bool ok = true;
    for (const auto &[key, value] : fix.settings) {
        const bool on = value == "on" || value == "true" || value == "1" || value == "yes";
        const char *flag = on ? "true" : "false";
        if (key == "memory-mapping" || key == "native-buffer" || key == "high-accuracy" || key == "async-shaders" || key == "texture-cache"
            || key == "shader-cache" || key == "fps-hack" || key == "cpu-opt" || key == "v-sync")
            ok &= v3k_set_game_setting(title_id, key.c_str(), flag);
        else if (key == "surface-sync")
            ok &= v3k_set_game_setting(title_id, "disable-surface-sync", on ? "false" : "true");
        else if (key == "stretch")
            ok &= v3k_set_game_setting(title_id, "stretch-display", flag);
        else if (key == "resolution")
            ok &= v3k_set_game_setting(title_id, "resolution-multiplier", value.c_str());
        else if (key == "screen-filter")
            ok &= v3k_set_game_setting(title_id, "screen-filter", value.c_str());
    }
    LOG_INFO("Applied the recommended settings to {}'s own settings", title_id);
    return ok;
}

int v3k_game_fix_flag(const char *title_id, const char *key) {
    auto *emuenv = ios::get_emuenv();
    if (!emuenv || !title_id || !key)
        return -1;
    app::GameFix fix;
    if (app::find_game_fix(fs::path(emuenv->cache_path.string()) / "game-fixes.txt", title_id, fix)) {
        for (const auto &[k, v] : fix.settings) {
            if (k == key)
                return (v == "on" || v == "true" || v == "1" || v == "yes") ? 1 : 0;
        }
    }
    return -1;
}

char *v3k_make_problem_report(char **message) {
    auto *emuenv = ios::get_emuenv();
    const fs::path folder = fs::path(v3k_log_path()).parent_path();
    const fs::path zip_path = fs::temp_directory_path() / fmt::format("Hinoki-report-{}.zip", std::time(nullptr));
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0)) {
        if (message)
            *message = dup_string("Could not create the report file.");
        return nullptr;
    }
    // Make sure what was logged so far is in the file
    if (auto logger = spdlog::default_logger())
        logger->flush();
    for (const char *name : { "vita3k.log", "vita3k-previous.log", "last-crash-report.txt" }) {
        const fs::path file = folder / name;
        std::error_code ec;
        if (fs::exists(file, ec))
            mz_zip_writer_add_file(&zip, name, file.string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
    }
    std::string info = fmt::format("Hinoki {}\nDevice: {}\niOS: {}\nJIT: {}\n", v3k_version(), ios::get_device_model(), ios::get_os_version(),
        v3k_is_jit_enabled() ? "enabled" : "not enabled");
    if (char *settings = v3k_settings_json()) {
        info += std::string("Settings: ") + settings + "\n";
        std::free(settings);
    }
    if (emuenv) {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(fs::path(emuenv->config_path.string()) / "config", ec)) {
            if (entry.path().extension() == ".xml") {
                const std::string name = "game-settings/" + entry.path().filename().string();
                mz_zip_writer_add_file(&zip, name.c_str(), entry.path().string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
            }
        }
    }
    mz_zip_writer_add_mem(&zip, "device.txt", info.data(), info.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return dup_string(zip_path.string());
}
