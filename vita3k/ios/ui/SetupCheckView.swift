// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit

/// One screen with everything that decides how well Hinoki runs on this phone and in this install
/// (standalone, LiveContainer, Strata), each with what to do about it, and a summary to copy for support
struct SetupCheckView: View {
    @EnvironmentObject var model: LibraryModel
    @AppStorage("v3k.ramHelpers") private var ramHelpers = false
    @State private var copied = false

    enum Level { case good, info, warning, problem }

    struct Item: Identifiable {
        var id: String { title }
        let title: String
        let value: String
        let level: Level
        let fix: String
    }

    private func gib(_ bytes: UInt64) -> String {
        String(format: "%.1f GB", Double(bytes) / 1_073_741_824)
    }

    private var installKind: String {
        switch v3k_ram_helpers_container_setup() {
        case Int32(V3K_RAM_SETUP_STANDALONE): return "Installed on its own"
        case Int32(V3K_RAM_SETUP_SEPARATE_PROCESS): return "In a container, separate process"
        default: return "In a container (LiveContainer/Strata)"
        }
    }

    private var items: [Item] {
        var list: [Item] = []

        list.append(Item(title: "Install", value: installKind, level: .info, fix: ""))

        // JIT
        if model.jitEnabled {
            list.append(Item(title: "JIT", value: "Ready", level: .good, fix: ""))
        } else if model.jitPreparing {
            list.append(Item(title: "JIT", value: "Preparing (\(model.jitProgress)/\(model.jitTarget))", level: .info, fix: "Wait a moment."))
        } else {
            list.append(Item(title: "JIT", value: model.jitCanRequest ? "Attached, not prepared yet" : "Off", level: model.jitCanRequest ? .info : .problem,
                             fix: model.jitCanRequest ? "It is prepared when you launch a game." : model.jitStatus))
        }

        // Memory limit
        let footprint = v3k_memory_footprint()
        let limit = footprint + v3k_memory_available()
        if limit >= 4 * 1024 * 1024 * 1024 {
            list.append(Item(title: "Memory limit", value: gib(limit), level: .good, fix: ""))
        } else {
            list.append(Item(title: "Memory limit", value: gib(limit), level: limit < 3 * 1024 * 1024 * 1024 ? .problem : .warning,
                             fix: "iOS gives Hinoki little memory: the Increased Memory Limit entitlement isn't active. A container (LiveContainer, Strata) or a signer that keeps it fixes this; RAM helpers also help."))
        }

        // Address space
        if v3k_extended_address_space() {
            list.append(Item(title: "Address space", value: "Large", level: .good, fix: ""))
        } else {
            list.append(Item(title: "Address space", value: "Small (\(v3k_largest_free_address_mib()) MB free)", level: .warning,
                             fix: "The Extended Virtual Addressing entitlement is missing (usual with free SideStore signing). Hinoki uses a smaller JIT pool and texture cache so it doesn't run out; games with many threads may be slower. A container (Strata, LiveContainer) has it."))
        }

        // RAM helpers
        let hint = String(cString: v3k_ram_helpers_container_hint())
        if !ramHelpers {
            list.append(Item(title: "RAM helpers", value: "Off", level: .info, fix: limit < 4 * 1024 * 1024 * 1024 ? "Turn them on in Settings > Memory: with this memory limit they help big games." : ""))
        } else if !v3k_ram_helpers_bundled() {
            list.append(Item(title: "RAM helpers", value: "Extension missing", level: .problem, fix: "Reinstall Hinoki and keep its app extensions (SideStore/AltStore: \"Keep extensions\")."))
        } else if !hint.isEmpty {
            list.append(Item(title: "RAM helpers", value: "Container setting needed", level: .problem, fix: hint))
        } else if v3k_ram_helpers_live() > 0 {
            list.append(Item(title: "RAM helpers", value: "\(v3k_ram_helpers_live()) running", level: .good, fix: ""))
        } else {
            list.append(Item(title: "RAM helpers", value: "On, start with a game", level: .info, fix: String(cString: v3k_ram_helpers_status())))
        }

        // Firmware
        if !model.hasFirmware {
            list.append(Item(title: "Firmware", value: "Missing", level: .problem, fix: "Install PSVUPDAT.PUP from Content Manager."))
        } else if !model.hasFonts {
            list.append(Item(title: "Firmware", value: "Fonts missing", level: .warning, fix: "Install the font package (PSP2UPDAT.PUP) from Content Manager: games may show no text without it."))
        } else {
            list.append(Item(title: "Firmware", value: "Installed", level: .good, fix: ""))
        }

        // Storage
        let free = model.storage.freeBytes
        if free > 0 {
            list.append(Item(title: "Free storage", value: gib(free), level: free < 2 * 1024 * 1024 * 1024 ? .warning : .good,
                             fix: free < 2 * 1024 * 1024 * 1024 ? "Shader caches and saves need room; free some space." : ""))
        }
        return list
    }

    private func symbol(_ level: Level) -> (String, Color) {
        switch level {
        case .good: return ("checkmark.circle.fill", .green)
        case .info: return ("info.circle.fill", .blue)
        case .warning: return ("exclamationmark.triangle.fill", .orange)
        case .problem: return ("xmark.octagon.fill", .red)
        }
    }

    private var summary: String {
        var lines = ["Hinoki \(String(cString: v3k_version()))", "\(UIDevice.current.model), iOS \(UIDevice.current.systemVersion)"]
        for item in items {
            lines.append("\(item.title): \(item.value)" + (item.fix.isEmpty ? "" : " — \(item.fix)"))
        }
        return lines.joined(separator: "\n")
    }

    var body: some View {
        Form {
            Section {
                ForEach(items) { item in
                    let (icon, color) = symbol(item.level)
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Image(systemName: icon).foregroundStyle(color)
                            Text(item.title)
                            Spacer()
                            Text(item.value).foregroundStyle(.secondary).multilineTextAlignment(.trailing)
                        }
                        if !item.fix.isEmpty {
                            Text(item.fix)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(.vertical, 2)
                }
            } footer: {
                Text("Checked now. Memory and JIT change while Hinoki runs; open this again after launching a game for the latest.")
            }
            Section {
                Button {
                    UIPasteboard.general.string = summary
                    copied = true
                } label: {
                    Label(copied ? "Copied" : "Copy summary", systemImage: copied ? "checkmark" : "doc.on.doc")
                }
            } footer: {
                Text("Paste it into a bug report or the Discord when asking for help.")
            }
        }
        .navigationTitle("Setup check")
        .onAppear { model.reload() }
    }
}
