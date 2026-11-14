// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @EnvironmentObject var model: LibraryModel
    @Environment(\.dismiss) private var dismiss
    @AppStorage("v3k.theme") private var themeName = AppTheme.midnight.rawValue
    /// Set when the settings are shown inside the PS Vita home screen instead of as a sheet
    var onDone: (() -> Void)?

    var body: some View {
        NavigationStack {
            Form {
                AppearanceSection()
                MusicSection()
                StorageSection()
                TouchControlsSection()
                EmulationSections()
                MultiplayerSection()
                AdhocSection()
                Section {
                    NavigationLink {
                        AllPatchesView()
                    } label: {
                        Label("Patches", systemImage: "bandage")
                    }
                } footer: {
                    Text("Cheats and mods are set per game: long-press a game › Game settings, patches & saves.")
                }
                MemorySection()
                JITSection()
                CreditsSection()
                AboutSection()
            }
            .alert(model.lastMessage ?? "", isPresented: Binding(get: { model.lastMessage != nil }, set: { if !$0 { model.lastMessage = nil } })) {
                Button("OK", role: .cancel) { model.lastMessage = nil }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if onDone == nil {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                    }
                }
            }
            // Inside the PS Vita home screen the panel has its own title and close button
            .toolbar(onDone == nil ? .automatic : .hidden, for: .navigationBar)
        }
        .tint(AppTheme(rawValue: themeName)?.accent ?? Theme.accent)
    }
}

// MARK: - Appearance

struct AppearanceSection: View {
    @AppStorage("v3k.theme") private var themeName = AppTheme.midnight.rawValue
    @AppStorage("v3k.animatedBackground") private var animatedBackground = true
    @AppStorage("v3k.vitaHome") private var vitaHome = false
    @AppStorage("v3k.vitaHome.lockScreen") private var vitaLockScreen = true
    @AppStorage("v3k.vitaHome.theme") private var vitaTheme = VitaTheme.defaultId

    var body: some View {
        Section("Appearance") {
            Toggle(isOn: $vitaHome) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("PS Vita home screen (beta)")
                    Text("The Vita's home screen in landscape: bubbles on pages you swipe sideways, the start screen and LiveArea you peel away from the top-right corner, each game's own LiveArea, gate and manual, and the Vita's font and themes from your firmware.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if vitaHome {
                Toggle("Start screen", isOn: $vitaLockScreen)
                Picker("Vita theme", selection: $vitaTheme) {
                    ForEach(VitaTheme.installed()) { theme in
                        Text(theme.name).tag(theme.id)
                    }
                }
                .onChange(of: vitaTheme) { _, _ in MenuMusic.shared.settingsChanged() }
                Text("Themes installed in Hinoki (ux0/theme) show up here, with their wallpapers.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 92), spacing: 12)], spacing: 12) {
                ForEach(AppTheme.allCases) { theme in
                    ThemeSwatch(theme: theme, selected: themeName == theme.rawValue) {
                        themeName = theme.rawValue
                    }
                }
            }
            .padding(.vertical, 4)
            Toggle(isOn: $animatedBackground) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Animated background")
                    Text("Moving light and particles for each theme, with a little tilt. Off when Reduce Motion is on.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
}

struct ThemeSwatch: View {
    let theme: AppTheme
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(theme.background)
                    .overlay(Circle().fill(theme.accentGradient).frame(width: 22, height: 22))
                    .overlay(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(selected ? theme.accent : Color.white.opacity(0.15), lineWidth: selected ? 2.5 : 1)
                    )
                    .frame(height: 54)
                Text(theme.name).font(.caption2).foregroundStyle(.primary)
            }
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Music

struct MusicSection: View {
    @AppStorage("v3k.musicEnabled") private var musicEnabled = true
    @AppStorage("v3k.musicVolume") private var musicVolume = 0.6
    @AppStorage("v3k.musicStyle") private var musicStyle = -1
    @AppStorage("v3k.vitaMusic") private var vitaMusic = false

    var body: some View {
        Section {
            Toggle("Menu music", isOn: $musicEnabled)
            if musicEnabled {
                Toggle(isOn: $vitaMusic) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Use the PS Vita home music")
                        Text("Plays the home music from your firmware (or from the selected Vita theme) instead of the songs made on your device. Needs the firmware installed; the other songs come back when it isn't found.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Picker("Style", selection: $musicStyle) {
                    Text("Mix of everything").tag(-1)
                    ForEach(musicStyles.indices, id: \.self) { index in
                        Text(musicStyles[index].name).tag(index)
                    }
                }
                VStack(alignment: .leading) {
                    Text("Volume: \(Int(musicVolume * 100))%")
                    Slider(value: $musicVolume, in: 0...1, step: 0.05)
                }
                Button {
                    MenuMusic.shared.skip()
                } label: {
                    Label("Next song", systemImage: "forward.fill")
                }
            }
        } header: {
            Text("Music")
        } footer: {
            Text("Original songs composed on your device, a new one every time. Music stops when a game starts and stays quiet while another app is playing audio.")
        }
        .onChange(of: musicEnabled) { _, _ in MenuMusic.shared.settingsChanged() }
        .onChange(of: musicVolume) { _, _ in MenuMusic.shared.settingsChanged() }
        .onChange(of: vitaMusic) { _, _ in MenuMusic.shared.settingsChanged() }
        .onChange(of: musicStyle) { _, _ in
            MenuMusic.shared.settingsChanged()
            MenuMusic.shared.skip()
        }
    }
}

// MARK: - Storage

struct StorageSection: View {
    @EnvironmentObject var model: LibraryModel
    @State private var picking = false

    private var locationName: String {
        guard model.storage.custom else { return "This device" }
        let url = URL(fileURLWithPath: model.storage.path)
        return url.deletingLastPathComponent().lastPathComponent + "/" + url.lastPathComponent
    }

    var body: some View {
        Section {
            LabeledContent("Location", value: locationName)
            if model.storage.totalBytes > 0 {
                LabeledContent("Free", value: model.storage.freeText)
                LabeledContent("Used", value: model.storage.usedText)
            }
            if !model.storage.note.isEmpty {
                Label(model.storage.note, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }
            Button {
                picking = true
            } label: {
                Label("Choose folder or drive…", systemImage: "externaldrive.badge.plus")
            }
            .disabled(model.storageBusy)
            if model.storage.custom {
                Button {
                    model.useInternalStorage()
                } label: {
                    Label("Use this device's storage", systemImage: "iphone")
                }
                .disabled(model.storageBusy)
            }
            if model.storageBusy {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("Moving storage…").foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Storage")
        } footer: {
            Text("Games, saves and firmware live here. Pick a folder on this device or on an external drive (USB-C SSD, thumb drive, microSD reader). Firmware is copied over automatically. You can also copy an existing Vita3K folder (with ux0 inside) onto the drive and pick it. Keep the drive connected while playing.")
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result {
                model.chooseStorage(url)
            }
        }
    }
}

// MARK: - Touch controls

struct TouchControlsSection: View {
    @AppStorage("v3k.touchOpacity") private var touchOpacity = 0.6
    @AppStorage("v3k.touchScale") private var touchScale = 1.0
    @AppStorage("v3k.touchHideWithController") private var hideWithController = true
    @AppStorage("v3k.touchHaptics") private var touchHaptics = true
    @AppStorage("v3k.touchRearButton") private var rearButton = true
    @AppStorage("v3k.portraitMode") private var portraitMode = true
    @AppStorage("v3k.tvMode") private var tvMode = true
    @AppStorage("v3k.controllerTheme") private var controllerTheme = ControllerTheme.classic.rawValue
    @EnvironmentObject var model: LibraryModel
    @State private var layoutCustomized = TouchLayout.isCustomized

    var body: some View {
        Section {
            Picker("Theme", selection: $controllerTheme) {
                ForEach(ControllerTheme.allCases) { theme in
                    Text(theme.name).tag(theme.rawValue)
                }
            }
            VStack(alignment: .leading) {
                Text("Opacity: \(Int(touchOpacity * 100))%")
                Slider(value: $touchOpacity, in: 0.2...1, step: 0.05)
            }
            VStack(alignment: .leading) {
                Text("Size: \(Int(touchScale * 100))%")
                Slider(value: $touchScale, in: 0.7...1.4, step: 0.05)
            }
            Toggle("Hide when a controller is connected", isOn: $hideWithController)
            Toggle("Vibrate on press", isOn: $touchHaptics)
            Toggle(isOn: $tvMode) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("TV mode")
                    Text("With a TV connected (Screen Mirroring / AirPlay or a cable), a game you start plays on the TV only and the phone becomes the controller. Off: the TV mirrors the phone. Takes effect when a game starts; after turning it on, reconnect the TV if it is already mirroring.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $portraitMode) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Portrait mode")
                    Text("Hold the phone upright to play with the game at the top and the controls below it. Landscape stays as it is; each has its own control layout.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $rearButton) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Rear touchpad button")
                    Text("A button at the top of the screen that sends your touches to the Vita's rear touchpad, for games that use it. Also in the game menu.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: Binding(get: { model.settings.motion }, set: { value in
                model.settings.motion = value
                model.setBool("motion", value)
            })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Motion controls")
                    Text("Tilting the iPhone (or a controller with motion sensors) moves the Vita's gyroscope and accelerometer.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if layoutCustomized {
                Button("Reset control layout") {
                    TouchLayout.resetAll()
                    layoutCustomized = false
                }
            }
            NavigationLink {
                ControllerMappingView()
            } label: {
                Label("Button mapping", systemImage: "gamecontroller")
            }
        } header: {
            Text("Controls")
        } footer: {
            Text("Touch control changes apply the next time a game starts. In game, the ✕ at the top opens the game menu: screenshot, rear touchpad, edit control layout (drag and pinch to move and resize buttons) and quit. The eye button hides or shows the controls. With a controller, hold Select + Start, or press Home/PS, to open the menu.")
        }
    }
}

// MARK: - Multiplayer

/// Local (ad hoc) play between phones. Games find each other by broadcasting on the network, which
/// iOS blocks for sideloaded apps, so each phone also sends straight to the addresses typed here.
struct MultiplayerSection: View {
    @AppStorage("v3k.adhocPeers") private var peers = ""
    @State private var ownAddress = MultiplayerSection.wifiAddress()

    var body: some View {
        Section {
            LabeledContent("This phone", value: ownAddress ?? "Not on Wi-Fi")
            TextField("Other player's IP, e.g. 192.168.1.23", text: $peers)
                .keyboardType(.numbersAndPunctuation)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
        } header: {
            Text("Multiplayer (experimental)")
        } footer: {
            Text("Local ad hoc play with other Hinoki players: both phones on the same Wi-Fi, or one joined to the other's Personal Hotspot. Type the other phone's address (shown above on that phone) on at least one of them; several addresses can be separated by commas. Then start the game's ad hoc / local mode on both, and allow Local Network access when iOS asks. Games that use Sony's ad hoc matching library don't connect yet.")
        }
        .onAppear { ownAddress = MultiplayerSection.wifiAddress() }
    }

    /// The phone's Wi-Fi address (or its Personal Hotspot address when it shares one)
    static func wifiAddress() -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        var found: [String: String] = [:]
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let sa = entry.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: entry.pointee.ifa_name)
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                found[name] = String(cString: host)
            }
        }
        return found["en0"] ?? found.first(where: { $0.key.hasPrefix("bridge") })?.value
    }
}

// MARK: - Emulation

struct EmulationSections: View {
    @EnvironmentObject var model: LibraryModel
    @AppStorage("v3k.fastCPU") private var fastCPU = false
    @AppStorage("v3k.keepCool") private var keepCool = true
    @State private var confirmClearShaders = false

    private let resolutions: [Double] = [0.5, 0.75, 1, 1.5, 2, 3, 4]
    private let anisotropy = [1, 2, 4, 8, 16]
    private let filters = ["Nearest", "Bilinear", "Bicubic", "FXAA", "FSR"]

    var body: some View {
        Section {
            Picker("Internal resolution", selection: binding(\.resolutionMultiplier) { model.setNumber("resolution-multiplier", $0) }) {
                ForEach(resolutions, id: \.self) { value in
                    Text(label(for: value)).tag(value)
                }
            }
            Toggle(isOn: binding(\.fpsHack) { model.setBool("fps-hack", $0) }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("60 FPS mode")
                    Text("Runs 30 FPS games at 60 when possible. Some games then run 2× too fast: turn it off for those (per game). A game's own 60 FPS patch is safer when there is one.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Resolution & frame rate")
        } footer: {
            Text("Higher than 1× is sharper but harder on the GPU (more heat, lower FPS). 1.5× or 2× suits most iPhones. Applies from the next game launch.")
        }

        Section("Graphics") {
            Picker(selection: binding(\.screenFilter) { model.setString("screen-filter", $0) }) {
                ForEach(filters, id: \.self) { Text($0).tag($0) }
            } label: {
                described("Screen filter", "How the picture is scaled to your screen. Nearest: sharp pixels. Bilinear: smooth. Bicubic: smoother. FXAA: smooths jagged edges. FSR: AMD upscaling, sharper than Bilinear at a small GPU cost.")
            }
            Picker(selection: binding(\.anisotropicFiltering) { model.setNumber("anisotropic-filtering", Double($0)) }) {
                ForEach(anisotropy, id: \.self) { Text("\($0)×").tag($0) }
            } label: {
                described("Anisotropic filtering", "Sharper floors and walls seen at an angle. Higher costs a little GPU.")
            }
            Toggle(isOn: binding(\.vSync) { model.setBool("v-sync", $0) }) {
                described("V-Sync", "On: every frame waits for the screen (smoothest, no skipped frames). Off: the game never waits for the screen; a frame the screen can't take yet is skipped. Turn off for games that slow down or stutter with it on. Applies from the next game launch.")
            }
            Toggle(isOn: binding(\.textureCache) { model.setBool("texture-cache", $0) }) {
                described("Texture cache", "Reuses textures instead of reloading them. Faster; turn off only if textures look wrong.")
            }
            Toggle(isOn: binding(\.highAccuracy) { model.setBool("high-accuracy", $0) }) {
                described("High accuracy", "Draws blending and effects more exactly. Fixes some graphics, costs speed.")
            }
            Picker(selection: binding(\.mappingMode) { mode in
                model.settings.memoryMapping = mode != "disabled"
                model.settings.nativeBuffer = mode == "page-table"
                model.setString("memory-mapping-mode", mode)
            }) {
                ForEach(MemoryMappingMode.allCases) { Text($0.title).tag($0.rawValue) }
            } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Memory mapping")
                    Text(MemoryMappingMode(rawValue: model.settings.mappingMode)?.detail ?? "")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: binding(\.stretchDisplay) { model.setBool("stretch-display", $0) }) {
                described("Stretch to screen", "Fills the whole screen instead of keeping the Vita's shape.")
            }
            Toggle(isOn: binding(\.surfaceSync) { model.setBool("surface-sync", $0) }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Surface sync")
                    Text(model.settings.memoryMapping
                         ? "Lets games read back what they drew (bloom, reflections, lighting). Costs speed and breaks some games, so it's best turned on per game."
                         : "Needs GPU memory mapping, which is off: turn that on for surface sync to work.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }

        Section {
            Toggle(isOn: binding(\.asyncShaders) { model.setBool("async-shaders", $0) }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Compile shaders in the background")
                    Text("Less stutter when new things appear. They can be missing for a moment.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $fastCPU) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Fast CPU")
                    Text("Faster maths shortcuts: less CPU work per frame, so more speed and less heat. Off by default: some games get trails, physics or animation glitches with it.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $keepCool) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Keep the phone cool")
                    Text("Less heat and battery use, a bit less top speed. The game's background threads and shader compiling stay on the efficiency cores. At the first sign of warmth (or in Low Power Mode) it eases off more, before the phone gets hot: the renderer steps down and FSR, Bicubic or FXAA become Bilinear until it cools. Applies from the next game launch.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Performance")
        } footer: {
            Text("Game Mode and the fast CPU cores are used automatically.")
        }

        Section {
            Toggle(isOn: binding(\.shaderCache) { model.setBool("shader-cache", $0) }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Shader cache")
                    Text("Saves compiled shaders, so the next launch stutters less.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: binding(\.showCompileShaders) { model.setBool("show-compile-shaders", $0) }) {
                described("Show shader compile progress", "Shows a bar while saved shaders load at game start.")
            }
            Toggle(isOn: binding(\.importTextures) { model.setBool("import-textures", $0) }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Texture packs")
                    Text("Uses HD or translation texture packs from the textures › <title ID> folder.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Button("Clear all shader caches", role: .destructive) { confirmClearShaders = true }
        } header: {
            Text("Shaders & textures")
        } footer: {
            Text("Clearing the caches can fix games whose graphics broke after an update; each game compiles its shaders again the next time.")
        }
        .alert("Clear all shader caches?", isPresented: $confirmClearShaders) {
            Button("Clear", role: .destructive) {
                for app in model.apps { _ = v3k_delete_shader_cache(app.titleId) }
            }
            Button("Cancel", role: .cancel) {}
        }

        Section("System") {
            Picker("System language", selection: binding(\.sysLang) { model.setNumber("sys-lang", Double($0)) }) {
                ForEach(Self.languages, id: \.0) { Text($0.1).tag($0.0) }
            }
            Picker("Confirm button", selection: binding(\.sysButton) { model.setNumber("sys-button", Double($0)) }) {
                Text("✕ (Western games)").tag(1)
                Text("○ (Japanese games)").tag(0)
            }
            Toggle(isOn: binding(\.cpuOpt) { model.setBool("cpu-opt", $0) }) {
                described("CPU optimisations", "Much faster. Turn off only to test a game that breaks.")
            }
            Toggle(isOn: binding(\.pstvMode) { model.setBool("pstv-mode", $0) }) {
                described("PS TV mode", "Tells games they run on a PS TV. Some then use the controller's extra buttons; touch-only games may refuse to start.")
            }
            NavigationLink {
                PerformanceOverlaySettings()
            } label: {
                LabeledContent("Performance overlay", value: UserDefaults.standard.bool(forKey: "v3k.hud.enabled") ? "On" : "Off")
            }
        }

        Section("Game audio") {
            VStack(alignment: .leading) {
                Text("Volume: \(model.settings.audioVolume)%")
                Slider(value: Binding(
                    get: { Double(model.settings.audioVolume) },
                    set: { model.settings.audioVolume = Int($0) }
                ), in: 0...100, step: 5, onEditingChanged: { editing in
                    if !editing { model.setNumber("audio-volume", Double(model.settings.audioVolume)) }
                })
            }
            Toggle(isOn: binding(\.ngsEnable) { model.setBool("ngs-enable", $0) }) {
                described("NGS audio", "The Vita's sound mixer, used by many games for music and effects. Turn off only if a game hangs on audio (it will be silent).")
            }
        }
    }

    private static let languages: [(Int, String)] = [
        (1, "English (US)"), (18, "English (UK)"), (0, "Japanese"), (2, "French"), (3, "Spanish"), (4, "German"),
        (5, "Italian"), (6, "Dutch"), (7, "Portuguese (Portugal)"), (17, "Portuguese (Brazil)"), (8, "Russian"),
        (9, "Korean"), (10, "Chinese (Traditional)"), (11, "Chinese (Simplified)"), (12, "Finnish"), (13, "Swedish"),
        (14, "Danish"), (15, "Norwegian"), (16, "Polish"), (19, "Turkish"),
    ]

    private func label(for value: Double) -> String {
        let native = "\(Int(960 * value))×\(Int(544 * value))"
        let factor = value == value.rounded() ? "\(Int(value))×" : "\(value)×"
        return "\(factor) (\(native))"
    }

    /// Binds a settings field and saves it to the core whenever it changes
    private func binding<T>(_ keyPath: WritableKeyPath<EmulatorSettings, T>, save: @escaping (T) -> Void) -> Binding<T> {
        Binding(
            get: { model.settings[keyPath: keyPath] },
            set: { newValue in
                model.settings[keyPath: keyPath] = newValue
                save(newValue)
            })
    }
}

// MARK: - Ad hoc over the internet

/// Ad hoc over the internet through a Hinoki relay server (tools/adhoc-relay)
struct AdhocSection: View {
    @EnvironmentObject var model: LibraryModel
    @State private var server = ""
    @State private var room = ""
    @State private var key = ""
    @State private var name = ""
    @State private var status = "Off"
    @State private var loaded = false
    private let timer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()

    var body: some View {
        Section {
            LabeledContent("Player name") {
                TextField("3-16 letters or digits", text: $name)
                    .multilineTextAlignment(.trailing)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit(saveAll)
            }
            Toggle(isOn: Binding(get: { model.settings.adhocRelay }, set: { value in
                model.settings.adhocRelay = value
                model.setBool("adhoc-relay", value)
                refresh()
            })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Use a relay server")
                    Text("Sends a game's ad hoc (local wireless) multiplayer through a relay server, so friends anywhere can play together. Everyone needs the same server, room and game.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if model.settings.adhocRelay {
                LabeledContent("Server") {
                    TextField("myserver.duckdns.org", text: $server)
                        .multilineTextAlignment(.trailing)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                        .onSubmit(saveAll)
                }
                LabeledContent("Room") {
                    TextField("public", text: $room)
                        .multilineTextAlignment(.trailing)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .onSubmit(saveAll)
                }
                LabeledContent("Key") {
                    SecureField("none", text: $key)
                        .multilineTextAlignment(.trailing)
                        .onSubmit(saveAll)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("Status")
                    Text(status).font(.caption).foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Ad hoc over the internet (experimental)")
        } footer: {
            Text("Player name is your PS Vita user name, which games show to other players. Give each device its own name (letters, digits, - and _). The server runs from tools/adhoc-relay in the Hinoki repo (Docker). Add :port if it isn't 27360. The key is only needed if the server sets one. Hinoki connects when a game starts its network.")
        }
        .onAppear {
            if !loaded {
                server = model.settings.adhocRelayServer
                room = model.settings.adhocRelayRoom
                key = model.settings.adhocRelayKey
                name = model.settings.userName
                loaded = true
            }
            refresh()
        }
        .onDisappear(perform: saveAll)
        .onReceive(timer) { _ in refresh() }
    }

    private func saveAll() {
        guard loaded else { return }
        if server != model.settings.adhocRelayServer {
            model.settings.adhocRelayServer = server
            model.setString("adhoc-relay-server", server)
        }
        if room != model.settings.adhocRelayRoom {
            model.settings.adhocRelayRoom = room
            model.setString("adhoc-relay-room", room)
        }
        let cleanName = String(name.filter { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") }.prefix(16))
        if cleanName != model.settings.userName && cleanName.count >= 3 {
            model.settings.userName = cleanName
            model.setString("user-name", cleanName)
        }
        if cleanName != name { name = cleanName.count >= 3 ? cleanName : model.settings.userName }
        if key != model.settings.adhocRelayKey {
            model.settings.adhocRelayKey = key
            model.setString("adhoc-relay-key", key)
        }
    }

    private func refresh() {
        let text = takeString(v3k_adhoc_relay_status()) ?? "off"
        status = text == "off" ? "Not connected (connects when a game starts ad hoc)" : String(text.prefix(1)).uppercased() + String(text.dropFirst())
    }
}

// MARK: - JIT, credits, about

/// Inside LiveContainer/Strata the container starts the helpers: what (if anything) to change there
struct ContainerHelperSetupNote: View {
    static var placeDescription: String {
        switch v3k_ram_helpers_container_setup() {
        case Int32(V3K_RAM_SETUP_CONTAINER_READY): return "Container, helpers supported"
        case Int32(V3K_RAM_SETUP_SEPARATE_PROCESS): return "Container, separate process"
        case Int32(V3K_RAM_SETUP_OPTION_OFF): return "Container, helpers off"
        default: return "Container"
        }
    }

    var body: some View {
        let hint = String(cString: v3k_ram_helpers_container_hint())
        if !hint.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Label("Your container needs a setting changed", systemImage: "exclamationmark.triangle.fill")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.orange)
                Text(hint)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.vertical, 2)
        } else if v3k_ram_helpers_container_setup() == Int32(V3K_RAM_SETUP_CONTAINER_READY) {
            Label("Your container supports RAM helpers", systemImage: "checkmark.circle.fill")
                .font(.footnote)
                .foregroundStyle(.green)
        }
    }
}

struct MemorySection: View {
    @AppStorage("v3k.ramHelpers") private var ramHelpers = false
    @AppStorage("v3k.ramHelpers.count") private var helperCount = 2

    var body: some View {
        Section {
            Toggle(isOn: $ramHelpers) {
                Label("RAM helpers (beta)", systemImage: "memorychip")
            }
            .onChange(of: ramHelpers) { _, on in
                if on { v3k_ram_helpers_prepare() }
            }
            if ramHelpers {
                Stepper(value: $helperCount, in: 1...4) {
                    LabeledContent("Helper processes", value: "\(helperCount)")
                }
                ContainerHelperSetupNote()
            }
            NavigationLink {
                RAMCheckerView()
            } label: {
                Label("RAM checker", systemImage: "gauge.with.dots.needle.33percent")
            }
        } header: {
            Text("Memory")
        } footer: {
            Text("Helper processes, each with its own iOS memory limit, lend memory for the PS Vita's RAM, so big games are less likely to be closed by iOS. Takes effect at the next game launch. Some signers remove app extensions; the RAM checker shows whether the helper is there.")
        }
    }
}

/// Live memory numbers: the device, Vita3K's own use and limit, and what the RAM helpers lend
struct RAMCheckerView: View {
    @AppStorage("v3k.ramHelpers") private var ramHelpers = false
    @State private var probing = false
    @State private var probeResult: String?

    private func mib(_ bytes: UInt64) -> String {
        bytes >= 1024 * 1024 * 1024
            ? String(format: "%.2f GB", Double(bytes) / 1_073_741_824)
            : "\(bytes / (1024 * 1024)) MB"
    }

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { _ in
            let footprint = v3k_memory_footprint()
            let available = v3k_memory_available()
            let limit = footprint + available
            let backed = v3k_ram_helpers_backed()
            let inUse = v3k_ram_helpers_in_use()
            Form {
                Section("This device") {
                    LabeledContent("Installed RAM", value: mib(v3k_memory_physical()))
                }
                Section {
                    LabeledContent("Using now", value: mib(footprint))
                    LabeledContent("Left before iOS closes it", value: mib(available))
                    LabeledContent("Limit", value: mib(limit))
                    ProgressView(value: limit > 0 ? Double(footprint) / Double(limit) : 0)
                        .tint(available < 300 * 1024 * 1024 ? .red : available < 800 * 1024 * 1024 ? .orange : .green)
                } header: {
                    Text("Hinoki")
                } footer: {
                    Text(limit < 3 * 1024 * 1024 * 1024
                         ? "A limit under 3 GB usually means the Increased Memory Limit entitlement isn't active. Signing tools like Impactor or GetMoreRam can add it."
                         : "The Increased Memory Limit entitlement looks active.")
                }
                Section {
                    LabeledContent("Setting", value: ramHelpers ? "On" : "Off")
                    LabeledContent("Helper extension", value: v3k_ram_helpers_bundled() ? "Included" : "Missing")
                    if v3k_ram_helpers_in_livecontainer() {
                        LabeledContent("Running in", value: ContainerHelperSetupNote.placeDescription)
                        ContainerHelperSetupNote()
                    }
                    LabeledContent("Helpers running", value: "\(v3k_ram_helpers_live())")
                    LabeledContent("Lent this game", value: mib(backed))
                    LabeledContent("Of that, in use", value: mib(inUse))
                    Text(String(cString: v3k_ram_helpers_status()))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Button {
                        probing = true
                        probeResult = nil
                        DispatchQueue.global(qos: .userInitiated).async {
                            let lend = v3k_ram_helpers_probe()
                            DispatchQueue.main.async {
                                probing = false
                                probeResult = lend > 0 ? "A helper started and can lend about \(mib(lend))." : String(cString: v3k_ram_helpers_status())
                            }
                        }
                    } label: {
                        HStack {
                            Text("Test the RAM helper")
                            if probing { Spacer(); ProgressView() }
                        }
                    }
                    .disabled(probing || !v3k_ram_helpers_bundled())
                    if let probeResult {
                        Text(probeResult).font(.footnote)
                    }
                } header: {
                    Text("RAM helpers")
                } footer: {
                    Text("Helpers start with a game and lend memory for its RAM; after that iOS doesn't count it against Hinoki. The numbers update every second.")
                }
            }
        }
        .navigationTitle("RAM checker")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct JITSection: View {
    @EnvironmentObject var model: LibraryModel

    var body: some View {
        BuiltinJITSection()
        Section("JIT") {
            LabeledContent("Status", value: model.jitEnabled ? "Ready" : "Not ready")
            Text(model.jitStatus)
                .font(.footnote)
            if !model.jitEnabled {
                EnableJITButtons()
            }
            Text("Enable JIT opens your JIT app (StikDebug, StosDebug or TrollStore; through LiveContainer when Hinoki runs inside it) and passes Hinoki's own JIT script where needed, so there's no script to set up. Vita3K notices when JIT attaches, at any time while it's open, and gets ready by itself.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

struct CreditsSection: View {
    var body: some View {
        Section {
            CreditRow(name: "Vita3K", detail: "The PS Vita emulator this app is built on, by the Vita3K team and contributors.", url: "https://github.com/Vita3K/Vita3K")
            CreditRow(name: "Tsubomi by 125hz", detail: "iOS 26 JIT (dynarmic/oaknut patch and region pool), guest memory pre-reservation, audio session and display fixes.", url: "https://github.com/125hz/Tsubomi")
            CreditRow(name: "MrBouza", detail: "Fixed FSR on iOS, added the on-screen keyboard for game text input, and fixed the 16 MB JIT cache limit.", url: "https://github.com/MrBouza")
            CreditRow(name: "Vita3K-Plus by nckstwrt", detail: "Many game compatibility fixes (graphics, audio, saves, timing) ported from this Vita3K fork.", url: "https://github.com/nckstwrt/Vita3K-Plus")
            CreditRow(name: "MeloNX by Stossy11", detail: "The RAM donor technique behind RAM helpers (memory lent by helper extension processes).", url: "https://github.com/stossy11")
            CreditRow(name: "StikJIT by the StikDebug team", detail: "Built-in JIT: StikDebug's JIT engine as a library (MPL-2.0), with jkcoxson's idevice.", url: "https://github.com/StikDebug/StikJIT")
            CreditRow(name: "dynarmic & oaknut", detail: "ARM recompiler and arm64 assembler, by merryhime and contributors.", url: "https://github.com/merryhime/dynarmic")
            CreditRow(name: "MoltenVK", detail: "Vulkan on Apple's Metal, by the Khronos Group.", url: "https://github.com/KhronosGroup/MoltenVK")
            CreditRow(name: "SDL", detail: "Windowing, input and audio.", url: "https://github.com/libsdl-org/SDL")
        } header: {
            Text("Credits")
        } footer: {
            Text("Hinoki is free software under the GNU GPL v2 or later.")
        }
    }
}

struct AboutSection: View {
    @EnvironmentObject var model: LibraryModel
    @State private var report: URL?

    private var appVersion: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        let commit = info?["V3KCommit"] as? String
        return "\(version) (\(build)\(commit.map { ", " + $0 } ?? ""))"
    }

    private var previousLog: String {
        URL(fileURLWithPath: String(cString: v3k_log_path())).deletingLastPathComponent()
            .appendingPathComponent("vita3k-previous.log").path
    }

    var body: some View {
        Section("About") {
            HinokiLockup()
                .padding(.vertical, 6)
            LabeledContent("Version", value: appVersion)
            LabeledContent("Core", value: model.version)
            NavigationLink {
                SetupCheckView()
            } label: {
                Label("Setup check", systemImage: "stethoscope")
            }
            ShareLink(item: URL(fileURLWithPath: String(cString: v3k_log_path()))) {
                Label("Share log", systemImage: "square.and.arrow.up")
            }
            if FileManager.default.fileExists(atPath: previousLog) {
                ShareLink(item: URL(fileURLWithPath: previousLog)) {
                    Label("Share previous session's log", systemImage: "clock.arrow.circlepath")
                }
            }
            Button {
                var message: UnsafeMutablePointer<CChar>?
                if let path = takeString(v3k_make_problem_report(&message)) {
                    report = URL(fileURLWithPath: path)
                }
                _ = takeString(message)
            } label: {
                Label("Report a problem…", systemImage: "ladybug")
            }
            .sheet(item: Binding(get: { report.map(ShareItem.init) }, set: { if $0 == nil { report = nil } })) { item in
                ShareSheet(items: [item.url])
            }
            Link(destination: URL(string: "https://discord.gg/pZyQDPHKF")!) {
                Label("Join the Discord", systemImage: "bubble.left.and.bubble.right")
            }
            Text("Report a problem bundles the logs, the last crash report, your device and settings into one file to post on the Discord. Say which game and what went wrong.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

struct CreditRow: View {
    let name: String
    let detail: String
    let url: String

    var body: some View {
        Link(destination: URL(string: url)!) {
            VStack(alignment: .leading, spacing: 3) {
                Text(name).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                Text(detail).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Button mapping

/// Chooses which controller button triggers each Vita button. Applies to physical controllers; the
/// on-screen buttons follow it automatically so their labels stay right.
struct ControllerMappingView: View {
    private struct Slot: Identifiable {
        let id: Int32 // SDL_GamepadButton number of the slot
        let name: String
    }

    private let slots: [Slot] = [
        Slot(id: 0, name: "✕ Cross"), Slot(id: 1, name: "○ Circle"), Slot(id: 2, name: "□ Square"), Slot(id: 3, name: "△ Triangle"),
        Slot(id: 9, name: "L"), Slot(id: 10, name: "R"), Slot(id: 4, name: "Select"), Slot(id: 6, name: "Start"),
        Slot(id: 5, name: "PS button"), Slot(id: 7, name: "L3 (PS TV)"), Slot(id: 8, name: "R3 (PS TV)"),
        Slot(id: 11, name: "D-pad up"), Slot(id: 12, name: "D-pad down"), Slot(id: 13, name: "D-pad left"), Slot(id: 14, name: "D-pad right"),
    ]

    private let physicalNames: [Int32: String] = [
        0: "Bottom face (A / ✕)", 1: "Right face (B / ○)", 2: "Left face (X / □)", 3: "Top face (Y / △)",
        4: "View / Select / Share", 5: "Home / PS", 6: "Menu / Start / Options",
        7: "Left stick press", 8: "Right stick press", 9: "Left shoulder", 10: "Right shoulder",
        11: "D-pad up", 12: "D-pad down", 13: "D-pad left", 14: "D-pad right",
    ]

    @State private var binds: [Int32] = Array(0..<15)

    var body: some View {
        Form {
            Section {
                ForEach(slots) { slot in
                    Picker(slot.name, selection: Binding(
                        get: { binds[Int(slot.id)] },
                        set: { newValue in
                            binds[Int(slot.id)] = newValue
                            _ = v3k_set_controller_bind(slot.id, newValue)
                        })) {
                        ForEach(0..<15, id: \.self) { physical in
                            Text(physicalNames[Int32(physical)] ?? "Button \(physical)").tag(Int32(physical))
                        }
                    }
                }
            } footer: {
                Text("Pick the controller button for each PS Vita button. The on-screen controls follow this mapping automatically.")
            }

            Section {
                Button("Swap ✕ and ○ (Japanese style)") {
                    let cross = binds[0]
                    binds[0] = binds[1]
                    binds[1] = cross
                    _ = v3k_set_controller_bind(0, binds[0])
                    _ = v3k_set_controller_bind(1, binds[1])
                }
                Button("Reset to default", role: .destructive) {
                    v3k_reset_controller_binds()
                    load()
                }
            }
        }
        .navigationTitle("Button mapping")
        .onAppear(perform: load)
    }

    private func load() {
        guard let pointer = v3k_controller_binds_json() else { return }
        let json = String(cString: pointer)
        v3k_free_string(pointer)
        if let data = json.data(using: .utf8), let decoded = try? JSONDecoder().decode([Int32].self, from: data), decoded.count >= 15 {
            binds = decoded
        }
    }
}

// MARK: - Performance overlay

struct PerformanceOverlaySettings: View {
    @AppStorage("v3k.hud.enabled") private var enabled = false
    @AppStorage("v3k.hud.position") private var position = 0
    @AppStorage("v3k.hud.size") private var size = 1
    @AppStorage("v3k.hud.opacity") private var opacity = 0.35
    @State private var refresh = false

    var body: some View {
        Form {
            Section {
                Toggle("Show performance overlay", isOn: $enabled)
            } footer: {
                Text("A small see-through panel over the game. It applies the next time a game starts.")
            }
            if enabled {
                Section("Show") {
                    ForEach(HUDSettings.items) { item in
                        Toggle(isOn: Binding(
                            get: { _ = refresh; return HUDSettings.isOn(item.key) },
                            set: { UserDefaults.standard.set($0, forKey: "v3k.hud.\(item.key)"); refresh.toggle() }
                        )) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.title)
                                Text(item.detail).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                Section {
                    Picker("Position", selection: $position) {
                        ForEach(HUDSettings.positions.indices, id: \.self) { Text(HUDSettings.positions[$0]).tag($0) }
                    }
                    Picker("Text size", selection: $size) {
                        ForEach(HUDSettings.sizes.indices, id: \.self) { Text(HUDSettings.sizes[$0]).tag($0) }
                    }
                    VStack(alignment: .leading) {
                        Text("Background: \(Int(opacity * 100))%")
                        Slider(value: $opacity, in: 0...0.8, step: 0.05)
                    }
                } header: {
                    Text("Layout")
                } footer: {
                    Text("Thermal state is what iOS reports (it doesn't give apps a temperature). When it reaches Serious, iOS slows the chip down, and frame rates drop.")
                }
            }
        }
        .navigationTitle("Performance overlay")
    }
}

/// A setting's name with a short explanation under it
@ViewBuilder
func described(_ title: String, _ detail: String) -> some View {
    VStack(alignment: .leading, spacing: 2) {
        Text(title)
        Text(detail).font(.caption).foregroundStyle(.secondary)
    }
}
