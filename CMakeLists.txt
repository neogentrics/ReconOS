cmake_minimum_required(VERSION 3.16)

project("ReconOS"
    VERSION 0.7.0
    DESCRIPTION "A Wayland compositor and desktop shell built from scratch"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# GNU extensions are required by the system headers wlroots pulls in.
set(CMAKE_C_EXTENSIONS ON)

# wlroots is pre-1.0 and requires callers to acknowledge its unstable API.
add_compile_definitions(WLR_USE_UNSTABLE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLR REQUIRED wlroots)
pkg_check_modules(WAYLAND REQUIRED wayland-server)
pkg_check_modules(XKB REQUIRED xkbcommon)
# Used directly for damage region tests; wlroots exposes pixman in its API.
pkg_check_modules(PIXMAN REQUIRED pixman-1)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner)

# --- Wayland protocol code generation ---
# xdg-shell is the protocol clients use to create ordinary application windows.
# wayland-scanner turns its XML description into C we compile in directly.

pkg_get_variable(WAYLAND_SCANNER_EXEC wayland-scanner wayland_scanner)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
    COMMAND "${WAYLAND_SCANNER_EXEC}" server-header
            "${XDG_SHELL_XML}" "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
    DEPENDS "${XDG_SHELL_XML}"
    COMMENT "Generating xdg-shell server header"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    COMMAND "${WAYLAND_SCANNER_EXEC}" private-code
            "${XDG_SHELL_XML}" "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    DEPENDS "${XDG_SHELL_XML}"
    COMMENT "Generating xdg-shell protocol code"
    VERBATIM)

# --- Executable ---

add_executable(ReconOS
    src/main.c
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h")

# Assets are looked up here at runtime; RECONOS_ASSETS overrides it.
target_compile_definitions(ReconOS PRIVATE
    RECONOS_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

target_include_directories(ReconOS PRIVATE
    ${WLR_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(ReconOS PRIVATE
    ${WLR_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKB_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    m)
