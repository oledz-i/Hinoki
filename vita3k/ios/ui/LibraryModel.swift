// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import SwiftUI
import UIKit

struct GameApp: Codable, Identifiable, Hashable {
    let titleId: String
    let title: String
    let version: String
    let category: String
    let iconPath: String
    let path: String
    let lastPlayed: Int64
    let playTime: Int64
    let customConfig: Bool
    /// Vita3K compatibility list: -1 unknown, 0 nothing ... 6 playable
    let compat: Int
    let compatIssue: Int

    var id: String { titleId }

    var compatibility: Compatibility? { Compatibility(rawValue: compat) }

    var icon: UIImage? {
        iconPath.isEmpty ? nil : UIImage(contentsOfFile: iconPath)
    }
}

/// States of the Vita3K compatibility list (github.com/Vita3K/compatibility)
enum Compatibility: Int {
    case nothing = 0, bootable, intro, menu, ingameLess, ingameMore, playable

    var label: String {
        switch self {
        case .nothing: "Nothing"
        case .bootable: "Boots"
        case .intro: "Intro"
        case .menu: "Menus"
        case .ingameLess: "In game −"
        case .ingameMore: "In game +"
        case .playable: "Playable"
        }
    }

    var detail: String {
        switch self {
        case .nothing: "Doesn't start in Vita3K yet"
        case .bootable: "Starts, but doesn't get past a black screen"
        case .intro: "Shows its intro, then stops"
        case .menu: "Reaches the menus but not gameplay"
        case .ingameLess: "Reaches gameplay with major problems"
        case .ingameMore: "Gameplay works with some problems"
        case .playable: "Playable from start to finish"
        }
    }

    var color: Color {
        switch self {
        case .nothing, .bootable: .red
        case .intro, .menu: .orange
        case .ingameLess: .yellow
        case .ingameMore: .mint
        case .playable: .green
        }
    }
}

struct EmulatorSettings: Codable {
    var resolutionMultiplier: Double = 1
    var anisotropicFiltering: Int = 1
    var audioVolume: Int = 100
    var screenFilter: String = "Bilinear"
    var vSync: Bool = true
    var cpuOpt: Bool = true
    var ngsEnable: Bool = true
    var highAccuracy: Bool = false
    var stretchDisplay: Bool = false
    var performanceOverlay: Bool = false
    var textureCache: Bool = true
    var turboMode: Bool = false
    var pstvMode: Bool = false
    var memoryMapping: Bool = true
    var fpsHack: Bool = false
    var asyncShaders: Bool = true
    var motion: Bool = true
    var surfaceSync: Bool = false
    var showCompileShaders: Bool = true
    var shaderCache: Bool = true
    var importTextures: Bool = false
    var sysButton: Int = 1
    var sysLang: Int = 1
    var nativeBuffer: Bool = false
    var mappingMode: String = "double-buffer"
    var adhocRelay: Bool = false
    var adhocRelayServer: String = ""
    var adhocRelayRoom: String = ""
    var adhocRelayKey: String = ""
    var userName: String = ""
}

struct StorageInfo: Codable {
    var path: String = ""
    var custom: Bool = false
    var freeBytes: UInt64 = 0
    var totalBytes: UInt64 = 0
    var note: String = ""

    var freeText: String {
        ByteCountFormatter.string(fromByteCount: Int64(freeBytes), countStyle: .file) + " free"
    }

    /// "81 GB of 128 GB used", like Settings › General › iPhone Storage
    var usedText: String {
        let used = totalBytes > freeBytes ? totalBytes - freeBytes : 0
        return ByteCountFormatter.string(fromByteCount: Int64(used), countStyle: .file) + " of "
            + ByteCountFormatter.string(fromByteCount: Int64(totalBytes), countStyle: .file) + " used"
    }
}

struct CrashReport: Codable, Identifiable {
    let report: String
    let log: String
    var id: String { report }
}

struct PackageInfo: Decodable {
    let kind: String
    let titleId: String
    let title: String
    let contentId: String
    let zrif: String
    let gameInstalled: Bool
}

/// One file or folder waiting to be installed
struct PendingImport: Identifiable {
    let id = UUID()
    let url: URL
    var zrif: String = ""
    /// For a .pkg: "game", "update", "dlc", "theme" or "unknown"
    var packageKind = ""
    var packageTitle = ""

    var name: String { url.lastPathComponent }
    var kind: Int32 { v3k_install_kind(url.path) }
    var isPackage: Bool { kind == 3 }
}

struct InstallProgress {
    var name: String
    var percent: Double
    var remaining: Int
}

/// Boxes a progress handler so it can travel through the C callback's context pointer
private final class ProgressBox {
    let handler: (Double) -> Void
    init(_ handler: @escaping (Double) -> Void) { self.handler = handler }
}

func takeString(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
    guard let pointer else { return nil }
    defer { v3k_free_string(pointer) }
    return String(cString: pointer)
}

@MainActor
final class LibraryModel: ObservableObject {
    @Published var apps: [GameApp] = []
    @Published var firmwareState: Int32 = 0
    @Published var jitEnabled = false
    @Published var jitStatus = ""
    @Published var jitPreparing = false
    @Published var jitCanRequest = false
    @Published var jitProgress = 0
    @Published var jitTarget = 0
    @Published var settingUpJIT = false
    @Published var settings = EmulatorSettings()
    @Published var storage = StorageInfo()
    @Published var storageBusy = false

    @Published var installProgress: InstallProgress?
    @Published var lastMessage: String?
    @Published var packageNeedingLicense: PendingImport?
    /// Set to ask the library to launch a game (from the game settings sheet)
    @Published var launchRequested: GameApp?
    /// How the previous session ended, if it crashed (shown once)
    @Published var crashReport: CrashReport?

    private var queue: [PendingImport] = []
    private var installing = false

    var hasFirmware: Bool { firmwareState & 2 != 0 }
    var hasFonts: Bool { firmwareState & 4 != 0 }
    var version: String { String(cString: v3k_version()) }
    var storagePath: String { String(cString: v3k_storage_path()) }

    /// Only the JIT fields, and only the ones that changed: polled while JIT is prepared. Publishing
    /// unchanged values (or the whole game list) made SwiftUI redraw the home screen 4 times a second
    func reloadJIT() {
        func set<T: Equatable>(_ path: ReferenceWritableKeyPath<LibraryModel, T>, _ value: T) {
            if self[keyPath: path] != value { self[keyPath: path] = value }
        }
        let wasPreparing = jitPreparing
        set(\.jitEnabled, v3k_is_jit_enabled())
        set(\.jitPreparing, v3k_jit_preparing())
        set(\.jitCanRequest, v3k_jit_can_request())
        set(\.jitProgress, Int(v3k_jit_progress()))
        set(\.jitTarget, Int(v3k_jit_target()))
        set(\.jitStatus, takeString(v3k_jit_status_text()) ?? "")
        if jitPreparing && !settingUpJIT && !jitPolling {
            // JIT is being set up in the background: look again shortly
            jitPolling = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
                self?.jitPolling = false
                self?.reloadJIT()
            }
        } else if wasPreparing && !jitPreparing {
            // Done: the full refresh once (games may need JIT state)
            reload()
        }
    }

    private var jitPolling = false

    func reload() {
        reloadJIT()
        firmwareState = v3k_firmware_state()
        if let json = takeString(v3k_apps_json()), let data = json.data(using: .utf8),
           let decoded = try? JSONDecoder().decode([GameApp].self, from: data) {
            let sorted = decoded.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
            if sorted != apps { apps = sorted }
        }
        if let json = takeString(v3k_storage_info_json()), let data = json.data(using: .utf8),
           let decoded = try? JSONDecoder().decode(StorageInfo.self, from: data) {
            storage = decoded
        }
        if let json = takeString(v3k_settings_json()), let data = json.data(using: .utf8),
           let decoded = try? JSONDecoder().decode(EmulatorSettings.self, from: data) {
            settings = decoded
        }
    }

    /// Picks up the report of a crash in the previous session (once per launch)
    func checkForCrashReport() {
        // Wait until JIT setup is done: StikDebug pauses the app while it works, which made the
        // report hard to close
        if v3k_jit_preparing() {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.checkForCrashReport() }
            return
        }
        if let json = takeString(v3k_take_crash_report()), let data = json.data(using: .utf8),
           let decoded = try? JSONDecoder().decode(CrashReport.self, from: data) {
            crashReport = decoded
        }
    }

    /// Compatibility list (once a day) and game fixes list (every launch), in the background
    func updateOnlineLists() {
        let defaults = UserDefaults.standard
        let lastCompat = defaults.double(forKey: "v3k.compatUpdated")
        let compatDue = Date().timeIntervalSince1970 - lastCompat > 24 * 3600
        DispatchQueue.global(qos: .utility).async {
            _ = v3k_update_game_fixes()
            var compatUpdated = false
            if compatDue && v3k_update_compat_db() {
                compatUpdated = true
            }
            DispatchQueue.main.async {
                if compatUpdated { defaults.set(Date().timeIntervalSince1970, forKey: "v3k.compatUpdated") }
                self.reload()
            }
        }
    }

    func refresh() {
        v3k_refresh_apps()
        reload()
    }

    func launch(_ app: GameApp) {
        // Fast CPU: the game fixes list can turn it off (or on) for a game
        let fastCPUFix = v3k_game_fix_flag(app.titleId, "fast-cpu")
        v3k_set_fast_cpu(fastCPUFix >= 0 ? fastCPUFix == 1 : (UserDefaults.standard.object(forKey: "v3k.fastCPU") as? Bool ?? false))
        // The overlay is drawn by the app now (PerformanceHUD.swift): keep the core's own one off
        if settings.performanceOverlay {
            _ = v3k_set_setting_bool("performance-overlay", false)
            UserDefaults.standard.set(true, forKey: "v3k.hud.enabled")
            settings.performanceOverlay = false
        }
        // Per-game "Precise shader math" (read by the renderer when the game starts)
        setenv("V3K_PRECISE_SHADER_MATH", UserDefaults.standard.bool(forKey: "v3k.preciseMath.\(app.titleId)") ? "1" : "0", 1)
        // Per-game "Vertex realignment" (on unless the player turned it off for this game)
        setenv("V3K_VERTEX_REALIGN_USER_OFF", UserDefaults.standard.bool(forKey: "v3k.vertexRealignOff.\(app.titleId)") ? "1" : "0", 1)
        v3k_request_launch(app.titleId)
    }

    /// Asks the attached JIT script for executable memory, then reports whether JIT is ready.
    /// The debugger pauses the whole app while it works, so the overlay is shown first.
    func setUpJIT(completion: @escaping (Bool) -> Void) {
        settingUpJIT = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
            v3k_jit_request()
            self.waitForJIT(completion: completion)
        }
    }

    private func waitForJIT(completion: @escaping (Bool) -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            if v3k_jit_preparing() {
                self.waitForJIT(completion: completion)
                return
            }
            self.settingUpJIT = false
            self.reload()
            completion(self.jitEnabled)
        }
    }

    func deleteSaveData(_ app: GameApp) {
        lastMessage = v3k_delete_save_data(app.titleId) ? "Save data of \(app.title) deleted." : "\(app.title) has no save data to delete."
    }

    // MARK: Storage location

    /// Uses a folder on the device or an external drive for games, saves and firmware. The folder is
    /// remembered as a bookmark so it can be reopened on the next launch.
    func chooseStorage(_ url: URL) {
        _ = url.startAccessingSecurityScopedResource() // kept open: games run from there
        guard let bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            lastMessage = "Hinoki can't keep access to that folder. Try another location."
            return
        }
        let previous = UserDefaults.standard.data(forKey: "v3k.storageBookmark")
        UserDefaults.standard.set(bookmark, forKey: "v3k.storageBookmark")
        let path = url.path
        storageBusy = true
        Task.detached(priority: .userInitiated) {
            var message: UnsafeMutablePointer<CChar>?
            let ok = v3k_set_storage_folder(path, &message)
            let text = takeString(message) ?? ""
            await MainActor.run {
                if !ok { UserDefaults.standard.set(previous, forKey: "v3k.storageBookmark") }
                self.storageBusy = false
                self.lastMessage = text
                self.reload()
            }
        }
    }

    func useInternalStorage() {
        UserDefaults.standard.removeObject(forKey: "v3k.storageBookmark")
        storageBusy = true
        Task.detached(priority: .userInitiated) {
            var message: UnsafeMutablePointer<CChar>?
            _ = v3k_use_internal_storage(&message)
            let text = takeString(message) ?? ""
            await MainActor.run {
                self.storageBusy = false
                self.lastMessage = text
                self.reload()
            }
        }
    }

    func delete(_ app: GameApp) {
        if v3k_delete_app(app.titleId) {
            reload()
        }
    }

    // MARK: Importing

    func enqueue(_ urls: [URL]) {
        // Licenses (work.bin, .rif, .zrif) before the games picked with them, so those find them
        let isLicense: (URL) -> Bool = { url in
            let accessing = url.startAccessingSecurityScopedResource()
            defer { if accessing { url.stopAccessingSecurityScopedResource() } }
            return v3k_install_kind(url.path) == 5
        }
        let ordered = urls.filter(isLicense) + urls.filter { !isLicense($0) }
        for url in ordered {
            var item = PendingImport(url: url)
            if item.isPackage {
                let accessing = url.startAccessingSecurityScopedResource()
                if let stored = takeString(v3k_zrif_next_to(url.path)) {
                    item.zrif = stored
                }
                // Updates use the installed game's license; games and DLC may already have theirs
                if let json = takeString(v3k_pkg_info_json(url.path)), let data = json.data(using: .utf8),
                   let info = try? JSONDecoder().decode(PackageInfo.self, from: data) {
                    item.packageKind = info.kind
                    item.packageTitle = info.title
                    if item.zrif.isEmpty { item.zrif = info.zrif }
                }
                if accessing { url.stopAccessingSecurityScopedResource() }
            }
            queue.append(item)
        }
        processNext()
    }

    /// Called once the user entered (or skipped) the zRIF of the package being asked about
    func provideLicense(_ zrif: String) {
        guard var item = packageNeedingLicense else { return }
        packageNeedingLicense = nil
        item.zrif = zrif.trimmingCharacters(in: .whitespacesAndNewlines)
        install(item)
    }

    private func processNext() {
        guard !installing, packageNeedingLicense == nil, !queue.isEmpty else { return }
        let item = queue.removeFirst()
        // An update never needs a key of its own (it uses the game's), and themes have none
        if item.isPackage && item.zrif.isEmpty && item.packageKind != "update" && item.packageKind != "theme" {
            packageNeedingLicense = item
            return
        }
        install(item)
    }

    private func install(_ item: PendingImport) {
        installing = true
        installProgress = InstallProgress(name: item.name, percent: 0, remaining: queue.count)

        let box = ProgressBox { [weak self] percent in
            Task { @MainActor in self?.installProgress?.percent = percent }
        }
        let context = Unmanaged.passRetained(box).toOpaque()
        let url = item.url
        let zrif = item.zrif

        Task.detached(priority: .userInitiated) {
            let accessing = url.startAccessingSecurityScopedResource()
            var message: UnsafeMutablePointer<CChar>?
            let ok = v3k_install(url.path, zrif, { ctx, percent in
                guard let ctx else { return }
                Unmanaged<ProgressBox>.fromOpaque(ctx).takeUnretainedValue().handler(Double(percent))
            }, context, &message)
            if accessing { url.stopAccessingSecurityScopedResource() }
            Unmanaged<ProgressBox>.fromOpaque(context).release()
            let text = takeString(message) ?? (ok ? "Installed." : "Installation failed.")

            await MainActor.run {
                self.installing = false
                self.installProgress = nil
                self.lastMessage = "\(item.name): \(text)"
                self.reload()
                self.processNext()
            }
        }
    }

    /// Adds a license from a typed or pasted zRIF
    func installZrif(_ text: String) {
        let zrif = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !zrif.isEmpty else { return }
        Task.detached(priority: .userInitiated) {
            var message: UnsafeMutablePointer<CChar>?
            let ok = v3k_install_zrif(zrif, &message)
            let text = takeString(message) ?? (ok ? "License added." : "The license could not be added.")
            await MainActor.run {
                self.lastMessage = text
                self.reload()
            }
        }
    }

    // MARK: Settings

    func setNumber(_ key: String, _ value: Double) {
        _ = v3k_set_setting_number(key, value)
    }

    func setBool(_ key: String, _ value: Bool) {
        _ = v3k_set_setting_bool(key, value)
    }

    func setString(_ key: String, _ value: String) {
        _ = v3k_set_setting_string(key, value)
    }
}
