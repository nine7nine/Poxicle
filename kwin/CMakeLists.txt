cmake_minimum_required(VERSION 3.16)
project(poxicle-kwin LANGUAGES C CXX)

# Wayland-only KWin (Plasma 6) binary OpenGL effect that draws poxicle edge
# particles per window. Targets the installed KWin (6.6.5+); the effect API has
# no binary compatibility, so this must be rebuilt whenever KWin bumps it.

# Qt5 also exists on this system; pin the versionless Qt CMake machinery to 6 so
# KWin's transitive Qt6Qml config doesn't trip qt_generate_foreign_qml_types().
set(QT_DEFAULT_MAJOR_VERSION 6)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# KWin::kwin's CMake config pulls in Qt Quick/Qml transitively, so a consumer
# must prime those components even though the effect itself only uses Core/Gui.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus OpenGL Qml Quick)
find_package(KF6 REQUIRED COMPONENTS CoreAddons Config KCMUtils I18n)

# Use ECM's Findepoxy (the SAME module KWin's find_dependency(epoxy) uses) to get
# the epoxy::epoxy target — for the effect's own GL state save/restore. Do NOT use
# pkg_check_modules with the prefix "epoxy": it shares the epoxy_* variable
# namespace with ECM's module and made KWin's transitive find flaky on reconfigure
# ("No rule to make target 'epoxy'"). glesv2 (poxicle-gl.c's direct GLES3 calls)
# uses a private prefix to avoid the same trap.
find_package(epoxy REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_GLESV2 REQUIRED IMPORTED_TARGET glesv2)

find_package(KWin REQUIRED)

# kcoreaddons_add_plugin needs the output dir set; mirror what the plugin loader
# and the README's QT_PLUGIN_PATH dev loop expect (build/bin/...).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

kcoreaddons_add_plugin(poxicle_kwin
    INSTALL_NAMESPACE "kwin/effects/plugins"
    SOURCES
        src/poxicleeffect.cpp
        src/poxconfig.cpp
        # poxicle's reusable simulation core + GLES renderer, compiled straight in
        # from the engine in this same repo (../src) — no poxicle-wl, no Wayland/EGL
        # backend, since KWin owns the surface + GL context.
        ../src/poxicle.c
        ../src/poxicle-gl.c)

target_include_directories(poxicle_kwin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(poxicle_kwin PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus              # org.ninez.PoxicleBridge external-source channel
    KF6::CoreAddons
    KF6::ConfigCore
    KWin::kwin
    epoxy::epoxy
    PkgConfig::PC_GLESV2)

# The "Configure…" KCM — a binary KCModule loaded from kwin/effects/configs/,
# declared by the effect's metadata X-KDE-ConfigModule. Reads/writes the same
# kwinrc [Effect-poxicle_kwin] format the effect consumes.
kcoreaddons_add_plugin(kwin_poxicle_config
    INSTALL_NAMESPACE "kwin/effects/configs"
    SOURCES src/poxiclekcm.cpp)

target_link_libraries(kwin_poxicle_config PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::KCMUtils
    KF6::I18n)

# The standalone GTK4 configurator now lives at the top level (../configurator)
# and builds with meson (`-Dconfigurator=true`) — it's toolkit-only, independent of
# this Qt/KWin effect, and a future GNOME integration will share it.

# ── External-source test producer (poxbridge-test) ──
# Stands in for a streaming client (Chiguiro) so the effect's external-source path
# can be exercised in a nested KWin without building Chiguiro: maps a memfd,
# animates a ring of PoxInstance through the poxbridge seqlock, and Registers it
# against a target window's pid. Dev/test tool — built, not installed.
add_executable(poxbridge-test poxio-test/poxbridge-test.cpp)
target_include_directories(poxbridge-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(poxbridge-test PRIVATE Qt6::Core Qt6::DBus)
