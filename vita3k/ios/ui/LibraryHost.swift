// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit

/// Owns the window that shows the SwiftUI library. SDL creates its own window for the game, so the
/// library window is hidden while a game runs and shown again when the game exits.
@MainActor
enum LibraryHost {
    static var window: UIWindow?
    static let model = LibraryModel()

    /// RAM helpers on, but the container (LiveContainer/Strata) won't start them: say what to change,
    /// once per kind of problem (it is also in Settings > Memory and the PS Vita home notices)
    private static func showContainerHelperHintOnce() {
        guard UserDefaults.standard.bool(forKey: "v3k.ramHelpers") else { return }
        let hint = String(cString: v3k_ram_helpers_container_hint())
        guard !hint.isEmpty else { return }
        let key = "v3k.ramHelpers.containerHintShown.\(v3k_ram_helpers_container_setup())"
        guard !UserDefaults.standard.bool(forKey: key) else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
            guard let presenter = window?.rootViewController, presenter.presentedViewController == nil else { return }
            UserDefaults.standard.set(true, forKey: key)
            let alert = UIAlertController(title: "RAM helpers need a container setting", message: hint, preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            presenter.present(alert, animated: true)
        }
    }

    static func show() {
        BackgroundMotion.shared.setActive(true)
        if window == nil {
            let newWindow: UIWindow
            if let scene = PhoneScene.current {
                newWindow = UIWindow(windowScene: scene)
            } else {
                newWindow = UIWindow(frame: UIScreen.main.bounds)
            }
            let controller = LibraryHostingController(rootView: AnyView(LibraryView().environmentObject(model)))
            controller.view.backgroundColor = .black
            newWindow.rootViewController = controller
            newWindow.overrideUserInterfaceStyle = .dark
            window = newWindow
        }
        model.reload()
        updateQuickActions()
        // How the last game ended, when there is something to say (e.g. the GPU kept failing)
        if let message = takeString(v3k_take_session_message()) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { model.lastMessage = message }
        }
        let firstShow = !hasShown
        hasShown = true
        if firstShow && !UserDefaults.standard.bool(forKey: "v3k.surfaceSyncOffByDefault") {
            // Surface sync went back to off by default (on for everything, it crashed Madden NFL 13
            // at boot): undo the build that switched it on. Per game it can still be turned on.
            UserDefaults.standard.set(true, forKey: "v3k.surfaceSyncOffByDefault")
            model.setBool("surface-sync", false)
            model.reload()
        }
        if firstShow {
            // After JIT setup (StikDebug pauses the app while it works)
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { model.updateOnlineLists() }
            NotificationCenter.default.addObserver(forName: Notification.Name("V3KLaunchRequest"), object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { takeLaunchRequest() }
            }
            observeReturnForJIT()
        }
        // A launch link or quick action may have opened the app; give JIT setup a moment to begin
        DispatchQueue.main.asyncAfter(deadline: .now() + (firstShow ? 0.6 : 0.3)) { takeLaunchRequest() }
        window?.alpha = 0
        window?.isHidden = false
        updateOrientation()
        window?.makeKeyAndVisible()
        UIView.animate(withDuration: firstShow ? 0.35 : 0.2, delay: 0, options: [.curveEaseOut]) {
            window?.alpha = 1
        }
        if firstShow { showContainerHelperHintOnce() }
        if firstShow && v3k_jit_can_request() {
            // Prepare JIT right away (StikDebug briefly pauses the app while it works), and start the
            // menu music only once that's done, so the music doesn't stutter as it begins
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                requestJIT()
            }
        } else {
            MenuMusic.shared.libraryShown()
            // StikDebug opens Vita3K first and attaches its script a few seconds later (it shows
            // "Connecting..." meanwhile): the one check above came too early and JIT stayed off for
            // the whole session. Keep looking for the debugger for as long as Vita3K is open: a debugger
            // can attach at any time (StikDebug, LiveContainer, a late "Connecting...")
            if firstShow { v3k_log_jit_state("library shown") }
            if firstShow && !v3k_is_jit_enabled() {
                watchForJITScript(until: .distantFuture)
                // Built-in JIT with an imported pairing file: no JIT app, no tap
                BuiltinJIT.shared.enableAtLaunchIfPossible()
            }
        }
    }

    private static func requestJIT() {
        v3k_jit_request()
        model.reloadJIT()
        startMusicWhenJITIsReady()
    }

    private static var watchingForJIT = false

    /// Prepares JIT as soon as a debugger attaches, checking twice a second until the deadline (and
    /// again whenever Vita3K comes back on screen, see init of the observer below)
    static func watchForJITScript(until deadline: Date) {
        guard !watchingForJIT else { return }
        watchingForJIT = true
        func check() {
            if v3k_is_jit_enabled() || v3k_jit_preparing() {
                watchingForJIT = false
                model.reloadJIT()
                return
            }
            if v3k_jit_can_request() {
                watchingForJIT = false
                v3k_log_ui("JIT: debugger attached after launch, preparing JIT now")
                // Not during a game: the library is where the pause is expected
                if window?.isHidden == false {
                    MenuMusic.shared.libraryHidden()
                    requestJIT()
                }
                return
            }
            if Date() > deadline {
                watchingForJIT = false
                return
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { check() }
        }
        check()
    }

    /// Coming back from StikDebug (or the app switcher) with JIT still off: look for the debugger again
    static func observeReturnForJIT() {
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated {
                if hasShown && !v3k_is_jit_enabled() && window?.isHidden == false {
                    watchForJITScript(until: .distantFuture)
                }
            }
        }
    }

    private static var hasShown = false
    private static var externalLaunch: String?

    /// vita3k://launch/<ID> links and Home Screen quick actions
    static func takeLaunchRequest() {
        // Files opened with "Open in Vita3K" are installed once the library is showing
        if window?.isHidden == false {
            var urls: [URL] = []
            while let path = takeString(v3k_take_import_request()) {
                urls.append(URL(fileURLWithPath: path))
            }
            if !urls.isEmpty { model.enqueue(urls) }
        }
        if let titleId = takeString(v3k_take_launch_request()) {
            externalLaunch = titleId
        }
        // vita3k://launch/library switches the PS Vita home screen off (a way out that needs no taps in the app)
        if let request = externalLaunch, ["library", "classic"].contains(request.lowercased()) {
            externalLaunch = nil
            UserDefaults.standard.set(false, forKey: "v3k.vitaHome")
            updateOrientation()
            return
        }
        guard let titleId = externalLaunch else { return }
        // Only from the library: a running game isn't interrupted
        guard window?.isHidden == false else {
            externalLaunch = nil
            return
        }
        if v3k_jit_preparing() || model.settingUpJIT {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { takeLaunchRequest() }
            return
        }
        externalLaunch = nil
        model.reload()
        if let app = model.apps.first(where: { $0.titleId.caseInsensitiveCompare(titleId) == .orderedSame }) {
            model.launchRequested = app
        } else {
            model.lastMessage = "\(titleId) isn't installed in Hinoki."
        }
    }

    /// Long-pressing the app icon lists the last games played
    static func updateQuickActions() {
        let recent = model.apps.filter { $0.lastPlayed > 0 }.sorted { $0.lastPlayed > $1.lastPlayed }.prefix(4)
        UIApplication.shared.shortcutItems = recent.map { app in
            UIApplicationShortcutItem(type: "org.vita3k.ios.launch", localizedTitle: app.title, localizedSubtitle: app.titleId,
                                      icon: UIApplicationShortcutIcon(systemImageName: "play.fill"),
                                      userInfo: ["titleId": app.titleId as NSString])
        }
    }

    private static func startMusicWhenJITIsReady() {
        if v3k_jit_preparing() {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { startMusicWhenJITIsReady() }
        } else if window?.isHidden == false {
            MenuMusic.shared.libraryShown()
        }
    }

    /// The PS Vita home screen is landscape only, like the Vita; the normal library turns freely
    static var wantsLandscape: Bool { UserDefaults.standard.bool(forKey: "v3k.vitaHome") }

    static func updateOrientation() {
        guard let window, let controller = window.rootViewController else { return }
        controller.setNeedsUpdateOfSupportedInterfaceOrientations()
        controller.setNeedsStatusBarAppearanceUpdate()
        controller.setNeedsUpdateOfHomeIndicatorAutoHidden()
        controller.setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
        if let scene = window.windowScene {
            let mask: UIInterfaceOrientationMask = wantsLandscape ? .landscape : .allButUpsideDown
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { error in
                v3k_log_ui("Library orientation: \(error.localizedDescription)")
            }
        }
    }

    static func hide() {
        BackgroundMotion.shared.setActive(false)
        // The game is about to start: the menu music fades out and stops
        MenuMusic.shared.libraryHidden()
        window?.isHidden = true
    }
}

/// Hosts the library; turns to landscape for the PS Vita home screen
final class LibraryHostingController: UIHostingController<AnyView> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        UserDefaults.standard.bool(forKey: "v3k.vitaHome") ? .landscape : .allButUpsideDown
    }

    override var prefersStatusBarHidden: Bool { UserDefaults.standard.bool(forKey: "v3k.vitaHome") }
    override var prefersHomeIndicatorAutoHidden: Bool { UserDefaults.standard.bool(forKey: "v3k.vitaHome") }
    // The PS Vita home screen folds its pages from the top-right corner, where Control Center is pulled
    // down: the first swipe there folds the page, a second one opens Control Center (as in games)
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge {
        UserDefaults.standard.bool(forKey: "v3k.vitaHome") ? .top : []
    }
}

// Called from C++ (ios_platform.mm) on the main thread
@_cdecl("v3k_ui_show")
public func v3kUIShow() {
    MainActor.assumeIsolated { LibraryHost.show() }
}

@_cdecl("v3k_ui_hide")
public func v3kUIHide() {
    MainActor.assumeIsolated { LibraryHost.hide() }
}
