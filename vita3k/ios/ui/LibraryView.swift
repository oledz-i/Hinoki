// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import SwiftUI
import UniformTypeIdentifiers

/// UI colour themes (Settings > Appearance). Views read the current one; @AppStorage("v3k.theme")
/// in the top-level views makes them redraw when it changes.
enum AppTheme: String, CaseIterable, Identifiable {
    case midnight, vita, sakura, ocean, sunset, forest, crimson, graphite

    var id: String { rawValue }

    var name: String {
        switch self {
        case .midnight: "Midnight"
        case .vita: "Vita Blue"
        case .sakura: "Sakura"
        case .ocean: "Ocean"
        case .sunset: "Sunset"
        case .forest: "Forest"
        case .crimson: "Crimson"
        case .graphite: "Graphite"
        }
    }

    private static func rgb(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
    }

    // accent, second accent, background top, background middle
    private var palette: (UInt32, UInt32, UInt32, UInt32) {
        switch self {
        case .midnight: (0x4085FF, 0x8C5CFF, 0x0D0F1F, 0x171233)
        case .vita: (0x2F8CFF, 0x00C2FF, 0x06142B, 0x0B2347)
        case .sakura: (0xFF7EB6, 0xFFB3D1, 0x1F0D17, 0x33142A)
        case .ocean: (0x1FD1C4, 0x3A8DFF, 0x041A1F, 0x082C33)
        case .sunset: (0xFF8A3D, 0xFF4F81, 0x1F0E0A, 0x331420)
        case .forest: (0x4CD97B, 0xB4E05A, 0x07170D, 0x0F2617)
        case .crimson: (0xFF4D5E, 0xFF9150, 0x1A0607, 0x2E0B10)
        case .graphite: (0xC8CDD6, 0x8A93A3, 0x111214, 0x1D1F23)
        }
    }

    var accent: Color { Self.rgb(palette.0) }
    var backgroundColors: [Color] { [Self.rgb(palette.2), Self.rgb(palette.3), .black] }
    var accent2: Color { Self.rgb(palette.1) }
    var background: LinearGradient {
        LinearGradient(colors: [Self.rgb(palette.2), Self.rgb(palette.3), .black], startPoint: .topLeading, endPoint: .bottomTrailing)
    }
    var accentGradient: LinearGradient {
        LinearGradient(colors: [accent, accent2], startPoint: .leading, endPoint: .trailing)
    }

    static var current: AppTheme {
        AppTheme(rawValue: UserDefaults.standard.string(forKey: "v3k.theme") ?? "") ?? .midnight
    }
}

enum Theme {
    static var accent: Color { AppTheme.current.accent }
    static var accent2: Color { AppTheme.current.accent2 }
    static var background: LinearGradient { AppTheme.current.background }
    static let card = Color.white.opacity(0.06)
    static var accentGradient: LinearGradient { AppTheme.current.accentGradient }
    static var accentUIColor: UIColor { UIColor(AppTheme.current.accent) }
}

struct LibraryView: View {
    @EnvironmentObject var model: LibraryModel
    @ObservedObject private var music = MenuMusic.shared
    @AppStorage("v3k.musicEnabled") private var musicEnabled = true
    @AppStorage("v3k.theme") private var themeName = AppTheme.midnight.rawValue
    @AppStorage("v3k.librarySort") private var sortMode = "title"
    @AppStorage("v3k.vitaHome") private var vitaHome = false

    @State private var search = ""
    @State private var showingImporter = false
    @State private var importerTypes: [UTType] = [.item]
    @State private var showingSettings = false
    @State private var pendingLaunch: GameApp?
    @State private var pendingDelete: GameApp?
    @State private var pendingDeleteSave: GameApp?
    @State private var gameDetails: GameApp?
    @State private var zrifInput = ""
    @State private var showingZrifEntry = false
    @State private var zrifEntry = ""

    private var filteredApps: [GameApp] {
        let apps = search.isEmpty ? model.apps : model.apps.filter {
            $0.title.localizedCaseInsensitiveContains(search) || $0.titleId.localizedCaseInsensitiveContains(search)
        }
        if sortMode == "recent" {
            return apps.sorted { ($0.lastPlayed, $1.title) > ($1.lastPlayed, $0.title) }
        }
        return apps
    }

    var body: some View {
        NavigationStack {
            ZStack {
                AnimatedBackground().ignoresSafeArea()

                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        StatusBar(jitEnabled: model.jitEnabled, jitPreparing: model.jitPreparing, jitCanRequest: model.jitCanRequest, jitProgress: model.jitProgress, jitTarget: model.jitTarget, hasFirmware: model.hasFirmware, hasFonts: model.hasFonts)

                        if !model.hasFirmware {
                            FirmwareCallout { openImporter([.item]) }
                        } else if !model.hasFonts {
                            FontsCallout { openImporter([.item]) }
                        }

                        PowerNotice()

                        if musicEnabled && !music.nowPlaying.isEmpty {
                            NowPlayingChip(title: music.nowPlaying, style: music.styleName) { music.skip() }
                        }

                        if search.isEmpty, let last = model.apps.filter({ $0.lastPlayed > 0 }).max(by: { $0.lastPlayed < $1.lastPlayed }) {
                            ContinuePlayingCard(app: last) { requestLaunch(last) }
                        }

                        if !model.apps.isEmpty {
                            Text(model.apps.count == 1 ? "1 game" : "\(model.apps.count) games")
                                .font(.footnote.weight(.medium))
                                .foregroundStyle(.white.opacity(0.5))
                        }

                        if model.apps.isEmpty {
                            EmptyLibrary(importFiles: { openImporter([.item]) }, importFolder: { openImporter([.folder]) })
                        } else {
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 18)], spacing: 22) {
                                ForEach(filteredApps) { app in
                                    GameCard(app: app)
                                        .onTapGesture { requestLaunch(app) }
                                        .contextMenu {
                                            Button { requestLaunch(app) } label: { Label("Play", systemImage: "play.fill") }
                                            Button { gameDetails = app } label: {
                                                Label("Game settings, patches, cheats & mods", systemImage: "slider.horizontal.3")
                                            }
                                            Button(role: .destructive) { pendingDeleteSave = app } label: {
                                                Label("Delete save data", systemImage: "externaldrive.badge.minus")
                                            }
                                            Button(role: .destructive) { pendingDelete = app } label: {
                                                Label("Delete game", systemImage: "trash")
                                            }
                                        }
                                }
                            }
                        }
                    }
                    .padding(.horizontal, 20)
                    .padding(.bottom, 40)
                }
                .refreshable { await MainActor.run { model.refresh() } }

                if vitaHome {
                    VitaHomeView(apps: model.apps, onLaunch: { requestLaunch($0) }, onDetails: { gameDetails = $0 },
                                 onDelete: { pendingDelete = $0 }, onDeleteSave: { pendingDeleteSave = $0 },
                                 onImportFiles: { openImporter([.item]) }, onImportFolder: { openImporter([.folder]) },
                                 onEnterZrif: { showingZrifEntry = true })
                }

                if let progress = model.installProgress {
                    InstallOverlay(progress: progress)
                }

                if model.settingUpJIT {
                    JITSetupOverlay(progress: model.jitProgress, target: model.jitTarget)
                }
            }
            .navigationTitle("Hinoki")
            .toolbar(vitaHome ? .hidden : .automatic, for: .navigationBar)
            // The Vita home screen has its own Search bubble: the library's search bar would cover it
            .modifier(LibrarySearch(enabled: !vitaHome, text: $search))
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu {
                        Picker("Sort", selection: $sortMode) {
                            Label("Title", systemImage: "textformat").tag("title")
                            Label("Recently played", systemImage: "clock").tag("recent")
                        }
                    } label: {
                        Image(systemName: "arrow.up.arrow.down.circle")
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    ImportMenu(importFiles: { openImporter([.item]) }, importFolder: { openImporter([.folder]) },
                               enterZrif: { showingZrifEntry = true })
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showingSettings = true } label: {
                        Image(systemName: "gearshape.fill").font(.title3)
                    }
                }
            }
            .fileImporter(isPresented: $showingImporter, allowedContentTypes: importerTypes, allowsMultipleSelection: true) { result in
                if case .success(let urls) = result {
                    model.enqueue(urls)
                }
            }
            .sheet(isPresented: $showingSettings) {
                SettingsView().environmentObject(model)
            }
            .sheet(item: $gameDetails) { app in
                GameSettingsView(app: app).environmentObject(model)
            }
            .sheet(item: $model.crashReport) { report in
                CrashReportView(report: report)
            }
            .onChange(of: model.launchRequested) { _, app in
                guard let app else { return }
                model.launchRequested = nil
                // Let the sheet finish closing first
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { requestLaunch(app) }
            }
            .onAppear {
                model.checkForCrashReport()
                LibraryHost.updateOrientation()
            }
            .onChange(of: vitaHome) { _, _ in LibraryHost.updateOrientation() }
            .alert(model.jitPreparing ? "JIT is almost ready" : "JIT is not ready", isPresented: Binding(get: { pendingLaunch != nil }, set: { if !$0 { pendingLaunch = nil } })) {
                if !model.jitPreparing {
                    EnableJITButtons()
                }
                Button(model.jitPreparing ? "OK" : "Not now", role: .cancel) { pendingLaunch = nil }
            } message: {
                Text(model.jitPreparing ? "Hinoki is still preparing JIT (\(model.jitProgress)/\(model.jitTarget)). Try again in a moment." : "\(model.jitStatus)\n\nTap Enable JIT: Hinoki opens your JIT app and gets ready by itself once JIT attaches.")
            }
            .modifier(DeleteAlerts(model: model, pendingDelete: $pendingDelete, pendingDeleteSave: $pendingDeleteSave))
            .modifier(ImportAlerts(model: model, zrifInput: $zrifInput, showingZrifEntry: $showingZrifEntry, zrifEntry: $zrifEntry, licenseMessage: licenseMessage))
        }
        .tint(Theme.accent)
        .id(themeName) // redraw everything in the new colours when the theme changes
    }

    private var licenseMessage: String {
        guard let item = model.packageNeedingLicense else { return "" }
        let what = item.packageTitle.isEmpty ? item.name : item.packageTitle
        let kind = item.packageKind == "dlc" ? "This DLC" : "This game"
        return "\(what)\n\(kind) needs its zRIF license key (updates don't need one). You can also put it in a .zrif file next to the .pkg, or add its work.bin first (+ › License file)."
    }

    private func openImporter(_ types: [UTType]) {
        importerTypes = types
        showingImporter = true
    }

    private func requestLaunch(_ app: GameApp) {
        model.reload()
        if model.jitEnabled {
            model.launch(app)
        } else if model.jitCanRequest {
            model.setUpJIT { ready in
                if ready {
                    model.launch(app)
                } else {
                    pendingLaunch = app
                }
            }
        } else {
            pendingLaunch = app
        }
    }
}

// MARK: - Pieces

struct GameCard: View {
    let app: GameApp

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ZStack {
                RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Theme.card)
                if let icon = app.icon {
                    Image(uiImage: icon)
                        .resizable()
                        .scaledToFill()
                } else {
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 44))
                        .foregroundStyle(Theme.accentGradient)
                }
            }
            .aspectRatio(1, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).stroke(.white.opacity(0.08), lineWidth: 1))
            .shadow(color: Theme.accent.opacity(0.25), radius: 14, y: 6)

            Text(app.title)
                .font(.subheadline.weight(.semibold))
                .lineLimit(2)
                .foregroundStyle(.white)
            HStack(spacing: 6) {
                Text(app.titleId).font(.caption2.monospaced())
                if !app.version.isEmpty {
                    Text("v\(app.version)").font(.caption2)
                }
            }
            .foregroundStyle(.white.opacity(0.5))
            if let compatibility = app.compatibility {
                CompatibilityBadge(compatibility: compatibility)
            }
        }
        .contentShape(Rectangle())
    }
}

struct StatusBar: View {
    let jitEnabled: Bool
    let jitPreparing: Bool
    let jitCanRequest: Bool
    let jitProgress: Int
    let jitTarget: Int
    let hasFirmware: Bool
    let hasFonts: Bool

    var body: some View {
        HStack(spacing: 10) {
            if jitPreparing {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.mini).tint(Theme.accent)
                    Text(jitTarget > 0 ? "JIT \(jitProgress)/\(jitTarget)" : "Preparing JIT")
                        .monospacedDigit()
                }
                    .font(.caption.weight(.semibold))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(Capsule().fill(Theme.accent.opacity(0.2)))
                    .foregroundStyle(Theme.accent)
            } else if jitCanRequest {
                Label("JIT attached", systemImage: "bolt.badge.clock")
                    .font(.caption.weight(.semibold))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(Capsule().fill(Theme.accent.opacity(0.2)))
                    .foregroundStyle(Theme.accent)
            } else if jitEnabled {
                Pill(text: "JIT ready", systemImage: "bolt.fill", good: true)
            } else {
                // Tap to enable JIT
                Menu {
                    EnableJITButtons()
                } label: {
                    Pill(text: "JIT off · Enable", systemImage: "bolt.fill", good: false)
                }
            }
            Pill(text: hasFirmware ? "Firmware" : "No firmware", systemImage: "cpu", good: hasFirmware)
            Pill(text: hasFonts ? "Fonts" : "No fonts", systemImage: "textformat", good: hasFonts)
            Spacer()
        }
        .padding(.top, 4)
    }
}

/// Low Power Mode and a hot iPhone both slow games down a lot: say so before the player starts one
struct PowerNotice: View {
    @State private var lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
    @State private var thermal = ProcessInfo.processInfo.thermalState

    private var message: String? {
        if lowPower { return "Low Power Mode is on: games run slower. Turn it off in Control Center for the best performance." }
        if thermal == .serious || thermal == .critical { return "Your iPhone is hot, so iOS is slowing it down. Games run smoother once it cools off." }
        return nil
    }

    var body: some View {
        Group {
            if let message {
                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: lowPower ? "battery.25" : "thermometer.sun.fill")
                        .foregroundStyle(.orange)
                    Text(message).font(.caption).foregroundStyle(.white.opacity(0.85))
                    Spacer(minLength: 0)
                }
                .padding(12)
                .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.orange.opacity(0.14)))
                .frame(maxWidth: 520, alignment: .leading)
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .NSProcessInfoPowerStateDidChange).receive(on: RunLoop.main)) { _ in
            lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        }
        .onReceive(NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification).receive(on: RunLoop.main)) { _ in
            thermal = ProcessInfo.processInfo.thermalState
        }
    }
}

struct NowPlayingChip: View {
    let title: String
    let style: String
    let skip: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "music.note")
                .foregroundStyle(Theme.accentGradient)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.caption.weight(.semibold)).foregroundStyle(.white)
                Text(style).font(.caption2).foregroundStyle(.white.opacity(0.5))
            }
            Spacer(minLength: 0)
            Button(action: skip) {
                Image(systemName: "forward.fill").font(.caption)
            }
            .buttonStyle(.bordered)
            .buttonBorderShape(.capsule)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.card))
        .frame(maxWidth: 360, alignment: .leading)
        .animation(.easeInOut, value: title)
    }
}

struct Pill: View {
    let text: String
    let systemImage: String
    let good: Bool

    var body: some View {
        Label(text, systemImage: systemImage)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Capsule().fill((good ? Color.green : Color.orange).opacity(0.18)))
            .foregroundStyle(good ? Color.green : Color.orange)
    }
}

/// The game's state on the Vita3K compatibility list
struct CompatibilityBadge: View {
    let compatibility: Compatibility

    var body: some View {
        HStack(spacing: 4) {
            Circle().fill(compatibility.color).frame(width: 6, height: 6)
            Text(compatibility.label).font(.caption2.weight(.semibold))
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(Capsule().fill(compatibility.color.opacity(0.16)))
        .foregroundStyle(compatibility.color)
    }
}

/// Firmware is installed but the font package isn't: some games show no text without it
struct FontsCallout: View {
    let action: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "textformat").font(.title3).foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 4) {
                Text("Font package missing").font(.subheadline.weight(.semibold))
                Text("Some games show blank text without it. Import PSP2UPDAT.PUP (the font package from PlayStation's firmware page).")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.7))
                Button("Import font package", action: action)
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .padding(.top, 2)
            }
            Spacer(minLength: 0)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Theme.card))
        .frame(maxWidth: 520, alignment: .leading)
    }
}

struct FirmwareCallout: View {
    let action: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: "exclamationmark.triangle.fill").font(.title2).foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 6) {
                Text("Firmware required").font(.headline)
                Text("Most games need the PS Vita firmware (PSVUPDAT.PUP) and the font package (PSP2UPDAT.PUP). Download them from PlayStation's site, then import them here.")
                    .font(.footnote)
                    .foregroundStyle(.white.opacity(0.7))
                Button("Import firmware", action: action)
                    .buttonStyle(.borderedProminent)
                    .padding(.top, 4)
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(Theme.card))
    }
}

struct EmptyLibrary: View {
    let importFiles: () -> Void
    let importFolder: () -> Void

    var body: some View {
        VStack(spacing: 18) {
            Image(systemName: "square.stack.3d.up.fill")
                .font(.system(size: 64))
                .foregroundStyle(Theme.accentGradient)
                .padding(.top, 40)
            Text("Your library is empty").font(.title2.bold())
            Text("Add games as .vpk, .zip, .pkg (with its zRIF or work.bin) or a dumped game folder.\nYou can also drop files into the Import folder of Hinoki in the Files app.")
                .font(.callout)
                .multilineTextAlignment(.center)
                .foregroundStyle(.white.opacity(0.65))
            HStack(spacing: 12) {
                Button(action: importFiles) { Label("Import files", systemImage: "doc.badge.plus") }
                    .buttonStyle(.borderedProminent)
                Button(action: importFolder) { Label("Import folder", systemImage: "folder.badge.plus") }
                    .buttonStyle(.bordered)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 30)
    }
}

struct JITSetupOverlay: View {
    var progress = 0
    var target = 0

    var body: some View {
        ZStack {
            Color.black.opacity(0.7).ignoresSafeArea()
            VStack(spacing: 14) {
                ProgressView().controlSize(.large).tint(Theme.accent)
                Text(target > 0 ? "Setting up JIT \(progress)/\(target)" : "Setting up JIT").font(.headline).monospacedDigit()
                Text("StikDebug is preparing executable memory for the emulator. Keep Hinoki open, this can take a little while.")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white.opacity(0.7))
            }
            .padding(24)
            .frame(maxWidth: 340)
            .background(RoundedRectangle(cornerRadius: 24, style: .continuous).fill(.ultraThinMaterial))
        }
    }
}

struct InstallOverlay: View {
    let progress: InstallProgress

    var body: some View {
        ZStack {
            Color.black.opacity(0.55).ignoresSafeArea()
            VStack(spacing: 14) {
                ProgressView(value: min(max(progress.percent, 0), 100), total: 100)
                    .progressViewStyle(.linear)
                    .tint(Theme.accent)
                Text("Installing \(progress.name)")
                    .font(.headline)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                Text(progress.remaining > 0 ? "\(Int(progress.percent))% · \(progress.remaining) more queued" : "\(Int(progress.percent))%")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.6))
            }
            .padding(24)
            .frame(maxWidth: 340)
            .background(RoundedRectangle(cornerRadius: 24, style: .continuous).fill(.ultraThinMaterial))
        }
    }
}

// MARK: - Crash report

struct CrashReportView: View {
    let report: CrashReport
    @Environment(\.dismiss) private var dismiss

    private var reportFile: URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("Hinoki crash report.txt")
        try? report.report.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Label("The last session didn't end normally", systemImage: "exclamationmark.triangle.fill")
                        .font(.headline)
                        .foregroundStyle(.orange)
                    Text("Share the report and the log when you report the problem, so it can be fixed.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Text(report.report)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .padding(12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(.quaternary, in: RoundedRectangle(cornerRadius: 12))
                    ShareLink(items: [reportFile, URL(fileURLWithPath: report.log)]) {
                        Label("Share report and log", systemImage: "square.and.arrow.up")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    Button {
                        dismiss()
                    } label: {
                        Text("Close").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }
                .padding()
            }
            .navigationTitle("Crash report")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Close") { dismiss() } }
            }
        }
    }
}

/// The + menu of the library
private struct ImportMenu: View {
    let importFiles: () -> Void
    let importFolder: () -> Void
    let enterZrif: () -> Void

    var body: some View {
        Menu {
            Button(action: importFiles) {
                Label("Files (VPK, ZIP, PKG, PUP)", systemImage: "doc.badge.plus")
            }
            Button(action: importFolder) {
                Label("Game folder", systemImage: "folder.badge.plus")
            }
            Section("License") {
                Button(action: importFiles) {
                    Label("License file (work.bin, .rif)", systemImage: "key.fill")
                }
                Button(action: enterZrif) {
                    Label("zRIF key", systemImage: "key.horizontal.fill")
                }
            }
        } label: {
            Image(systemName: "plus.circle.fill").font(.title2).foregroundStyle(Theme.accentGradient)
        }
    }
}

/// The alerts of importing: a .pkg asking for its license, typing a zRIF, and the result message
/// (kept out of LibraryView.body, which grew too long for the Swift type checker)
private struct ImportAlerts: ViewModifier {
    @ObservedObject var model: LibraryModel
    @Binding var zrifInput: String
    @Binding var showingZrifEntry: Bool
    @Binding var zrifEntry: String
    let licenseMessage: String

    func body(content: Content) -> some View {
        content
            .alert("License needed", isPresented: Binding(get: { model.packageNeedingLicense != nil }, set: { _ in })) {
                TextField("zRIF", text: $zrifInput)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Install") {
                    model.provideLicense(zrifInput)
                    zrifInput = ""
                }
                Button("Install without", role: .cancel) {
                    model.provideLicense("")
                    zrifInput = ""
                }
            } message: {
                Text(licenseMessage)
            }
            .alert("Add a license", isPresented: $showingZrifEntry) {
                TextField("zRIF (KO5i...)", text: $zrifEntry)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Add") {
                    model.installZrif(zrifEntry)
                    zrifEntry = ""
                }
                Button("Cancel", role: .cancel) { zrifEntry = "" }
            } message: {
                Text("Paste the game's or DLC's zRIF. Then import its .pkg, or a dump that has no work.bin.")
            }
            .alert(model.lastMessage ?? "", isPresented: Binding(get: { model.lastMessage != nil && model.packageNeedingLicense == nil }, set: { if !$0 { model.lastMessage = nil } })) {
                Button("OK", role: .cancel) { model.lastMessage = nil }
            }
    }
}

/// Deleting a game or its save data (kept out of LibraryView.body for the Swift type checker)
private struct DeleteAlerts: ViewModifier {
    @ObservedObject var model: LibraryModel
    @Binding var pendingDelete: GameApp?
    @Binding var pendingDeleteSave: GameApp?

    func body(content: Content) -> some View {
        content
            .alert("Delete \(pendingDelete?.title ?? "")?", isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } })) {
                Button("Delete", role: .destructive) {
                    if let app = pendingDelete { model.delete(app) }
                    pendingDelete = nil
                }
                Button("Cancel", role: .cancel) { pendingDelete = nil }
            } message: {
                Text("The game is removed from Hinoki. Save data is kept.")
            }
            .alert("Delete the save data of \(pendingDeleteSave?.title ?? "")?", isPresented: Binding(get: { pendingDeleteSave != nil }, set: { if !$0 { pendingDeleteSave = nil } })) {
                Button("Delete", role: .destructive) {
                    if let app = pendingDeleteSave { model.deleteSaveData(app) }
                    pendingDeleteSave = nil
                }
                Button("Cancel", role: .cancel) { pendingDeleteSave = nil }
            } message: {
                Text("This can't be undone.")
            }
    }
}

/// The library's search bar, left out while the PS Vita home screen is showing
struct LibrarySearch: ViewModifier {
    let enabled: Bool
    @Binding var text: String

    func body(content: Content) -> some View {
        if enabled {
            content.searchable(text: $text, prompt: "Search games")
        } else {
            content
        }
    }
}

/// The game played most recently, one tap away
struct ContinuePlayingCard: View {
    let app: GameApp
    let play: () -> Void

    private var playedText: String {
        let hours = app.playTime / 3600, minutes = (app.playTime % 3600) / 60
        let time = hours > 0 ? "\(hours) h \(minutes) min played" : minutes > 0 ? "\(minutes) min played" : ""
        let last = Date(timeIntervalSince1970: TimeInterval(app.lastPlayed))
        let when = RelativeDateTimeFormatter().localizedString(for: last, relativeTo: Date())
        return time.isEmpty ? "Last played \(when)" : "Last played \(when) · \(time)"
    }

    var body: some View {
        Button(action: play) {
            HStack(spacing: 14) {
                Group {
                    if let icon = app.icon {
                        Image(uiImage: icon).resizable().scaledToFill()
                    } else {
                        Image(systemName: "gamecontroller.fill").font(.title2).foregroundStyle(.white.opacity(0.6))
                    }
                }
                .frame(width: 56, height: 56)
                .background(Color.white.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                VStack(alignment: .leading, spacing: 3) {
                    Text("Continue playing").font(.caption.weight(.semibold)).foregroundStyle(.white.opacity(0.55))
                    Text(app.title).font(.headline).foregroundStyle(.white).lineLimit(1)
                    Text(playedText).font(.caption).foregroundStyle(.white.opacity(0.5)).lineLimit(1)
                }
                Spacer(minLength: 8)
                Image(systemName: "play.circle.fill").font(.system(size: 34)).foregroundStyle(.white)
            }
            .padding(12)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color.white.opacity(0.09)))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Color.white.opacity(0.08)))
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Continue playing \(app.title)")
    }
}
