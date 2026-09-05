cmake_minimum_required(VERSION 3.16)

project("ReconOS"
    VERSION 0.3.0
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
    src/recon_apps.c
    src/recon_avatar.c
    src/recon_modules.c
    src/recon_capture.c
    src/recon_net.c
    src/recon_png.c
    src/recon_fonts.c
    src/recon_wallpaper.c
    src/recon_appwin.c
    src/recon_cmd.c
    src/recon_clock.c
    src/recon_control.c
    src/recon_display.c
    src/recon_tls.c
    src/recon_control_panel.c
    src/recon_decor.c
    src/recon_desktop.c
    src/recon_error.c
    src/recon_explorer.c
    src/recon_firewall.c
    src/recon_fs.c
    src/recon_ico.c
    src/recon_icon_gen.c
    src/recon_icons.c
    src/recon_filedlg.c
    src/recon_notepad.c
    src/recon_registry.c
    src/recon_crypt.c
    src/recon_users.c
    src/recon_service.c
    src/recon_session.c
    src/recon_access.c
    src/recon_help.c
    src/recon_package.c
    src/recon_clip.c
    src/recon_props.c
    src/recon_theme.c
    src/recon_procinfo.c
    src/recon_shell.c
    src/recon_taskmgr.c
    src/recon_terminal.c
    src/recon_ui.c
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h")

# The version, from the project declaration above, so the running system and
# the packaged tarball cannot disagree about what they are. It was written out
# by hand in five places and had not been touched in some time.
target_compile_definitions(ReconOS PRIVATE
    RECONOS_VERSION="${PROJECT_VERSION}")

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

# Modules are loaded with dlopen and call back into ReconOS directly, so the
# executable must export its own symbols rather than hiding them. Without this
# a module links but every ReconOS function it calls is undefined at load.
set_target_properties(ReconOS PROPERTIES ENABLE_EXPORTS ON)

# zlib for writing PNG. Already present as a wlroots dependency; named here
# because ReconOS calls it directly now and a transitive dependency is not a
# promise.
pkg_check_modules(ZLIB REQUIRED zlib)

# mbedTLS, for the network port's encryption. Found by name rather than by
# pkg-config because 2.x does not ship a .pc file on the distributions this
# builds on -- which is worth saying, because the obvious first assumption on
# seeing find_library here is that somebody did not know about pkg_check_modules.
#
# Three libraries, and all three are needed: mbedtls is the protocol,
# mbedx509 is the certificate parsing, mbedcrypto is the primitives.
find_library(MBEDTLS_LIB mbedtls REQUIRED)
find_library(MBEDX509_LIB mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)

target_link_libraries(ReconOS PRIVATE
    ${WLR_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKB_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${MBEDTLS_LIB}
    ${MBEDX509_LIB}
    ${MBEDCRYPTO_LIB}
    ${CMAKE_DL_LIBS}
    m)

# Where built modules are collected, so a fresh system can install them into
# its own filesystem on first run rather than starting with no applications.
target_compile_definitions(ReconOS PRIVATE
    RECONOS_MODULE_DIR="${CMAKE_BINARY_DIR}/modules")

add_subdirectory(modules)

# --- Tests ---
#
# The filesystem operations are tested on their own because they are the ones
# that lose a user's work when they are wrong, and clicking through a desktop
# is a poor way to find that out. recon_fs.c has no dependency on wlroots, so
# the test binary needs nothing but the standard library.

add_executable(recon_fs_tests
    tests/test_fs.c
    src/recon_fs.c)

target_include_directories(recon_fs_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The registry's failures are quiet -- a value that reads back wrong makes the
# desktop behave slightly oddly forever and nobody connects the two -- so it is
# tested the same way, against a throwaway root.
add_executable(recon_registry_tests
    tests/test_registry.c
    src/recon_registry.c
    src/recon_fs.c)

target_include_directories(recon_registry_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# A skin's failures are silent: a role no skin answers draws as nothing and
# looks like a missing feature rather than a missing colour.
add_executable(recon_theme_tests
    tests/test_theme.c
    src/recon_theme.c
    src/recon_registry.c
    src/recon_fs.c
    # Installing a skin writes into /System, so recon_theme asks who is
    # signed in before it tries. Accounts and password hashing come along
    # with that -- neither needs a compositor, which is the property these
    # test targets are actually protecting.
    src/recon_users.c
    src/recon_crypt.c)

target_include_directories(recon_theme_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The font list's failures are rules about what must not happen: a font that
# ships must not be removable, and neither must the one being drawn with.
# Both are a couple of ifs, and neither failure is visible until somebody's
# desktop has no typeface left.
add_executable(recon_fonts_tests
    tests/test_fonts.c
    src/recon_fonts.c
    src/recon_registry.c
    src/recon_fs.c)

target_include_directories(recon_fonts_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The encryption's failures are invisible from a running system: a certificate
# remade on every start still works, and breaks every client that pinned the
# old one. A world-readable private key works perfectly too.
add_executable(recon_tls_tests
    tests/test_tls.c
    src/recon_tls.c
    src/recon_crypt.c
    src/recon_fs.c)

target_include_directories(recon_tls_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_tls_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

# Calendar arithmetic is wrong in 2100 or it is not, and nothing but a test
# finds out which for seventy years. The twelve-hour clock's two special cases
# are wrong twice a day and nobody notices either.
add_executable(recon_clock_tests
    tests/test_clock.c
    src/recon_clock.c
    # The clock asks recon_net whether there is a way out before it offers to
    # check the time, and recon_net asks the firewall before it opens
    # anything -- so both come along, the same way the network tests take them.
    src/recon_net.c
    src/recon_firewall.c
    src/recon_error.c
    src/recon_registry.c
    src/recon_fs.c)

target_include_directories(recon_clock_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_clock_tests PRIVATE ${WAYLAND_LIBRARIES})

# Whether the accessibility skins work is a question about people who are not
# in the room. It is answered by simulating each deficiency and measuring, not
# by looking at the palette and deciding it seems fine.
add_executable(recon_access_tests
    tests/test_access.c
    src/recon_theme.c
    src/recon_registry.c
    src/recon_fs.c
    src/recon_users.c
    src/recon_crypt.c)

target_include_directories(recon_access_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_access_tests PRIVATE m)

# Hashing is checked against published vectors rather than against itself. A
# hash that is merely self-consistent is wrong in a way nobody notices, and
# passwords would still appear to work.
add_executable(recon_crypt_tests
    tests/test_crypt.c
    src/recon_crypt.c)

target_include_directories(recon_crypt_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Accounts, and what a limited one may do. The refusals are the part worth
# proving: letting people in is easy, keeping the right people out is not.
add_executable(recon_users_tests
    tests/test_users.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_fs.c)

target_include_directories(recon_users_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Networking reports rather than computes, which is exactly the kind of code
# that goes wrong quietly: a wrong answer looks like a true fact about a
# different machine. These check the parts that are true of any machine --
# loopback exists, an interface is listed once -- rather than of this one.
add_executable(recon_net_tests
    tests/test_net.c
    src/recon_net.c
    src/recon_registry.c
    # Opening a connection asks the firewall, and a refusal is an error with
    # a code. Neither needs a compositor, which is the property these test
    # targets exist to protect.
    src/recon_firewall.c
    src/recon_error.c
    src/recon_fs.c)

target_include_directories(recon_net_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Only wayland-server, because that is genuinely all recon_net needs: an
# event loop to hang a socket on. No wlroots, no generated protocol headers,
# no compositor.
target_link_libraries(recon_net_tests PRIVATE ${WAYLAND_LIBRARIES})
target_include_directories(recon_net_tests PRIVATE ${WAYLAND_INCLUDE_DIRS})

enable_testing()
add_test(NAME filesystem COMMAND recon_fs_tests)
add_test(NAME registry COMMAND recon_registry_tests)
add_test(NAME theme COMMAND recon_theme_tests)
add_test(NAME fonts COMMAND recon_fonts_tests)
add_test(NAME tls COMMAND recon_tls_tests)
add_test(NAME clock COMMAND recon_clock_tests)
add_test(NAME accessibility COMMAND recon_access_tests)
add_test(NAME hashing COMMAND recon_crypt_tests)
add_test(NAME accounts COMMAND recon_users_tests)
add_test(NAME network COMMAND recon_net_tests)

# --- A Wayland client, for testing what ReconOS does with somebody else's
#     window ---
#
# ReconOS has hosted client windows since v0.1.0 and there has never been a
# client written against it, so everything on that path was reasoned about and
# nothing was watched. Server-side decorations made that untenable: the whole
# feature is "the client stops drawing its title bar and we draw it instead",
# and the clients to hand -- weston's own demos -- do not use the protocol and
# draw their frames regardless.
#
# Not registered with ctest: it needs a compositor to connect to, which is the
# thing under test rather than something the test can arrange.

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

set(XDG_DECORATION_XML
    "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")

foreach(SPEC "xdg-shell:${XDG_SHELL_XML}" "xdg-decoration:${XDG_DECORATION_XML}")
    string(REPLACE ":" ";" PARTS "${SPEC}")
    list(GET PARTS 0 PROTO_NAME)
    list(GET PARTS 1 PROTO_XML)

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER_EXEC}" client-header
                "${PROTO_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-client-protocol.h"
        DEPENDS "${PROTO_XML}"
        COMMENT "Generating ${PROTO_NAME} client header"
        VERBATIM)

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-client-protocol.c"
        COMMAND "${WAYLAND_SCANNER_EXEC}" private-code
                "${PROTO_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-client-protocol.c"
        DEPENDS "${PROTO_XML}"
        COMMENT "Generating ${PROTO_NAME} client code"
        VERBATIM)
endforeach()

add_executable(decor_client
    tests/decor_client.c
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h")

target_include_directories(decor_client PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${WAYLAND_CLIENT_INCLUDE_DIRS})
target_link_libraries(decor_client PRIVATE ${WAYLAND_CLIENT_LIBRARIES} rt)
