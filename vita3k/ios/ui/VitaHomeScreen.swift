// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit

// The PS Vita home screen (Settings > Appearance > PS Vita home screen). Everything is laid out in the
// Vita's own 960 x 544 screen space and scaled to the phone's height; wider phones get more room at the
// sides. Geometry of the LiveArea comes from Vita3K's LiveArea (gate, frames, background), the start
// screen from Vita3K's old home screen code.

// MARK: - Screen space

/// Maps the Vita's 960 x 544 screen onto the view
struct VitaSpace {
    let size: CGSize
    /// Points per Vita pixel
    var s: CGFloat { min(size.height / 544, size.width / 960) }
    /// Width of the view in Vita pixels (960 on a Vita, more on a wide phone)
    var width: CGFloat { size.width / s }
    /// Left edge of the centred 960-wide area, in Vita pixels
    var inset: CGFloat { (width - 960) / 2 }

    func x(_ vita: CGFloat) -> CGFloat { (vita + inset) * s }
    func y(_ vita: CGFloat) -> CGFloat { vita * s }
    func len(_ vita: CGFloat) -> CGFloat { vita * s }
}

// MARK: - Page curl ("peel")

/// The fold of a page whose top-right corner is dragged to `point`: the part of the page that stays and the
/// flap that is folded over (reflected across the fold line)
enum PeelGeometry {
    static func split(size: CGSize, point: CGPoint) -> (keep: [CGPoint], flap: [CGPoint]) {
        let corner = CGPoint(x: size.width, y: 0)
        let rect = [CGPoint(x: 0, y: 0), corner, CGPoint(x: size.width, y: size.height), CGPoint(x: 0, y: size.height)]
        let nx = corner.x - point.x, ny = corner.y - point.y
        let len2 = nx * nx + ny * ny
        guard len2 > 0.5 else { return (rect, []) }
        let mx = (corner.x + point.x) / 2, my = (corner.y + point.y) / 2
        func side(_ q: CGPoint) -> CGFloat { (q.x - mx) * nx + (q.y - my) * ny }
        var keep: [CGPoint] = [], cut: [CGPoint] = []
        for i in 0..<4 {
            let a = rect[i], b = rect[(i + 1) % 4]
            let sa = side(a), sb = side(b)
            if sa <= 0 { keep.append(a) } else { cut.append(a) }
            if (sa <= 0) != (sb <= 0) {
                let t = sa / (sa - sb)
                let q = CGPoint(x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t)
                keep.append(q)
                cut.append(q)
            }
        }
        let flap = cut.map { q -> CGPoint in
            let d = 2 * side(q) / len2
            return CGPoint(x: q.x - d * nx, y: q.y - d * ny)
        }
        return (keep, flap)
    }
}

/// The page that stays, as an animatable shape (so the peel can be animated)
struct PeelPageShape: Shape {
    var point: CGPoint
    var animatableData: AnimatablePair<CGFloat, CGFloat> {
        get { AnimatablePair(point.x, point.y) }
        set { point = CGPoint(x: newValue.first, y: newValue.second) }
    }

    func path(in rect: CGRect) -> Path {
        let keep = PeelGeometry.split(size: rect.size, point: point).keep
        var path = Path()
        if let first = keep.first {
            path.move(to: first)
            keep.dropFirst().forEach { path.addLine(to: $0) }
            path.closeSubpath()
        }
        return path
    }
}

/// The folded-over back of the page
struct PeelFlapShape: Shape {
    var point: CGPoint
    var animatableData: AnimatablePair<CGFloat, CGFloat> {
        get { AnimatablePair(point.x, point.y) }
        set { point = CGPoint(x: newValue.first, y: newValue.second) }
    }

    func path(in rect: CGRect) -> Path {
        let flap = PeelGeometry.split(size: rect.size, point: point).flap
        var path = Path()
        if let first = flap.first {
            path.move(to: first)
            flap.dropFirst().forEach { path.addLine(to: $0) }
            path.closeSubpath()
        }
        return path
    }
}

/// A page with a dog-eared top-right corner that is peeled away with a finger, like the Vita's start screen
/// and LiveArea. Peeling it far enough (or flicking) finishes the curl and calls `onPeeled`.
struct Peelable<Content: View>: View {
    /// Size of the dog-ear at rest, in points
    let fold: CGFloat
    let onPeeled: () -> Void
    @ViewBuilder let content: () -> Content

    @State private var point: CGPoint?
    @State private var dragging = false
    @State private var finishing = false

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size
            let rest = CGPoint(x: size.width - fold, y: fold)
            let current = point ?? rest
            ZStack(alignment: .topLeading) {
                content()
                    .frame(width: size.width, height: size.height)
                    .clipShape(PeelPageShape(point: current))
                    // Shadow the page casts under the flap
                    .shadow(color: .black.opacity(0.35), radius: 10, x: -4, y: 6)
                PeelFlapShape(point: current)
                    .fill(LinearGradient(colors: [Color(white: 0.97), Color(white: 0.78), Color(white: 0.9)],
                                         startPoint: .topTrailing, endPoint: .bottomLeading))
                    .shadow(color: .black.opacity(0.45), radius: 8, x: -3, y: 5)
                    .allowsHitTesting(false)
                // Where a finger can grab the corner
                Color.clear
                    .contentShape(Rectangle())
                    .frame(width: max(fold * 2.6, 90), height: max(fold * 2.6, 90))
                    // The gesture belongs to the corner area only (position() comes after it): a gesture added
                    // after position() would cover the whole page and take every touch
                    .gesture(
                        DragGesture(minimumDistance: 0, coordinateSpace: .named("peel"))
                            .onChanged { value in
                                guard !finishing else { return }
                                dragging = true
                                point = CGPoint(x: min(value.location.x, size.width - 4), y: max(value.location.y, 4))
                            }
                            .onEnded { value in
                                guard !finishing else { return }
                                dragging = false
                                let travel = hypot(size.width - value.predictedEndLocation.x, value.predictedEndLocation.y)
                                let moved = hypot(value.translation.width, value.translation.height)
                                // A tap on the corner finishes the peel too (no need to find the exact drag)
                                if travel > size.width * 0.38 || moved < 8 {
                                    finishing = true
                                    UIImpactFeedbackGenerator(style: .light).impactOccurred()
                                    withAnimation(.easeIn(duration: 0.38)) {
                                        point = CGPoint(x: -size.width * 1.2, y: size.height * 2.2)
                                    }
                                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                                        onPeeled()
                                        point = nil
                                        finishing = false
                                    }
                                } else {
                                    withAnimation(.spring(response: 0.35, dampingFraction: 0.7)) { point = nil }
                                }
                            }
                    )
                    .position(x: size.width - max(fold * 1.3, 45), y: max(fold * 1.3, 45))
            }
            .coordinateSpace(name: "peel")
        }
        // Control Center, a call or the app switcher can take the touch mid-fold: SwiftUI then never
        // ends the drag, which left the page stuck half folded. Put it back
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            guard dragging, !finishing else { return }
            dragging = false
            point = nil
        }
    }
}

// MARK: - Layout of the home pages

enum VitaHomeLayout {
    static let perPage = 10
    /// Bubbles per row, top to bottom
    static let rows = [3, 4, 3]
    /// Row centres and the distance between bubbles in each row, measured from a real Vita (960x544 pixels)
    static let rowY: [CGFloat] = [138, 290, 440]
    static let pitches: [CGFloat] = [229, 223, 229]
    static let centerX: CGFloat = 482
    static let bubble: CGFloat = 148

    /// Centre of slot `index` (0 ... 9) on a page, in Vita pixels
    static func slot(_ index: Int) -> CGPoint {
        var remaining = index
        for (row, count) in rows.enumerated() {
            if remaining < count {
                let x = centerX + (CGFloat(remaining) - CGFloat(count - 1) / 2) * pitches[row]
                return CGPoint(x: x, y: rowY[row])
            }
            remaining -= count
        }
        return CGPoint(x: centerX, y: 282)
    }
}

/// One thing on the home screen: a game, or one of the system applications
enum VitaHomeItem: Identifiable, Hashable {
    case game(GameApp)
    case search, contentManager, settings, themes, music, classicLibrary

    var id: String {
        switch self {
        case .game(let app): "game." + app.titleId
        case .search: "sys.search"
        case .contentManager: "sys.content"
        case .settings: "sys.settings"
        case .themes: "sys.themes"
        case .music: "sys.music"
        case .classicLibrary: "sys.library"
        }
    }

    var title: String {
        switch self {
        case .game(let app): app.title
        case .search: "Search"
        case .contentManager: "Content Manager"
        case .settings: "Settings"
        case .themes: "Themes"
        case .music: "Music"
        case .classicLibrary: "Library"
        }
    }
}

// MARK: - Home screen

struct VitaHomeView: View {
    let apps: [GameApp]
    let onLaunch: (GameApp) -> Void
    let onDetails: (GameApp) -> Void
    let onDelete: (GameApp) -> Void
    let onDeleteSave: (GameApp) -> Void
    let onImportFiles: () -> Void
    let onImportFolder: () -> Void
    let onEnterZrif: () -> Void
    /// UI preview harness only (.ci/ui-preview): "app:<title ID>" opens that LiveArea, "panel:<name>" a panel
    var preview: String? = nil

    @EnvironmentObject private var model: LibraryModel
    @ObservedObject private var builtinJIT = BuiltinJIT.shared
    @AppStorage("v3k.vitaHome") private var vitaHome = false
    @AppStorage("v3k.vitaHome.lockScreen") private var showStartScreen = true
    @AppStorage("v3k.vitaHome.theme") private var themeId = VitaTheme.defaultId
    @State private var locked = true
    @State private var page = 0
    @State private var openApp: GameApp?
    @State private var searching = false
    @State private var infoApp: GameApp?
    @State private var panel: VitaPanelKind?

    private var theme: VitaTheme { VitaTheme.current() }

    private var items: [VitaHomeItem] {
        // The Vita puts the system applications on the first page and new games after them
        let games = apps.sorted { ($0.lastPlayed, $1.title) > ($1.lastPlayed, $0.title) }.map { VitaHomeItem.game($0) }
        // Content Manager stays a bubble (always on the first page); the other system applications are
        // buttons in the information bar
        return [.contentManager] + games
    }

    private var recentApps: [GameApp] {
        Array(apps.filter { $0.lastPlayed > 0 }.sorted { $0.lastPlayed > $1.lastPlayed }.prefix(4))
    }

    private var pages: [[VitaHomeItem]] {
        let all = items
        return stride(from: 0, to: all.count, by: VitaHomeLayout.perPage).map {
            Array(all[$0..<min($0 + VitaHomeLayout.perPage, all.count)])
        }
    }

    var body: some View {
        GeometryReader { proxy in
            let space = VitaSpace(size: proxy.size)
            ZStack {
                VitaWallpaper(theme: theme, page: page, animated: openApp == nil && panel == nil && !searching && !locked)
                    .animation(.easeInOut(duration: 0.35), value: themeId)

                homePages(space: space)
                    // Opening a game zooms through the bubbles into its LiveArea
                    .scaleEffect(openApp == nil ? 1 : 1.12)
                    .blur(radius: openApp == nil ? 0 : 6)
                    .opacity(openApp == nil ? 1 : 0)
                    // Nothing underneath takes touches while a LiveArea, panel or search is open
                    .allowsHitTesting(openApp == nil && panel == nil && !searching && !locked)

                VitaPageDots(count: pages.count, current: page, space: space)
                    .opacity(openApp == nil ? 1 : 0)

                VitaPageArrows(page: $page, count: pages.count, space: space)
                    .opacity(openApp == nil ? 1 : 0)
                    .allowsHitTesting(openApp == nil && panel == nil && !searching && !locked)

                if let app = openApp {
                    Peelable(fold: space.len(44), onPeeled: { openApp = nil }) {
                        VitaLiveArea(app: app, space: space, theme: theme,
                                     start: { launch(app) }, info: { infoApp = app }, settings: { onDetails(app) })
                    }
                    .transition(.asymmetric(insertion: .scale(scale: 0.86).combined(with: .opacity),
                                            removal: .scale(scale: 0.94).combined(with: .opacity)))
                    .zIndex(2)
                }


                if let kind = panel {
                    Peelable(fold: space.len(44), onPeeled: { panel = nil }) {
                        VitaPanel(kind: kind, space: space, model: model, notices: notices, close: { withAnimation(.easeOut(duration: 0.2)) { panel = nil } },
                                  importFiles: onImportFiles, importFolder: onImportFolder, enterZrif: onEnterZrif)
                    }
                    .transition(.asymmetric(insertion: .move(edge: .trailing).combined(with: .opacity), removal: .opacity))
                    .zIndex(3.5)
                }

                if searching {
                    VitaSearch(apps: apps, space: space, close: { withAnimation(.easeOut(duration: 0.2)) { searching = false } }) { app in
                        withAnimation(.easeOut(duration: 0.2)) { searching = false }
                        open(app)
                    }
                    .transition(.opacity)
                    .zIndex(4)
                }

                if locked && showStartScreen {
                    Peelable(fold: space.len(44), onPeeled: { locked = false }) {
                        VitaStartScreen(space: space, theme: theme)
                    }
                    .zIndex(5)
                }
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
            // JIT setup freezes the app for about a second while the debugger works: say so, so the
            // pause doesn't look like a glitch. Static on purpose (a spinner would just stop mid-turn)
            .overlay(alignment: .bottom) {
                if builtinJIT.busy || model.jitPreparing {
                    Label("Enabling JIT…", systemImage: "bolt.fill")
                        .font(VitaFont.font(space.len(15)))
                        .foregroundStyle(.white)
                        .padding(.horizontal, space.len(14))
                        .padding(.vertical, space.len(7))
                        .background(Capsule().fill(Color.black.opacity(0.55)))
                        .padding(.bottom, space.len(18))
                        .allowsHitTesting(false)
                        .transition(.opacity)
                }
            }
            // The information bar is its own layer above everything, so nothing that comes and goes
            // underneath (a new wallpaper, a LiveArea, a panel) can ever cover or reorder it
            .overlay(alignment: .top) {
                // The start screen draws its own bar
                if !(locked && showStartScreen) {
                VitaInfoBar(space: space, theme: theme, notices: notices.count,
                            openNotices: openApp == nil && panel == nil ? { openPanel(.notifications) } : nil, recent: recentApps,
                            buttons: locked || searching ? [] : [
                                VitaBarButton(symbol: "magnifyingglass", title: "Search") { tap(.search) },
                                VitaBarButton(symbol: "gearshape.fill", title: "Settings") { tap(.settings) },
                                VitaBarButton(symbol: "paintpalette.fill", title: "Themes") { tap(.themes) },
                                VitaBarButton(symbol: "music.note", title: "Music") { tap(.music) },
                                VitaBarButton(symbol: "square.grid.2x2.fill", title: "Library") { tap(.classicLibrary) },
                            ])
                    .transition(.opacity)
                }
            }
            .clipped()
        }
        .ignoresSafeArea()
        .background(Color.black.ignoresSafeArea())
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .sheet(item: $infoApp) { app in GameInfoView(app: app) }
        .onAppear(perform: applyPreview)
        // Deleting games can leave fewer pages than the one being shown
        .onChange(of: pages.count) { _, count in
            if page >= count { page = max(count - 1, 0) }
        }
    }

    private func homePages(space: VitaSpace) -> some View {
        // The home pages scroll sideways, one page at a time
        ScrollView(.horizontal) {
            LazyHStack(spacing: 0) {
                ForEach(Array(pages.enumerated()), id: \.offset) { index, items in
                    VitaHomePage(items: items, space: space, tap: tap, menu: menu)
                        .frame(width: space.size.width, height: space.size.height)
                        .id(index)
                }
            }
            .scrollTargetLayout()
        }
        .scrollTargetBehavior(.paging)
        .scrollIndicators(.hidden)
        .scrollPosition(id: Binding<Int?>(get: { page }, set: { page = $0 ?? 0 }))
        .animation(.easeOut(duration: 0.25), value: page)
    }

    private func tap(_ item: VitaHomeItem) {
        switch item {
        case .game(let app): open(app)
        case .search: withAnimation(.easeOut(duration: 0.2)) { searching = true }
        case .contentManager: openPanel(.contentManager)
        case .settings: openPanel(.settings)
        case .themes: openPanel(.themes)
        case .music: openPanel(.music)
        case .classicLibrary: vitaHome = false
        }
    }

    private func applyPreview() {
        guard let preview, preview != "lock" else { return }
        locked = false
        if preview.hasPrefix("app:"), let app = apps.first(where: { $0.titleId == preview.dropFirst(4) }) {
            openApp = app
        } else if preview.hasPrefix("panel:") {
            switch preview.dropFirst(6) {
            case "themes": panel = .themes
            case "settings": panel = .settings
            case "music": panel = .music
            case "content": panel = .contentManager
            default: break
            }
        }
    }

    private func openPanel(_ kind: VitaPanelKind) {
        withAnimation(.spring(response: 0.42, dampingFraction: 0.86)) { panel = kind }
    }

    /// What the notification bubble counts: things worth the player's attention
    private var notices: [String] {
        var list: [String] = []
        if !model.hasFirmware { list.append("The PS Vita firmware isn't installed. Install it from Content Manager (PSVUPDAT.PUP).") }
        else if !model.hasFonts { list.append("The firmware's fonts aren't installed. Install the font package from Content Manager.") }
        if model.jitPreparing { list.append("Preparing JIT (\(model.jitProgress)/\(model.jitTarget))…") }
        else if !model.jitEnabled { list.append(model.jitCanRequest ? "JIT is attached; it starts when you launch a game." : "JIT is off: \(model.jitStatus)") }
        if apps.isEmpty { list.append("No games yet. Open Content Manager to install some.") }
        if UserDefaults.standard.bool(forKey: "v3k.ramHelpers") {
            let hint = String(cString: v3k_ram_helpers_container_hint())
            if !hint.isEmpty { list.append("RAM helpers: \(hint)") }
        }
        return list
    }

    private func open(_ app: GameApp) {
        UIImpactFeedbackGenerator(style: .medium).impactOccurred(intensity: 0.9)
        withAnimation(.spring(response: 0.48, dampingFraction: 0.84)) { openApp = app }
    }

    private func launch(_ app: GameApp) {
        onLaunch(app)
        // Back on the home screen (not this LiveArea) when the game ends
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { openApp = nil }
    }

    @ViewBuilder
    private func menu(_ item: VitaHomeItem) -> some View {
        if case .game(let app) = item {
            Button { onLaunch(app) } label: { Label("Start", systemImage: "play.fill") }
            Button { infoApp = app } label: { Label("Information", systemImage: "info.circle") }
            Button { onDetails(app) } label: { Label("Game settings, patches & saves", systemImage: "slider.horizontal.3") }
            Button(role: .destructive) { onDeleteSave(app) } label: { Label("Delete save data", systemImage: "externaldrive.badge.minus") }
            Button(role: .destructive) { onDelete(app) } label: { Label("Delete game", systemImage: "trash") }
        }
    }
}

// MARK: - Wallpaper

/// The theme's wallpaper for the page; the default theme is the Vita's blue with the PlayStation shapes
/// drifting behind the bubbles
struct VitaWallpaper: View {
    let theme: VitaTheme
    let page: Int
    /// False while something covers the home screen: the drawn backgrounds stop animating
    var animated = true

    var body: some View {
        ZStack {
            if let id = theme.style {
                if id == "shapes" {
                    VitaDefaultBackground()
                } else if let style = VitaBackgroundStyle.all.first(where: { $0.id == id }) {
                    VitaStyledBackground(style: style, animated: animated)
                }
            } else if let image = VitaFirmware.image(atPath: theme.wallpapers[min(page, theme.wallpapers.count - 1)]) {
                Image(uiImage: image).resizable().scaledToFill()
                    .id(page)
                    .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.4), value: page)
        .ignoresSafeArea()
    }
}

/// A drawn Frutiger Aero background: a glossy gradient, light, waves or hills, rising bubbles
struct VitaBackgroundStyle: Identifiable {
    let id: String
    let name: String
    let sky: [Color]
    var wave: Color = .white
    var rays = false
    var hills: Color? = nil
    var bubbles = 12
    var shapes = false
    var stars = false // twinkling points of light (night skies)
    var aurora = false // slow ribbons of light across the sky
    var petals = false // blossom petals drifting down

    private static func rgb(_ r: Double, _ g: Double, _ b: Double) -> Color { Color(red: r, green: g, blue: b) }

    static let all: [VitaBackgroundStyle] = [
        VitaBackgroundStyle(id: "vita", name: "Vita Blue", sky: [rgb(0.14, 0.40, 0.88), rgb(0.06, 0.22, 0.66), rgb(0.02, 0.10, 0.40)], bubbles: 10, shapes: true),
        VitaBackgroundStyle(id: "meadow", name: "Aero Meadow", sky: [rgb(0.30, 0.68, 0.98), rgb(0.56, 0.84, 1.0), rgb(0.86, 0.96, 1.0)],
                            rays: true, hills: rgb(0.28, 0.74, 0.24), bubbles: 14),
        VitaBackgroundStyle(id: "reef", name: "Aqua Reef", sky: [rgb(0.0, 0.62, 0.80), rgb(0.0, 0.40, 0.66), rgb(0.0, 0.16, 0.40)],
                            wave: rgb(0.7, 1.0, 1.0), rays: true, bubbles: 24),
        VitaBackgroundStyle(id: "sunset", name: "Sunset Glass", sky: [rgb(1.0, 0.66, 0.26), rgb(0.90, 0.30, 0.46), rgb(0.34, 0.14, 0.56)], bubbles: 14),
        VitaBackgroundStyle(id: "lagoon", name: "Emerald Lagoon", sky: [rgb(0.22, 0.88, 0.66), rgb(0.04, 0.60, 0.56), rgb(0.02, 0.28, 0.40)],
                            rays: true, bubbles: 16),
        VitaBackgroundStyle(id: "violet", name: "Violet Dream", sky: [rgb(0.78, 0.58, 1.0), rgb(0.50, 0.30, 0.92), rgb(0.18, 0.08, 0.54)], bubbles: 18),
        VitaBackgroundStyle(id: "shapes", name: "PlayStation Shapes", sky: [rgb(0.36, 0.62, 0.86), rgb(0.10, 0.30, 0.62), rgb(0.03, 0.10, 0.30)]),
        VitaBackgroundStyle(id: "aurora", name: "Midnight Aurora", sky: [rgb(0.02, 0.10, 0.26), rgb(0.02, 0.18, 0.30), rgb(0.01, 0.05, 0.14)],
                            wave: rgb(0.55, 1.0, 0.85), bubbles: 6, stars: true, aurora: true),
        VitaBackgroundStyle(id: "sakura", name: "Sakura Breeze", sky: [rgb(1.0, 0.86, 0.92), rgb(0.98, 0.66, 0.80), rgb(0.80, 0.44, 0.66)],
                            bubbles: 8, petals: true),
        VitaBackgroundStyle(id: "frost", name: "Frosted Glass", sky: [rgb(0.84, 0.93, 1.0), rgb(0.60, 0.78, 0.94), rgb(0.34, 0.52, 0.76)],
                            wave: rgb(0.92, 0.98, 1.0), rays: true, bubbles: 22),
        VitaBackgroundStyle(id: "crimson", name: "Crimson Pulse", sky: [rgb(0.80, 0.10, 0.18), rgb(0.48, 0.04, 0.12), rgb(0.14, 0.01, 0.05)],
                            wave: rgb(1.0, 0.72, 0.72), bubbles: 10, shapes: true),
        VitaBackgroundStyle(id: "starfield", name: "Starlit Night", sky: [rgb(0.10, 0.12, 0.34), rgb(0.05, 0.06, 0.20), rgb(0.01, 0.01, 0.08)],
                            wave: rgb(0.70, 0.76, 1.0), bubbles: 4, stars: true),
    ]
}

struct VitaStyledBackground: View {
    let style: VitaBackgroundStyle
    var animated = true
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private func noise(_ i: Int, _ k: Double) -> CGFloat {
        let x = sin(Double(i) * 12.9898 + k * 78.233) * 43758.5453
        return CGFloat(x - floor(x))
    }

    var body: some View {
        let paused = !animated || reduceMotion || ProcessInfo.processInfo.isLowPowerModeEnabled
        TimelineView(.animation(minimumInterval: 1 / 24, paused: paused)) { timeline in
            Canvas { context, size in
                let t = timeline.date.timeIntervalSinceReferenceDate
                let w = size.width, h = size.height
                context.fill(Path(CGRect(origin: .zero, size: size)), with: .linearGradient(
                    Gradient(colors: style.sky), startPoint: CGPoint(x: w * 0.3, y: 0), endPoint: CGPoint(x: w * 0.7, y: h)))
                // Soft light from the top left
                context.fill(Path(ellipseIn: CGRect(x: -w * 0.25, y: -h * 0.9, width: w * 1.1, height: h * 1.6)),
                             with: .radialGradient(Gradient(colors: [.white.opacity(0.38), .clear]),
                                                   center: CGPoint(x: w * 0.2, y: 0), startRadius: 0, endRadius: w * 0.6))
                if style.rays {
                    for i in 0..<7 {
                        let a = 0.2 + Double(i) * 0.16 + sin(t * 0.12 + Double(i)) * 0.03
                        let origin = CGPoint(x: w * 0.12, y: -h * 0.1)
                        var ray = Path()
                        ray.move(to: origin)
                        ray.addLine(to: CGPoint(x: origin.x + CGFloat(cos(a)) * h * 2.4, y: origin.y + CGFloat(sin(a)) * h * 2.4))
                        ray.addLine(to: CGPoint(x: origin.x + CGFloat(cos(a + 0.07)) * h * 2.4, y: origin.y + CGFloat(sin(a + 0.07)) * h * 2.4))
                        ray.closeSubpath()
                        context.fill(ray, with: .color(.white.opacity(0.07)))
                    }
                }
                if style.stars {
                    for i in 0..<70 {
                        let rate: Double = 0.6 + Double(noise(i, 11)) * 1.4
                        let wave: Double = sin(t * rate + Double(i))
                        let twinkle: Double = 0.35 + 0.65 * abs(wave)
                        let r: CGFloat = 0.6 + noise(i, 12) * 1.6
                        let sx: CGFloat = noise(i, 13) * w - r
                        let sy: CGFloat = noise(i, 14) * h * 0.78 - r
                        let star = CGRect(x: sx, y: sy, width: r * 2, height: r * 2)
                        context.fill(Path(ellipseIn: star), with: .color(.white.opacity(twinkle * 0.85)))
                    }
                }
                if style.aurora {
                    // Three soft ribbons that sway slowly, green to teal to violet
                    let colors: [Color] = [Color(red: 0.3, green: 1.0, blue: 0.7), Color(red: 0.2, green: 0.8, blue: 1.0), Color(red: 0.7, green: 0.45, blue: 1.0)]
                    for band in 0..<3 {
                        let b = Double(band)
                        let top: CGFloat = h * (0.18 + CGFloat(band) * 0.11)
                        let thickness: CGFloat = h * (0.16 - CGFloat(band) * 0.03)
                        let upperSpeed: Double = 0.10 + b * 0.04
                        let lowerSpeed: Double = 0.08 + b * 0.03
                        var ribbon = Path()
                        var x: CGFloat = -20
                        ribbon.move(to: CGPoint(x: x, y: top))
                        while x <= w + 20 {
                            let phase: Double = Double(x / w) * 3.4 + t * upperSpeed + b * 2.1
                            let y: CGFloat = top + CGFloat(sin(phase)) * h * 0.07
                            ribbon.addLine(to: CGPoint(x: x, y: y))
                            x += 16
                        }
                        while x >= -20 {
                            let phase: Double = Double(x / w) * 2.6 + t * lowerSpeed + b
                            let y: CGFloat = top + thickness + CGFloat(sin(phase)) * h * 0.05
                            ribbon.addLine(to: CGPoint(x: x, y: y))
                            x -= 16
                        }
                        ribbon.closeSubpath()
                        let tint = colors[band]
                        let glow = Gradient(colors: [tint.opacity(0.0), tint.opacity(0.28), tint.opacity(0.0)])
                        let from = CGPoint(x: 0, y: top - h * 0.06)
                        let to = CGPoint(x: 0, y: top + thickness + h * 0.06)
                        context.fill(ribbon, with: .linearGradient(glow, startPoint: from, endPoint: to))
                    }
                }
                if style.petals {
                    let petalFill = Color(red: 1.0, green: 0.93, blue: 0.96).opacity(0.85)
                    let petalEdge = Color(red: 0.95, green: 0.6, blue: 0.75).opacity(0.5)
                    for i in 0..<26 {
                        let size: CGFloat = (0.012 + noise(i, 15) * 0.014) * h
                        let speed: Double = 0.02 + Double(noise(i, 16)) * 0.025
                        let fall = CGFloat((t * speed + Double(noise(i, 17))).truncatingRemainder(dividingBy: 1))
                        let sway = CGFloat(sin(t * 0.5 + Double(i))) * 0.04
                        let across: CGFloat = (noise(i, 18) + sway + fall * 0.15).truncatingRemainder(dividingBy: 1.1)
                        let cy: CGFloat = -size + fall * (h + size * 2)
                        let spin: Double = t * (0.6 + Double(noise(i, 19))) + Double(i)
                        var local = context
                        local.translateBy(x: across * w, y: cy)
                        local.rotate(by: .radians(spin))
                        let petal = Path(ellipseIn: CGRect(x: -size, y: -size * 0.55, width: size * 2, height: size * 1.1))
                        local.fill(petal, with: .color(petalFill))
                        local.stroke(petal, with: .color(petalEdge), lineWidth: 0.8)
                    }
                }
                if style.shapes {
                    for i in 0..<12 {
                        let side = (0.06 + noise(i, 1) * 0.1) * h
                        let cx = (noise(i, 2) + CGFloat(sin(t * 0.15 + Double(i))) * 0.02) * w
                        var cy = (noise(i, 3) - CGFloat(t * (0.006 + Double(noise(i, 4)) * 0.01)).truncatingRemainder(dividingBy: 1.3))
                        if cy < -0.15 { cy += 1.3 }
                        var local = context
                        local.translateBy(x: cx, y: cy * h)
                        local.rotate(by: .radians(t * 0.1 * Double(noise(i, 5) - 0.5) + Double(i)))
                        local.opacity = 0.14
                        var p = Path()
                        let r = side / 2
                        switch i % 4 {
                        case 0: p.move(to: CGPoint(x: 0, y: -r)); p.addLine(to: CGPoint(x: r, y: r * 0.7)); p.addLine(to: CGPoint(x: -r, y: r * 0.7)); p.closeSubpath()
                        case 1: p.addEllipse(in: CGRect(x: -r, y: -r, width: side, height: side))
                        case 2: p.move(to: CGPoint(x: -r, y: -r)); p.addLine(to: CGPoint(x: r, y: r)); p.move(to: CGPoint(x: r, y: -r)); p.addLine(to: CGPoint(x: -r, y: r))
                        default: p.addRect(CGRect(x: -r, y: -r, width: side, height: side))
                        }
                        local.stroke(p, with: .color(.white), lineWidth: max(side * 0.07, 2))
                    }
                }
                if let hills = style.hills {
                    for layer in 0..<2 {
                        var hill = Path()
                        let base = h * (0.80 + CGFloat(layer) * 0.07)
                        hill.move(to: CGPoint(x: 0, y: h))
                        var x: CGFloat = 0
                        while x <= w + 12 {
                            hill.addLine(to: CGPoint(x: x, y: base + CGFloat(sin(Double(x / w) * 4.2 + Double(layer) * 2)) * h * 0.05))
                            x += 12
                        }
                        hill.addLine(to: CGPoint(x: w, y: h)); hill.closeSubpath()
                        context.fill(hill, with: .linearGradient(Gradient(colors: [hills.opacity(0.95 - Double(layer) * 0.2), hills.opacity(0.55)]),
                                                                 startPoint: CGPoint(x: 0, y: base), endPoint: CGPoint(x: 0, y: h)))
                    }
                }
                // The white waves along the bottom
                for layer in 0..<3 {
                    var wave = Path()
                    let base = h * (0.80 + CGFloat(layer) * 0.07)
                    wave.move(to: CGPoint(x: 0, y: h))
                    var x: CGFloat = 0
                    while x <= w + 12 {
                        let phase = Double(x / w) * .pi * (1.6 + Double(layer) * 0.5) + t * (0.25 + Double(layer) * 0.12) + Double(layer) * 1.7
                        wave.addLine(to: CGPoint(x: x, y: base + CGFloat(sin(phase)) * h * (0.035 - CGFloat(layer) * 0.006)))
                        x += 12
                    }
                    wave.addLine(to: CGPoint(x: w, y: h)); wave.closeSubpath()
                    context.fill(wave, with: .color(style.wave.opacity([0.30, 0.20, 0.13][layer])))
                }
                // Glass bubbles drifting up
                for i in 0..<style.bubbles {
                    let radius = (0.012 + noise(i, 6) * noise(i, 6) * 0.07) * h * 2
                    let speed = 0.012 + Double(noise(i, 7)) * 0.02
                    let rise = CGFloat((t * speed + Double(noise(i, 8))).truncatingRemainder(dividingBy: 1))
                    let cx = noise(i, 9) * w + CGFloat(sin(t * 0.4 + Double(i))) * radius * 0.4
                    let cy = h + radius - rise * (h + radius * 2)
                    let rect = CGRect(x: cx - radius, y: cy - radius, width: radius * 2, height: radius * 2)
                    context.fill(Path(ellipseIn: rect), with: .radialGradient(Gradient(colors: [.white.opacity(0.02), .white.opacity(0.22)]),
                                                                              center: CGPoint(x: cx, y: cy), startRadius: 0, endRadius: radius))
                    context.stroke(Path(ellipseIn: rect), with: .color(.white.opacity(0.5)), lineWidth: 1)
                    context.fill(Path(ellipseIn: CGRect(x: cx - radius * 0.55, y: cy - radius * 0.75, width: radius * 0.8, height: radius * 0.4)),
                                 with: .color(.white.opacity(0.5)))
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// The default Vita home background: deep blue light with the four PlayStation shapes floating slowly
struct VitaDefaultBackground: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private struct Floater {
        let kind: Int // 0 triangle, 1 circle, 2 cross, 3 square
        let x: CGFloat, y: CGFloat, size: CGFloat
        let speed: Double, phase: Double, spin: Double
    }

    private static let shapes: [Floater] = {
        var generator = SystemRandomNumberGenerator()
        return (0..<22).map { i in
            Floater(kind: i % 4,
                  x: CGFloat.random(in: 0...1, using: &generator),
                  y: CGFloat.random(in: 0...1, using: &generator),
                  size: CGFloat.random(in: 0.05...0.16, using: &generator),
                  speed: Double.random(in: 0.006...0.018, using: &generator),
                  phase: Double.random(in: 0...(2 * .pi), using: &generator),
                  spin: Double.random(in: -0.15...0.15, using: &generator))
        }
    }()

    var body: some View {
        TimelineView(.animation(minimumInterval: 1 / 30, paused: reduceMotion)) { timeline in
            Canvas { context, size in
                let t = timeline.date.timeIntervalSinceReferenceDate
                // Light from the top left fading to the Vita's deep blue
                context.fill(Path(CGRect(origin: .zero, size: size)), with: .linearGradient(
                    Gradient(colors: [Color(red: 0.36, green: 0.62, blue: 0.86), Color(red: 0.10, green: 0.30, blue: 0.62), Color(red: 0.03, green: 0.10, blue: 0.30)]),
                    startPoint: .zero, endPoint: CGPoint(x: size.width * 0.6, y: size.height)))
                context.fill(Path(ellipseIn: CGRect(x: -size.width * 0.2, y: -size.height * 0.6, width: size.width * 1.1, height: size.height * 1.2)),
                             with: .radialGradient(Gradient(colors: [.white.opacity(0.22), .clear]),
                                                   center: CGPoint(x: size.width * 0.25, y: 0), startRadius: 0, endRadius: size.width * 0.6))
                for shape in Self.shapes {
                    let side = shape.size * size.height
                    // Each shape rises slowly and sways; it comes back at the bottom when it leaves the top
                    var y = (shape.y - CGFloat(t * shape.speed).truncatingRemainder(dividingBy: 1.3))
                    if y < -0.15 { y += 1.3 }
                    let x = shape.x + CGFloat(sin(t * 0.2 + shape.phase)) * 0.02
                    let center = CGPoint(x: x * size.width, y: y * size.height)
                    var local = context
                    local.translateBy(x: center.x, y: center.y)
                    local.rotate(by: .radians(t * shape.spin + shape.phase))
                    local.opacity = 0.16
                    local.stroke(path(kind: shape.kind, side: side), with: .color(.white), lineWidth: max(side * 0.07, 2))
                }
            }
        }
        .ignoresSafeArea()
    }

    private func path(kind: Int, side: CGFloat) -> Path {
        let r = side / 2
        var p = Path()
        switch kind {
        case 0:
            p.move(to: CGPoint(x: 0, y: -r))
            p.addLine(to: CGPoint(x: r * 0.95, y: r * 0.7))
            p.addLine(to: CGPoint(x: -r * 0.95, y: r * 0.7))
            p.closeSubpath()
        case 1:
            p.addEllipse(in: CGRect(x: -r, y: -r, width: side, height: side))
        case 2:
            p.move(to: CGPoint(x: -r, y: -r)); p.addLine(to: CGPoint(x: r, y: r))
            p.move(to: CGPoint(x: r, y: -r)); p.addLine(to: CGPoint(x: -r, y: r))
        default:
            p.addRect(CGRect(x: -r * 0.85, y: -r * 0.85, width: side * 0.85, height: side * 0.85))
        }
        return p
    }
}

// MARK: - Pages and bubbles

private struct VitaHomePage<MenuContent: View>: View {
    let items: [VitaHomeItem]
    let space: VitaSpace
    let tap: (VitaHomeItem) -> Void
    @ViewBuilder let menu: (VitaHomeItem) -> MenuContent

    var body: some View {
        ZStack {
            ForEach(Array(items.enumerated()), id: \.element.id) { index, item in
                let slot = VitaHomeLayout.slot(index)
                // The button is exactly the bubble (and its name); positioning comes last so only the
                // bubble itself takes touches
                Button { tap(item) } label: {
                    VitaBubbleView(item: item, space: space)
                }
                .buttonStyle(VitaBubbleButtonStyle())
                .modifier(VitaItemMenu(item: item, menu: menu))
                .modifier(VitaFloat(amount: space.len(3), seed: index))
                .position(x: space.x(slot.x), y: space.y(slot.y))
            }
        }
    }
}

/// Bubbles bob gently, each at its own pace (an animation that runs by itself, not a redraw every frame:
/// per-frame redraws made taps unreliable)
private struct VitaFloat: ViewModifier {
    let amount: CGFloat
    let seed: Int
    @State private var up = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func body(content: Content) -> some View {
        content
            .offset(y: reduceMotion ? 0 : (up ? -amount : amount))
            .onAppear {
                guard !reduceMotion else { return }
                withAnimation(.easeInOut(duration: 2.0 + Double(seed % 5) * 0.35).repeatForever(autoreverses: true).delay(Double(seed % 7) * 0.2)) {
                    up = true
                }
            }
    }
}

/// Press feedback of a bubble: it squashes while the finger is on it
private struct VitaBubbleButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            // squashes under the finger, then springs back past its size when let go
            .scaleEffect(configuration.isPressed ? 0.86 : 1)
            .brightness(configuration.isPressed ? 0.06 : 0)
            .shadow(color: .white.opacity(configuration.isPressed ? 0.55 : 0), radius: configuration.isPressed ? 14 : 0)
            .animation(configuration.isPressed ? .easeOut(duration: 0.12) : .spring(response: 0.36, dampingFraction: 0.45), value: configuration.isPressed)
            .onChange(of: configuration.isPressed) { _, pressed in
                if pressed { UIImpactFeedbackGenerator(style: .soft).impactOccurred(intensity: 0.8) }
            }
    }
}

/// The long-press menu: games only (an empty menu on the system bubbles got in the way of their taps)
private struct VitaItemMenu<MenuContent: View>: ViewModifier {
    let item: VitaHomeItem
    let menu: (VitaHomeItem) -> MenuContent

    func body(content: Content) -> some View {
        if case .game = item {
            content.contextMenu { menu(item) }
        } else {
            content
        }
    }
}

struct VitaBubbleView: View {
    let item: VitaHomeItem
    let space: VitaSpace

    var body: some View {
        let d = space.len(VitaHomeLayout.bubble)
        // The name overlaps the lower edge of the bubble, as on the Vita
        VStack(spacing: -space.len(20)) {
            ZStack {
                switch item {
                case .game(let app):
                    if let icon = app.icon {
                        Image(uiImage: icon).resizable().scaledToFill()
                    } else {
                        systemIcon("gamecontroller.fill", [Color(white: 0.5), Color(white: 0.25)])
                    }
                case .search:
                    systemIcon("magnifyingglass", [Color(red: 0.98, green: 0.62, blue: 0.16), Color(red: 0.9, green: 0.38, blue: 0.05)])
                case .contentManager:
                    systemIcon("arrow.down.circle", [Color(red: 0.25, green: 0.7, blue: 0.95), Color(red: 0.05, green: 0.4, blue: 0.75)])
                case .settings:
                    systemIcon("gearshape.fill", [Color(white: 0.62), Color(white: 0.32)])
                case .themes:
                    systemIcon("paintpalette.fill", [Color(red: 0.95, green: 0.45, blue: 0.7), Color(red: 0.6, green: 0.2, blue: 0.75)])
                case .music:
                    systemIcon("music.note", [Color(red: 0.45, green: 0.85, blue: 0.55), Color(red: 0.1, green: 0.55, blue: 0.4)])
                case .classicLibrary:
                    systemIcon("square.grid.2x2.fill", [Color(red: 0.55, green: 0.6, blue: 0.95), Color(red: 0.25, green: 0.3, blue: 0.7)])
                }
                VitaGloss(d: d)
            }
            .frame(width: d, height: d)
            .clipShape(Circle())
            .overlay(Circle().strokeBorder(LinearGradient(colors: [.white.opacity(0.95), .white.opacity(0.3)], startPoint: .top, endPoint: .bottom),
                                           lineWidth: max(space.len(3), 1.5)))
            .shadow(color: .black.opacity(0.35), radius: space.len(8), y: space.len(5))

            // One line, as on the Vita: long names shrink a little, then end in "…", so they never run
            // into the bubbles of the next row or the names beside them
            Text(item.title)
                .font(VitaFont.font(space.len(19)))
                .foregroundStyle(.white)
                .shadow(color: .black.opacity(0.85), radius: 2.5, y: 1)
                .shadow(color: .black.opacity(0.5), radius: 6)
                .lineLimit(1)
                .minimumScaleFactor(0.9)
                .truncationMode(.tail)
                .multilineTextAlignment(.center)
                .frame(width: space.len(200), height: space.len(24))
        }
        .contentShape(Rectangle())
    }

    private func systemIcon(_ symbol: String, _ colors: [Color]) -> some View {
        ZStack {
            LinearGradient(colors: colors, startPoint: .top, endPoint: .bottom)
            Image(systemName: symbol)
                .font(.system(size: space.len(48), weight: .semibold))
                .foregroundStyle(.white)
                .shadow(color: .black.opacity(0.25), radius: 2, y: 1)
        }
    }
}

/// The glass of a bubble: a bright cap on top, a soft glow below and a darker rim
struct VitaGloss: View {
    let d: CGFloat

    var body: some View {
        ZStack {
            Circle().fill(RadialGradient(colors: [.clear, .clear, .black.opacity(0.2)], center: .center, startRadius: 0, endRadius: d / 2))
            Circle().fill(RadialGradient(colors: [.white.opacity(0.42), .clear], center: UnitPoint(x: 0.5, y: 1.05), startRadius: 0, endRadius: d * 0.6))
            Ellipse()
                .fill(LinearGradient(colors: [.white.opacity(0.78), .white.opacity(0.06)], startPoint: .top, endPoint: .bottom))
                .frame(width: d * 0.76, height: d * 0.44)
                .offset(y: -d * 0.29)
        }
        .allowsHitTesting(false)
    }
}

/// The arrows at the sides of the home screen that turn the page
private struct VitaPageArrows: View {
    @Binding var page: Int
    let count: Int
    let space: VitaSpace

    var body: some View {
        HStack {
            arrow("chevron.left", visible: page > 0) { withAnimation(.easeOut(duration: 0.25)) { page -= 1 } }
            Spacer(minLength: 0)
            arrow("chevron.right", visible: page < count - 1) { withAnimation(.easeOut(duration: 0.25)) { page += 1 } }
        }
        .padding(.horizontal, space.len(max(space.inset, 0) + 6))
        .frame(maxHeight: .infinity)
    }

    private func arrow(_ symbol: String, visible: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: space.len(30), weight: .bold))
                .foregroundStyle(.white.opacity(0.9))
                .shadow(color: .black.opacity(0.35), radius: 3)
                .frame(width: space.len(44), height: space.len(120))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .opacity(visible ? 1 : 0)
        .allowsHitTesting(visible)
    }
}

/// The page dots down the left side
struct VitaPageDots: View {
    let count: Int
    let current: Int
    let space: VitaSpace

    var body: some View {
        VStack(spacing: space.len(14)) {
            ForEach(0..<count, id: \.self) { index in
                Circle()
                    .fill(index == current ? Color.white : Color.white.opacity(0.35))
                    .frame(width: space.len(index == current ? 12 : 9), height: space.len(index == current ? 12 : 9))
                    .shadow(color: .black.opacity(0.3), radius: 2)
            }
        }
        .animation(.easeOut(duration: 0.2), value: current)
        .position(x: space.x(62), y: space.size.height / 2 + space.len(16))
        .opacity(count > 1 ? 1 : 0)
        .allowsHitTesting(false)
    }
}

// MARK: - Information bar

/// A system application in the information bar
struct VitaBarButton: Identifiable {
    let symbol: String
    let title: String
    let action: () -> Void
    var id: String { symbol }
}

/// Bar buttons light up and pop slightly when pressed
private struct VitaBarButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background(Capsule().fill(.white.opacity(configuration.isPressed ? 0.3 : 0)))
            .scaleEffect(configuration.isPressed ? 0.9 : 1)
            .animation(.spring(response: 0.22, dampingFraction: 0.55), value: configuration.isPressed)
            .onChange(of: configuration.isPressed) { _, pressed in
                if pressed { UIImpactFeedbackGenerator(style: .light).impactOccurred(intensity: 0.7) }
            }
    }
}

struct VitaInfoBar: View {
    let space: VitaSpace
    let theme: VitaTheme
    var notices = 0
    /// Opens the notices; nil hides the big notification bubble in the corner
    var openNotices: (() -> Void)? = nil
    /// The games played last, shown as small circles after the home icon
    var recent: [GameApp] = []
    /// System applications, as buttons after the home icon
    var buttons: [VitaBarButton] = []
    @ObservedObject private var music = MenuMusic.shared

    var body: some View {
        ZStack(alignment: .topTrailing) {
            TimelineView(.everyMinute) { context in
                HStack(spacing: space.len(12)) {
                    Image(systemName: "house.fill").font(.system(size: space.len(19), weight: .semibold))
                    ForEach(buttons) { button in
                        Button(action: button.action) {
                            Image(systemName: button.symbol)
                                .font(.system(size: space.len(20), weight: .semibold))
                                .frame(width: max(space.len(50), 40), height: max(space.len(44), 34))
                                .background(Capsule().fill(.white.opacity(0.12)))
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(VitaBarButtonStyle())
                        .accessibilityLabel(button.title)
                    }
                    ForEach(buttons.isEmpty ? recent : Array(recent.prefix(2))) { app in
                        Group {
                            if let icon = app.icon {
                                Image(uiImage: icon).resizable().scaledToFill()
                            } else {
                                Color.gray
                            }
                        }
                        .frame(width: space.len(22), height: space.len(22))
                        .clipShape(Circle())
                        .overlay(Circle().strokeBorder(.white.opacity(0.8), lineWidth: 1))
                    }
                    if !music.nowPlaying.isEmpty && (UserDefaults.standard.object(forKey: "v3k.musicEnabled") as? Bool ?? true) {
                        Label(music.nowPlaying, systemImage: "music.note")
                            .font(VitaFont.font(space.len(15)))
                            .lineLimit(1)
                            .opacity(0.8)
                            .padding(.leading, space.len(8))
                    }
                    Spacer(minLength: space.len(8))
                    Image(systemName: "wifi").font(.system(size: space.len(16), weight: .semibold))
                    clock(context.date)
                    battery
                }
                .foregroundStyle(.white)
                .padding(.leading, space.len(max(space.inset, 0) + 40))
                // Clear of the big bubble (and the dog-ear) in the corner and of the phone's rounded corner
                .padding(.trailing, space.len(max(space.inset, 0) + (openNotices != nil ? 150 : 58)))
                .frame(height: space.len(buttons.isEmpty ? 32 : 48))
                .frame(maxWidth: .infinity)
                .background(theme.barColor?.opacity(0.9) ?? Color.black.opacity(0.88))
            }
            if let openNotices {
                bubble(openNotices)
            }
        }
    }

    /// The big glossy notification bubble, half off the top of the screen
    private func bubble(_ open: @escaping () -> Void) -> some View {
        let d = space.len(76)
        return Button(action: open) {
            ZStack {
                Circle().fill(LinearGradient(colors: notices > 0
                                             ? [Color(red: 1, green: 0.78, blue: 0.3), Color(red: 0.95, green: 0.42, blue: 0.08)]
                                             : [Color(red: 0.4, green: 0.82, blue: 1), Color(red: 0.04, green: 0.42, blue: 0.86)],
                                             startPoint: .top, endPoint: .bottom))
                VitaGloss(d: d)
                if notices > 0 {
                    Text("\(notices)").font(VitaFont.font(space.len(22), bold: true)).foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.4), radius: 2)
                        .offset(y: d * 0.2)
                } else {
                    Image(systemName: "bell.fill").font(.system(size: space.len(20))).foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.35), radius: 2).offset(y: d * 0.2)
                }
            }
            .frame(width: d, height: d)
            .clipShape(Circle())
            .overlay(Circle().strokeBorder(LinearGradient(colors: [.white.opacity(0.95), .white.opacity(0.35)], startPoint: .top, endPoint: .bottom), lineWidth: 2))
            .shadow(color: .black.opacity(0.35), radius: 6, y: 3)
        }
        .buttonStyle(.plain)
        .offset(x: -space.len(max(space.inset, 0) + 70), y: -space.len(28))
        .contextMenu {
            // A way out that doesn't depend on any other control
            Button { UserDefaults.standard.set(false, forKey: "v3k.vitaHome") } label: {
                Label("Back to the normal library", systemImage: "square.grid.2x2")
            }
        }
    }

    /// The time with a small AM/PM, as on the Vita
    private func clock(_ date: Date) -> some View {
        let twelve = (DateFormatter.dateFormat(fromTemplate: "j", options: 0, locale: .current) ?? "").contains("a")
        let formatter = DateFormatter()
        formatter.dateFormat = twelve ? "h:mm" : "HH:mm"
        return HStack(alignment: .firstTextBaseline, spacing: space.len(3)) {
            Text(formatter.string(from: date)).font(VitaFont.font(space.len(22))).monospacedDigit()
            if twelve {
                Text(Calendar.current.component(.hour, from: date) < 12 ? "AM" : "PM").font(VitaFont.font(space.len(11)))
            }
        }
    }

    private var battery: some View {
        UIDevice.current.isBatteryMonitoringEnabled = true
        let raw = UIDevice.current.batteryLevel
        let level = raw < 0 ? 1 : CGFloat(raw)
        let charging = UIDevice.current.batteryState == .charging || UIDevice.current.batteryState == .full
        let tint = !charging && level < 0.15 ? Color(red: 0.95, green: 0.25, blue: 0.2) : Color(red: 0.35, green: 0.9, blue: 0.35)
        return HStack(spacing: space.len(1.5)) {
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: space.len(3)).strokeBorder(.white.opacity(0.9), lineWidth: 1.2)
                RoundedRectangle(cornerRadius: space.len(1.5)).fill(tint)
                    .frame(width: max(space.len(23) * level, 2), height: space.len(8))
                    .padding(.leading, space.len(2.5))
            }
            .frame(width: space.len(29), height: space.len(14))
            RoundedRectangle(cornerRadius: 1).fill(.white.opacity(0.9)).frame(width: space.len(2.5), height: space.len(6))
        }
    }
}

// MARK: - Start screen

/// The start screen: the picture in a rounded white frame, with the clock and date in the corner
struct VitaStartScreen: View {
    let space: VitaSpace
    let theme: VitaTheme

    private var picture: UIImage? {
        if let path = theme.startImage, let image = VitaFirmware.image(atPath: path) { return image }
        return VitaFirmware.lockScreen
    }

    private static var twelveHour: Bool {
        (DateFormatter.dateFormat(fromTemplate: "j", options: 0, locale: .current) ?? "").contains("a")
    }

    static func time(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = twelveHour ? "h:mm" : "HH:mm"
        return formatter.string(from: date)
    }

    static func period(_ date: Date) -> String? {
        twelveHour ? (Calendar.current.component(.hour, from: date) < 12 ? "AM" : "PM") : nil
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            Group {
                if let picture {
                    Image(uiImage: picture).resizable().scaledToFill()
                } else {
                    VitaDefaultBackground()
                }
            }
            .frame(width: space.size.width, height: space.size.height)
            .clipped()

            // The frame of the start screen: 32 px in from the edges below the information bar, 20 px corners
            RoundedRectangle(cornerRadius: space.len(20), style: .continuous)
                .strokeBorder(.white, lineWidth: max(space.len(2), 1))
                .frame(width: space.len(960 - 64), height: space.size.height - space.len(32) - space.len(64))
                .offset(x: space.x(32), y: space.len(32 + 32))

            TimelineView(.periodic(from: .now, by: 1)) { context in
                let rightAligned = theme.dateLayout == 2
                let top = theme.dateLayout == 1
                // Vita3K's start screen: date 186 px and clock 146 px above the bottom (468 / 426 at the top)
                let dateY: CGFloat = top ? 544 - 468 : 544 - 186
                let clockY: CGFloat = top ? 544 - 426 : 544 - 146
                ZStack(alignment: rightAligned ? .topTrailing : .topLeading) {
                    Text(context.date, format: .dateTime.weekday(.wide).month(.wide).day())
                        .font(VitaFont.font(space.len(30)))
                        .offset(x: rightAligned ? -space.len(max(space.inset, 0) + 50) : space.x(60), y: space.len(dateY))
                    HStack(alignment: .firstTextBaseline, spacing: space.len(6)) {
                        Text(Self.time(context.date))
                            .font(VitaFont.font(space.len(100)))
                            .monospacedDigit()
                        if let period = Self.period(context.date) {
                            Text(period).font(VitaFont.font(space.len(34)))
                        }
                    }
                        .offset(x: rightAligned ? -space.len(max(space.inset, 0) + 50) : space.x(80), y: space.len(clockY - 12))
                }
                .foregroundStyle(theme.dateColor)
                .shadow(color: .black.opacity(0.35), radius: 3, y: 1)
                .frame(width: space.size.width, height: space.size.height, alignment: rightAligned ? .topTrailing : .topLeading)
            }

            // A hint for anyone who has never held a Vita: in the corner away from the clock
            Label("Drag the corner, or tap it, to start", systemImage: "arrow.down.left")
                .font(VitaFont.font(space.len(16)))
                .foregroundStyle(theme.dateColor.opacity(0.85))
                .shadow(color: .black.opacity(0.4), radius: 2)
                .padding(.horizontal, space.len(max(space.inset, 0) + 60))
                .frame(width: space.size.width, alignment: theme.dateLayout == 2 ? .leading : .trailing)
                .offset(y: space.len(32 + 32 + 22))

            VitaInfoBar(space: space, theme: theme)
        }
        .frame(width: space.size.width, height: space.size.height)
        .background(Color.black)
    }
}

// MARK: - LiveArea

/// Pictures of LiveAreas, kept after the first read (a LiveArea draws many times while it animates)
enum VitaImageCache {
    private static let cache: NSCache<NSString, UIImage> = {
        let cache = NSCache<NSString, UIImage>()
        cache.totalCostLimit = 64 * 1024 * 1024
        return cache
    }()
    private static let missing = NSCache<NSString, NSNumber>()

    static func image(atPath path: String) -> UIImage? {
        let key = path as NSString
        if let image = cache.object(forKey: key) { return image }
        if missing.object(forKey: key) != nil { return nil }
        guard let image = UIImage(contentsOfFile: path) else {
            missing.setObject(1, forKey: key)
            return nil
        }
        cache.setObject(image, forKey: key, cost: Int(image.size.width * image.size.height * 4))
        return image
    }
}

/// What a game's sce_sys/livearea/contents/template.xml says
struct LiveAreaTemplate {
    struct Frame {
        let id: String
        var background: String?
        var image: String?
        var text: String?
        var textColor: Color = .white
        var textSize: CGFloat = 0
    }

    var style = "a1"
    var background: String?
    var gate: String?
    var frames: [Frame] = []

    static func load(contents: String) -> LiveAreaTemplate {
        var template = LiveAreaTemplate()
        guard let data = FileManager.default.contents(atPath: (contents as NSString).appendingPathComponent("template.xml")) else { return template }
        let parser = LiveAreaXML()
        let xml = XMLParser(data: data)
        xml.delegate = parser
        _ = xml.parse()
        template.style = parser.style ?? "a1"
        template.background = parser.background
        template.gate = parser.gate
        template.frames = parser.frames
        return template
    }

    /// Gate position (distance from the right and bottom edges) and frame rectangles (distance from the right
    /// and bottom, size) per style, from Vita3K's LiveArea
    static let styles: [String: (gate: CGPoint, frames: [String: (CGPoint, CGSize)])] = [
        "a1": (CGPoint(x: 620, y: 361), ["frame1": (CGPoint(x: 900, y: 414), CGSize(width: 260, height: 260)),
                                         "frame2": (CGPoint(x: 320, y: 414), CGSize(width: 260, height: 260)),
                                         "frame3": (CGPoint(x: 900, y: 154), CGSize(width: 840, height: 150))]),
        "a2": (CGPoint(x: 620, y: 395), ["frame1": (CGPoint(x: 900, y: 404), CGSize(width: 260, height: 400)),
                                         "frame2": (CGPoint(x: 320, y: 404), CGSize(width: 260, height: 400)),
                                         "frame3": (CGPoint(x: 640, y: 204), CGSize(width: 320, height: 200))]),
        "a3": (CGPoint(x: 620, y: 395), ["frame1": (CGPoint(x: 900, y: 414), CGSize(width: 260, height: 200)),
                                         "frame2": (CGPoint(x: 320, y: 414), CGSize(width: 260, height: 200)),
                                         "frame3": (CGPoint(x: 900, y: 214), CGSize(width: 260, height: 210)),
                                         "frame4": (CGPoint(x: 640, y: 214), CGSize(width: 320, height: 210)),
                                         "frame5": (CGPoint(x: 320, y: 214), CGSize(width: 260, height: 210))]),
        "a4": (CGPoint(x: 620, y: 395), ["frame1": (CGPoint(x: 900, y: 414), CGSize(width: 260, height: 200)),
                                         "frame2": (CGPoint(x: 320, y: 414), CGSize(width: 260, height: 200)),
                                         "frame3": (CGPoint(x: 900, y: 214), CGSize(width: 840, height: 70)),
                                         "frame4": (CGPoint(x: 900, y: 144), CGSize(width: 840, height: 70)),
                                         "frame5": (CGPoint(x: 900, y: 74), CGSize(width: 840, height: 70))]),
        "a5": (CGPoint(x: 380, y: 395), ["frame1": (CGPoint(x: 900, y: 412), CGSize(width: 480, height: 68)),
                                         "frame2": (CGPoint(x: 900, y: 344), CGSize(width: 480, height: 68)),
                                         "frame3": (CGPoint(x: 900, y: 276), CGSize(width: 480, height: 68)),
                                         "frame4": (CGPoint(x: 900, y: 208), CGSize(width: 480, height: 68)),
                                         "frame5": (CGPoint(x: 900, y: 140), CGSize(width: 480, height: 68)),
                                         "frame6": (CGPoint(x: 900, y: 72), CGSize(width: 480, height: 68)),
                                         "frame7": (CGPoint(x: 420, y: 214), CGSize(width: 360, height: 210))]),
        "psmobile": (CGPoint(x: 380, y: 345), ["frame1": (CGPoint(x: 866, y: 414), CGSize(width: 446, height: 154)),
                                               "frame2": (CGPoint(x: 866, y: 249.5), CGSize(width: 446, height: 109)),
                                               "frame3": (CGPoint(x: 866, y: 119), CGSize(width: 196, height: 58)),
                                               "frame4": (CGPoint(x: 866, y: 34), CGSize(width: 772, height: 30))]),
    ]
}

private final class LiveAreaXML: NSObject, XMLParserDelegate {
    var style: String?
    var background: String?
    var gate: String?
    var frames: [LiveAreaTemplate.Frame] = []
    private var path: [String] = []
    private var attributesStack: [[String: String]] = []
    private var text = ""
    private var backgroundIsDefault = false
    private var gateIsDefault = false

    func parser(_ parser: XMLParser, didStartElement name: String, namespaceURI: String?, qualifiedName: String?, attributes: [String: String] = [:]) {
        if name == "livearea" { style = attributes["style"] }
        if path.count == 1, let id = attributes["id"] { frames.append(LiveAreaTemplate.Frame(id: id)) }
        path.append(name)
        attributesStack.append(attributes)
        text = ""
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) { text += string }

    func parser(_ parser: XMLParser, didEndElement name: String, namespaceURI: String?, qualifiedName: String?) {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let attributes = attributesStack.last ?? [:]
        let inFrame = path.count >= 2 && !(frames.isEmpty) && path[1] != "livearea-background" && path[1] != "gate"
        if !value.isEmpty {
            if path.contains("livearea-background"), name == "image" {
                // The image marked default="on" wins; otherwise the first one
                let isDefault = attributes["default"] == "on"
                if background == nil || (isDefault && !backgroundIsDefault) {
                    background = value
                    backgroundIsDefault = isDefault
                }
            } else if path.contains("gate"), name == "startup-image" || name == "image" {
                let isDefault = attributes["default"] == "on"
                if gate == nil || (isDefault && !gateIsDefault) {
                    gate = value
                    gateIsDefault = isDefault
                }
            } else if inFrame, path.contains("liveitem"), var frame = frames.last {
                switch name {
                case "background": if frame.background == nil { frame.background = value }
                case "image": if frame.image == nil { frame.image = value }
                case "str":
                    if frame.text == nil {
                        frame.text = value
                        if let color = VitaTheme.color(hex: attributes["color"]) { frame.textColor = color }
                        frame.textSize = CGFloat(Double(attributes["size"] ?? "") ?? 0)
                    }
                default: break
                }
                frames[frames.count - 1] = frame
            }
        }
        path.removeLast()
        attributesStack.removeLast()
        text = ""
    }
}

/// A game's LiveArea: its background, frames and gate with the Start button, as the Vita lays them out.
/// Start zooms the game's picture out of the gate like the Vita does before the game starts.
struct VitaLiveArea: View {
    let app: GameApp
    let space: VitaSpace
    let theme: VitaTheme
    let start: () -> Void
    let info: () -> Void
    let settings: () -> Void

    @State private var template = LiveAreaTemplate()
    @State private var starting = false
    @State private var showingManual = false

    private var contents: String { (app.folder as NSString).appendingPathComponent("sce_sys/livearea/contents") }

    private func image(_ name: String?) -> UIImage? {
        guard let name, !name.isEmpty else { return nil }
        return VitaImageCache.image(atPath: (contents as NSString).appendingPathComponent(name))
    }

    private var manualPages: [String] { VitaManual.pages(appPath: app.folder) }

    /// The game's own picture (pic0, else the start-up picture)
    private var gameArt: UIImage? {
        let sys = (app.folder as NSString).appendingPathComponent("sce_sys")
        for name in ["pic0.png", "pic1.png", "livearea/contents/startup.png"] {
            if let picture = VitaImageCache.image(atPath: (sys as NSString).appendingPathComponent(name)) { return picture }
        }
        return nil
    }

    var body: some View {
        let style = LiveAreaTemplate.styles[template.style] ?? LiveAreaTemplate.styles["a1"]!
        let gate = CGRect(x: 960 - style.gate.x, y: 544 - style.gate.y, width: 280, height: 158)
        ZStack(alignment: .topLeading) {
            // Behind the LiveArea: the home wallpaper, darkened
            VitaWallpaper(theme: theme, page: 0)
            Color.black.opacity(0.45)

            // Background: 840 x 500 at 60, 44
            Group {
                if let bg = image(template.background) ?? image("bg0.png") ?? image("bg.png") {
                    Image(uiImage: bg).resizable()
                } else if let art = gameArt {
                    // No LiveArea of its own: the game's picture fills it (as the Vita does for homebrew)
                    Image(uiImage: art).resizable().scaledToFill()
                } else {
                    Color(red: 148 / 255, green: 164 / 255, blue: 173 / 255)
                }
            }
            .frame(width: space.len(840), height: space.len(500))
            .clipShape(RoundedRectangle(cornerRadius: space.len(6), style: .continuous))
            .shadow(color: .black.opacity(0.4), radius: space.len(8))
            .position(x: space.x(60 + 420), y: space.y(44 + 250))

            // Frames
            ForEach(template.frames, id: \.id) { frame in
                if let layout = style.frames[frame.id] {
                    let rect = CGRect(x: 960 - layout.0.x, y: 544 - layout.0.y, width: layout.1.width, height: layout.1.height)
                    ZStack {
                        if let bg = image(frame.background) {
                            Image(uiImage: bg).resizable()
                        }
                        if let picture = image(frame.image) {
                            Image(uiImage: picture).resizable().scaledToFit()
                                .frame(maxWidth: space.len(min(picture.size.width, rect.width)), maxHeight: space.len(min(picture.size.height, rect.height)))
                        }
                        if let text = frame.text {
                            Text(text)
                                .font(VitaFont.font(space.len(max(frame.textSize > 0 ? frame.textSize / 2 : 14, 10))))
                                .foregroundStyle(frame.textColor)
                                .multilineTextAlignment(.center)
                                .padding(space.len(6))
                        }
                    }
                    .frame(width: space.len(rect.width), height: space.len(rect.height))
                    .clipped()
                    .position(x: space.x(rect.midX), y: space.y(rect.midY))
                }
            }

            // Games whose LiveArea has no frames of their own get a strip with their details along the bottom
            if !template.frames.contains(where: { style.frames[$0.id] != nil && (image($0.background) != nil || image($0.image) != nil || $0.text != nil) }) {
                VitaInfoStrip(app: app, space: space)
                    .frame(width: space.len(840), alignment: .leading)
                    .position(x: space.x(60 + 420), y: space.y(544 - 62))
            }

            // The row of buttons along the top of the LiveArea
            HStack(spacing: space.len(26)) {
                if !manualPages.isEmpty {
                    VitaLiveAreaIcon(symbol: "book.fill", title: "Manual", colors: [Color(red: 0.95, green: 0.3, blue: 0.6), Color(red: 0.75, green: 0.05, blue: 0.4)], space: space) { showingManual = true }
                }
                VitaLiveAreaIcon(symbol: "info.circle.fill", title: "Information", colors: [Color(red: 0.4, green: 0.8, blue: 1), Color(red: 0.05, green: 0.45, blue: 0.85)], space: space, action: info)
                VitaLiveAreaIcon(symbol: "slider.horizontal.3", title: "Settings", colors: [Color(red: 1, green: 0.8, blue: 0.3), Color(red: 0.95, green: 0.55, blue: 0.1)], space: space, action: settings)
            }
            .position(x: space.x(gate.midX), y: space.y(44 + 46))

            // Gate with the Start button
            Button(action: pressStart) {
                ZStack(alignment: .bottom) {
                    Group {
                        if let picture = image(template.gate) ?? image("startup.png") {
                            Image(uiImage: picture).resizable()
                        } else {
                            ZStack {
                                if let art = gameArt {
                                    Image(uiImage: art).resizable().scaledToFill().overlay(Color.black.opacity(0.3))
                                } else {
                                    Color(red: 47 / 255, green: 51 / 255, blue: 50 / 255)
                                }
                                if let icon = app.icon {
                                    Image(uiImage: icon).resizable().frame(width: space.len(94), height: space.len(94))
                                        .padding(.bottom, space.len(32))
                                }
                            }
                        }
                    }
                    .frame(width: space.len(280), height: space.len(158))

                    Text("Start")
                        .font(VitaFont.font(space.len(17), bold: true))
                        .foregroundStyle(.white)
                        .frame(width: space.len(120), height: space.len(32))
                        .background(RoundedRectangle(cornerRadius: space.len(10), style: .continuous).fill(Color(red: 20 / 255, green: 168 / 255, blue: 222 / 255)))
                        .shadow(color: .black.opacity(0.3), radius: 2, y: 1)
                        .padding(.bottom, space.len(8))
                }
                .frame(width: space.len(280), height: space.len(158))
                .clipShape(RoundedRectangle(cornerRadius: space.len(10), style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: space.len(10), style: .continuous)
                    .strokeBorder(Color(white: 0.75), lineWidth: max(space.len(3), 1.5)))
                .shadow(color: .black.opacity(0.45), radius: space.len(10), y: space.len(4))
            }
            .buttonStyle(VitaPressStyle())
            .contextMenu {
                Button(action: pressStart) { Label("Start", systemImage: "play.fill") }
                Button(action: info) { Label("Information", systemImage: "info.circle") }
                Button(action: settings) { Label("Game settings, patches & saves", systemImage: "slider.horizontal.3") }
            }
            .position(x: space.x(gate.midX), y: space.y(gate.midY))

            // Options (the Vita's triangle menu): information and the game's settings
            Menu {
                Button(action: info) { Label("Information", systemImage: "info.circle") }
                Button(action: settings) { Label("Game settings, patches & saves", systemImage: "slider.horizontal.3") }
            } label: {
                Image(systemName: "triangle")
                    .font(.system(size: space.len(18), weight: .bold))
                    .foregroundStyle(Color(red: 0.4, green: 0.9, blue: 0.75))
                    .frame(width: space.len(46), height: space.len(46))
                    .background(Circle().fill(.black.opacity(0.45)))
            }
            .position(x: space.x(30), y: space.y(510))

            if starting {
                VitaStartZoom(app: app, from: gate, space: space)
            }
        }
        .frame(width: space.size.width, height: space.size.height)
        .onAppear { template = LiveAreaTemplate.load(contents: contents) }
        .fullScreenCover(isPresented: $showingManual) { VitaManual(pages: manualPages) }
    }

    private func pressStart() {
        guard !starting else { return }
        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        withAnimation(.easeOut(duration: 0.1)) { starting = true }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.75) {
            start()
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { starting = false }
        }
    }
}

/// A LiveArea's details for games without frames of their own: title, ID, version, play time, rating
private struct VitaInfoStrip: View {
    let app: GameApp
    let space: VitaSpace

    var body: some View {
        HStack(spacing: space.len(14)) {
            VStack(alignment: .leading, spacing: space.len(3)) {
                Text(app.title)
                    .font(VitaFont.font(space.len(22), bold: true))
                    .lineLimit(1)
                HStack(spacing: space.len(12)) {
                    Text("\(app.titleId) · v\(app.version)")
                    Label(app.playTimeText, systemImage: "clock")
                    if app.lastPlayed > 0 {
                        Label(Date(timeIntervalSince1970: TimeInterval(app.lastPlayed)).formatted(date: .abbreviated, time: .omitted), systemImage: "calendar")
                    }
                }
                .font(VitaFont.font(space.len(14)))
                .opacity(0.9)
                .lineLimit(1)
            }
            Spacer(minLength: 0)
            if let compatibility = app.compatibility {
                Text(compatibility.label)
                    .font(VitaFont.font(space.len(14), bold: true))
                    .padding(.horizontal, space.len(12)).padding(.vertical, space.len(4))
                    .background(Capsule().fill(compatibility.color))
            }
        }
        .foregroundStyle(.white)
        .shadow(color: .black.opacity(0.7), radius: 2, y: 1)
        .padding(.horizontal, space.len(18))
        .padding(.vertical, space.len(10))
        .background(LinearGradient(colors: [.clear, .black.opacity(0.65)], startPoint: .top, endPoint: .bottom))
    }
}

/// A glossy rounded button along the top of a LiveArea (Manual, Information, Settings)
private struct VitaLiveAreaIcon: View {
    let symbol: String
    let title: String
    let colors: [Color]
    let space: VitaSpace
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack {
                RoundedRectangle(cornerRadius: space.len(14), style: .continuous)
                    .fill(LinearGradient(colors: colors, startPoint: .top, endPoint: .bottom))
                RoundedRectangle(cornerRadius: space.len(14), style: .continuous)
                    .fill(LinearGradient(colors: [.white.opacity(0.55), .white.opacity(0.0)], startPoint: .top, endPoint: .center))
                    .padding(space.len(2))
                Image(systemName: symbol)
                    .font(.system(size: space.len(26), weight: .semibold))
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.3), radius: 2, y: 1)
            }
            .frame(width: space.len(64), height: space.len(64))
            .overlay(RoundedRectangle(cornerRadius: space.len(14), style: .continuous).strokeBorder(.white.opacity(0.9), lineWidth: 2))
            .shadow(color: .black.opacity(0.35), radius: space.len(5), y: space.len(3))
        }
        .buttonStyle(VitaPressStyle())
        .accessibilityLabel(title)
    }
}

struct VitaPressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.95 : 1)
            .brightness(configuration.isPressed ? -0.08 : 0)
            .animation(.spring(response: 0.22, dampingFraction: 0.6), value: configuration.isPressed)
    }
}

/// Start pressed: the screen darkens and the game's picture (pic0) grows out of the gate to fill the screen
private struct VitaStartZoom: View {
    let app: GameApp
    let from: CGRect
    let space: VitaSpace
    @State private var grown = false

    private var picture: UIImage? {
        UIImage(contentsOfFile: (app.folder as NSString).appendingPathComponent("sce_sys/pic0.png"))
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color.black.opacity(grown ? 0.85 : 0)
            Group {
                if let picture {
                    Image(uiImage: picture).resizable().scaledToFill()
                } else {
                    Color.black
                }
            }
            .frame(width: grown ? space.size.width : space.len(from.width), height: grown ? space.size.height : space.len(from.height))
            .clipShape(RoundedRectangle(cornerRadius: grown ? 0 : space.len(10), style: .continuous))
            .position(x: grown ? space.size.width / 2 : space.x(from.midX), y: grown ? space.size.height / 2 : space.y(from.midY))
            .opacity(grown ? 1 : 0.2)
        }
        .frame(width: space.size.width, height: space.size.height)
        .onAppear { withAnimation(.easeInOut(duration: 0.6)) { grown = true } }
        .allowsHitTesting(false)
    }
}

// MARK: - Manual

/// The game's manual (sce_sys/manual/*.png), one page at a time
struct VitaManual: View {
    let pages: [String]
    @Environment(\.dismiss) private var dismiss
    @State private var index = 0

    static func pages(appPath: String) -> [String] {
        let folder = (appPath as NSString).appendingPathComponent("sce_sys/manual")
        let fm = FileManager.default
        var files = ((try? fm.contentsOfDirectory(atPath: folder)) ?? []).filter { $0.lowercased().hasSuffix(".png") }.sorted { $0.localizedStandardCompare($1) == .orderedAscending }
        var base = folder
        if files.isEmpty, let sub = ((try? fm.contentsOfDirectory(atPath: folder)) ?? []).sorted().first(where: {
            var isDir: ObjCBool = false
            return fm.fileExists(atPath: (folder as NSString).appendingPathComponent($0), isDirectory: &isDir) && isDir.boolValue
        }) {
            base = (folder as NSString).appendingPathComponent(sub)
            files = ((try? fm.contentsOfDirectory(atPath: base)) ?? []).filter { $0.lowercased().hasSuffix(".png") }.sorted { $0.localizedStandardCompare($1) == .orderedAscending }
        }
        return files.map { (base as NSString).appendingPathComponent($0) }
    }

    var body: some View {
        ZStack(alignment: .bottom) {
            Color.black.ignoresSafeArea()
            TabView(selection: $index) {
                ForEach(Array(pages.enumerated()), id: \.offset) { i, path in
                    Group {
                        if let image = UIImage(contentsOfFile: path) {
                            Image(uiImage: image).resizable().scaledToFit()
                        }
                    }
                    .tag(i)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: .never))
            .ignoresSafeArea()
            HStack {
                Button { dismiss() } label: {
                    Image(systemName: "xmark").font(.headline).foregroundStyle(.white).padding(10).background(Circle().fill(.black.opacity(0.5)))
                }
                Spacer()
                Text("\(index + 1) / \(pages.count)").font(VitaFont.font(15)).foregroundStyle(.white)
                    .padding(.horizontal, 12).padding(.vertical, 6).background(Capsule().fill(.black.opacity(0.5)))
                Spacer()
                Color.clear.frame(width: 40, height: 40)
            }
            .padding()
        }
        .statusBarHidden(true)
    }
}

// MARK: - Search

/// Search: type a few letters and the games that match appear as bubbles
struct VitaSearch: View {
    let apps: [GameApp]
    let space: VitaSpace
    let close: () -> Void
    let open: (GameApp) -> Void
    @State private var query = ""
    @FocusState private var focused: Bool

    private var results: [GameApp] {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return [] }
        return apps.filter { $0.title.localizedCaseInsensitiveContains(q) || $0.titleId.localizedCaseInsensitiveContains(q) }
            .sorted { $0.title < $1.title }
    }

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.opacity(0.6).ignoresSafeArea().onTapGesture(perform: close)
            VStack(spacing: space.len(18)) {
                HStack(spacing: space.len(12)) {
                    Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                    TextField("Search games", text: $query)
                        .font(VitaFont.font(space.len(22)))
                        .focused($focused)
                        .submitLabel(.search)
                        .autocorrectionDisabled()
                    Button(action: close) { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
                }
                .padding(.horizontal, space.len(18))
                .frame(height: space.len(52))
                .background(Capsule().fill(Color(white: 0.95)))
                .foregroundStyle(.black)
                .frame(maxWidth: space.len(620))

                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: space.len(22)) {
                        ForEach(results) { app in
                            Button { open(app) } label: { VitaBubbleView(item: .game(app), space: space) }.buttonStyle(VitaBubbleButtonStyle())
                        }
                    }
                    .padding(.horizontal, space.len(40))
                }
                if !query.isEmpty && results.isEmpty {
                    Text("No games found").font(VitaFont.font(space.len(18))).foregroundStyle(.white.opacity(0.8))
                }
            }
            .padding(.top, space.len(44))
        }
        .onAppear { focused = true }
    }
}


// MARK: - System panels

enum VitaPanelKind: Hashable {
    case contentManager, settings, themes, music, notifications

    var title: String {
        switch self {
        case .contentManager: "Content Manager"
        case .settings: "Settings"
        case .themes: "Themes"
        case .music: "Music"
        case .notifications: "Notifications"
        }
    }

    var symbol: String {
        switch self {
        case .contentManager: "arrow.down.circle"
        case .settings: "gearshape.fill"
        case .themes: "paintpalette.fill"
        case .music: "music.note"
        case .notifications: "bell.fill"
        }
    }
}

/// A system application of the home screen, drawn like the Vita's own: a blue title bar with the application's
/// bubble, white text on a dark glass page. It is peeled away from the top-right corner like a LiveArea.
struct VitaPanel: View {
    let kind: VitaPanelKind
    let space: VitaSpace
    @ObservedObject var model: LibraryModel
    let notices: [String]
    let close: () -> Void
    let importFiles: () -> Void
    let importFolder: () -> Void
    let enterZrif: () -> Void

    @AppStorage("v3k.vitaHome.theme") private var themeId = VitaTheme.defaultId
    @AppStorage("v3k.vitaMusic") private var vitaMusic = false
    @AppStorage("v3k.musicEnabled") private var musicEnabled = true
    @AppStorage("v3k.vitaHome") private var vitaHome = true

    var body: some View {
        ZStack(alignment: .top) {
            LinearGradient(colors: [Color(red: 0.06, green: 0.12, blue: 0.24), Color(red: 0.02, green: 0.04, blue: 0.1)], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()

            VStack(spacing: 0) {
                // Title bar, below the information bar
                HStack(spacing: space.len(14)) {
                    Image(systemName: kind.symbol)
                        .font(.system(size: space.len(24), weight: .semibold))
                        .frame(width: space.len(44), height: space.len(44))
                        .background(Circle().fill(LinearGradient(colors: [Color(red: 0.3, green: 0.7, blue: 1), Color(red: 0.05, green: 0.4, blue: 0.8)], startPoint: .top, endPoint: .bottom)))
                        .overlay(Circle().strokeBorder(.white.opacity(0.85), lineWidth: 1.5))
                    Text(kind.title).font(VitaFont.font(space.len(28), bold: true))
                    Spacer()
                    Button(action: close) {
                        Image(systemName: "xmark").font(.system(size: space.len(18), weight: .bold))
                            .frame(width: space.len(44), height: space.len(44))
                            .background(Circle().fill(.white.opacity(0.14)))
                    }
                    .padding(.trailing, space.len(60 + max(space.inset, 0)))
                }
                .foregroundStyle(.white)
                .padding(.leading, space.len(40 + max(space.inset, 0)))
                .padding(.top, space.len(52))
                .padding(.bottom, space.len(12))

                Divider().overlay(.white.opacity(0.25))

                content
                    .padding(.horizontal, space.len(max(space.inset, 0)))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(width: space.size.width, height: space.size.height)
        .environment(\.colorScheme, .dark)
    }

    @ViewBuilder
    private var content: some View {
        switch kind {
        case .settings:
            // All the settings of the app, inside the panel (its own Done button is not needed here)
            SettingsView(onDone: close).environmentObject(model)
                .padding(.horizontal, space.len(max(space.inset, 0)))
        case .contentManager: contentManager
        case .themes: themes
        case .music: music
        case .notifications: notifications
        }
    }

    private func row(_ symbol: String, _ title: String, _ detail: String, tint: Color = Color(red: 0.3, green: 0.7, blue: 1), action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: space.len(16)) {
                Image(systemName: symbol).font(.system(size: space.len(24)))
                    .foregroundStyle(tint).frame(width: space.len(40))
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(VitaFont.font(space.len(22), bold: true))
                    Text(detail).font(VitaFont.font(space.len(15))).foregroundStyle(.white.opacity(0.65))
                }
                Spacer()
                Image(systemName: "chevron.right").foregroundStyle(.white.opacity(0.4))
            }
            .padding(.horizontal, space.len(20)).padding(.vertical, space.len(12))
            .background(RoundedRectangle(cornerRadius: space.len(12), style: .continuous).fill(.white.opacity(0.08)))
            .contentShape(Rectangle())
        }
        .buttonStyle(VitaPressStyle())
    }

    private var contentManager: some View {
        ScrollView {
            VStack(spacing: space.len(12)) {
                row("doc.badge.plus", "Install from files", "VPK, ZIP, PKG, PUP firmware, work.bin or .rif licenses", action: importFiles)
                row("folder.badge.plus", "Install a game folder", "A decrypted game folder or an existing Vita3K folder", action: importFolder)
                row("key.horizontal.fill", "Enter a zRIF key", "For a PKG that needs its license key", action: enterZrif)
                HStack(spacing: space.len(10)) {
                    stat("gamecontroller.fill", "\(model.apps.count)", "games")
                    stat("cpu", model.hasFirmware ? "Installed" : "Missing", "firmware")
                    stat("textformat", model.hasFonts ? "Installed" : "Missing", "fonts")
                }
                .padding(.top, space.len(6))
            }
            .padding(.horizontal, space.len(40)).padding(.vertical, space.len(18))
            .frame(maxWidth: space.len(820))
            .frame(maxWidth: .infinity)
        }
    }

    private func stat(_ symbol: String, _ value: String, _ label: String) -> some View {
        VStack(spacing: 2) {
            Image(systemName: symbol).foregroundStyle(Color(red: 0.3, green: 0.7, blue: 1))
            Text(value).font(VitaFont.font(space.len(20), bold: true))
            Text(label).font(VitaFont.font(space.len(14))).foregroundStyle(.white.opacity(0.6))
        }
        .frame(maxWidth: .infinity).padding(.vertical, space.len(12))
        .background(RoundedRectangle(cornerRadius: space.len(12), style: .continuous).fill(.white.opacity(0.06)))
        .foregroundStyle(.white)
    }

    private var themes: some View {
        let all = VitaTheme.installed()
        return ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: space.len(220)), spacing: space.len(16))], spacing: space.len(16)) {
                ForEach(all) { theme in
                    Button {
                        themeId = theme.id
                        MenuMusic.shared.settingsChanged()
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    } label: {
                        VStack(spacing: space.len(8)) {
                            ZStack {
                                if let id = theme.style, let style = VitaBackgroundStyle.all.first(where: { $0.id == id }) {
                                    VitaStyledBackground(style: style, animated: false)
                                } else if let path = theme.wallpapers.first, let image = VitaFirmware.image(atPath: path) {
                                    Image(uiImage: image).resizable().scaledToFill()
                                } else {
                                    Color.gray
                                }
                            }
                            .frame(height: space.len(110))
                            .clipShape(RoundedRectangle(cornerRadius: space.len(10), style: .continuous))
                            .overlay(RoundedRectangle(cornerRadius: space.len(10), style: .continuous)
                                .strokeBorder(themeId == theme.id ? Color(red: 0.3, green: 0.8, blue: 1) : .white.opacity(0.2), lineWidth: themeId == theme.id ? 3 : 1))
                            Text(theme.name).font(VitaFont.font(space.len(16))).foregroundStyle(.white).lineLimit(1)
                        }
                    }
                    .buttonStyle(VitaPressStyle())
                }
            }
            .padding(space.len(40))
            if all.count == 1 {
                Text("Themes you install in Hinoki (ux0/theme) show up here too.")
                    .font(VitaFont.font(space.len(15))).foregroundStyle(.white.opacity(0.6))
                    .padding(.bottom, space.len(20))
            }
        }
    }

    private var music: some View {
        VStack(spacing: space.len(14)) {
            row(musicEnabled ? "speaker.wave.2.fill" : "speaker.slash.fill", "Menu music", musicEnabled ? "On" : "Off") {
                musicEnabled.toggle()
                MenuMusic.shared.settingsChanged()
            }
            row("opticaldisc", "Use the PS Vita home music", vitaMusic ? "On: the firmware's own music" : "Off: songs made on your device") {
                vitaMusic.toggle()
                MenuMusic.shared.settingsChanged()
            }
            row("forward.fill", "Next song", "Skip to a new generated song") { MenuMusic.shared.skip() }
        }
        .padding(.horizontal, space.len(40)).padding(.vertical, space.len(18))
        .frame(maxWidth: space.len(820))
        .frame(maxHeight: .infinity, alignment: .top)
    }

    private var notifications: some View {
        ScrollView {
            VStack(spacing: space.len(10)) {
                row("square.grid.2x2.fill", "Back to the normal library", "Turn the PS Vita home screen off (Settings > Appearance turns it on again)") {
                    vitaHome = false
                }
                if notices.isEmpty {
                    Text("No new notifications").font(VitaFont.font(space.len(18))).foregroundStyle(.white.opacity(0.7))
                        .padding(.top, space.len(60))
                }
                ForEach(Array(notices.enumerated()), id: \.offset) { _, text in
                    HStack(spacing: space.len(14)) {
                        Image(systemName: "bell.fill").foregroundStyle(Color(red: 1, green: 0.75, blue: 0.3))
                        Text(text).font(VitaFont.font(space.len(17))).foregroundStyle(.white)
                        Spacer()
                    }
                    .padding(space.len(16))
                    .background(RoundedRectangle(cornerRadius: space.len(12), style: .continuous).fill(.white.opacity(0.08)))
                }
            }
            .padding(.horizontal, space.len(40)).padding(.vertical, space.len(18))
            .frame(maxWidth: space.len(820))
            .frame(maxWidth: .infinity)
        }
    }
}
