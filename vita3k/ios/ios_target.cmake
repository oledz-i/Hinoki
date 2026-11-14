# Vita3KiOS: creates the iOS app target. Included from vita3k/CMakeLists.txt when IOS is set,
# in place of the desktop executable and the Android shared library.

enable_language(Swift)

set(VITA3K_IOS_SWIFT_SOURCES
	ios/ui/LibraryHost.swift
	ios/ui/JITEnabler.swift
	ios/ui/HinokiBrand.swift
	ios/ui/LibraryModel.swift
	ios/ui/LibraryView.swift
	ios/ui/SettingsView.swift
	ios/ui/SetupCheckView.swift
	ios/ui/TouchControls.swift
	ios/ui/MenuMusic.swift
	ios/ui/GameSettingsView.swift
	ios/ui/PerformanceHUD.swift
	ios/ui/AnimatedBackground.swift
	ios/ui/VitaHomeView.swift
	ios/ui/VitaHomeScreen.swift
)

add_executable(vita3k MACOSX_BUNDLE
	ios/main_ios.cpp
	ios/ios_platform.mm
	ios/ios_platform.h
	ios/ios_session.cpp
	ios/crash_report.cpp
	ios/crash_report.h
	ios/cheats.cpp
	ios/cheats.h
	ios/game_extras.cpp
	ios/game_extras.h
	ios/ios_session.h
	ios/controls/virtual_pad.cpp
	ios/controls/virtual_pad.h
	ios/jit/ios_jit.cpp
	ios/jit/ios_jit.h
	ios/ramhelper/ram_helpers.mm
	ios/ramhelper/builtin_jit.mm
	ios/ramhelper/builtin_jit.h
	ios/ramhelper/log.cpp
	ios/ramhelper/ram_helpers.h
	ios/ramhelper/protocol.h
	ios/bridge/vita3k_bridge.cpp
	ios/bridge/vita3k_bridge.h
	${VITA3K_IOS_SWIFT_SOURCES}
	interface.cpp
	performance.cpp
)

target_include_directories(vita3k PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vita3k PRIVATE
	net
	SDL3::SDL3
	pugixml::pugixml
	moltenvk_static
	codec
	"-framework Foundation"
	"-framework UIKit"
	"-framework SwiftUI"
	"-framework UniformTypeIdentifiers"
	"-framework GameController"
	"-framework AVFoundation"
	"-framework CoreMotion"
	"-framework Photos"
	"-framework Security"
)

# Objective-C++ sources use ARC
set_source_files_properties(ios/ios_platform.mm ios/ramhelper/ram_helpers.mm ios/ramhelper/builtin_jit.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

set_target_properties(vita3k PROPERTIES
	Swift_LANGUAGE_VERSION 5
	XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
	XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/ios/ui/Vita3K-Bridging-Header.h"
	XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.4"
)

