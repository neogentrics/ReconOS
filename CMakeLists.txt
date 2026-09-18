cmake_minimum_required(VERSION 3.16)

project("ReconOS"
    VERSION 0.4.74
    DESCRIPTION "A Wayland compositor and desktop shell built from scratch"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# GNU extensions are required by the system headers wlroots pulls in.
set(CMAKE_C_EXTENSIONS ON)

# wlroots is pre-1.0 and requires callers to acknowledge its unstable API.
add_compile_definitions(WLR_USE_UNSTABLE)

#
# --- Warnings ---
#
# There were none of these for the whole of v0.1 through v0.4, which means the
# build had been running at the compiler's default level -- and the first time
# they were switched on, four real things fell out of it in a few minutes:
#
#   * two mail header chains written as `a() || b() || c();`, an expression
#     whose value is discarded, which is exactly the shape a real mistake takes
#   * a click compared across two different enums that happen to share their
#     values today and are under no obligation to tomorrow
#   * two switches over an enum that had quietly stopped covering it, so they
#     had stopped being able to warn about the next value added
#   * six functions nothing called, one of them the old Alt+Tab left behind
#     after it was replaced
#
# None of those was going to be found by reading. -Wunused-parameter is left
# off deliberately: a callback signature is fixed by whoever calls it, so a
# handler that ignores an argument is the normal case and not a smell, and
# twenty-two of those would bury the next real one.
#
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Two warnings promoted to errors, and only two.
#
# A call with no prototype in front of it is not a style question. C assumes it
# returns `int`, so a function returning a float is read as an integer and the
# caller carries on with a number that was never computed -- which is exactly
# what happened to `recon_sqrtf` in `test_libc_math.c`: the suite reported
# `sqrtf(0)` as 1 and the fault was in the declaration, not the square root.
#
# The compiler had said so both times. Nobody was listening, because this build
# has no `-Werror` and a warning is one line among a few thousand. Rather than
# turn every warning into an error -- a policy change across a tree two
# sessions are working in -- the two that can only ever mean a wrong answer are
# errors and the rest stay warnings.
#
# They cost nothing today: the whole tree compiles with no instance of either.
add_compile_options(-Werror=implicit-function-declaration -Werror=implicit-int)

#
# Configure the suites without the compositor.
#
# Every test target here was written to need no window -- that is the property
# they were designed around and it is stated all over this file. It was not
# *true of the build*, though: `pkg_check_modules(WLR REQUIRED wlroots)` ran
# before anything, so configuring at all needed wlroots 0.17.1, and a machine
# without it could not run a single suite.
#
# That is what kept the sweeps off any machine but this one. With this option
# the twenty-five suites build against stock libraries, which is what makes
# running them automatically possible.
#
#   cmake -B build -DRECONOS_TESTS_ONLY=ON
#
option(RECONOS_TESTS_ONLY
    "Build only the test suites, with no compositor and no wlroots" OFF)

find_package(PkgConfig REQUIRED)

# Needed by the suites as well as the compositor, so outside the guard below.
pkg_check_modules(WAYLAND REQUIRED wayland-server)
pkg_check_modules(XKB REQUIRED xkbcommon)
# Used directly for damage region tests; wlroots exposes pixman in its API.
pkg_check_modules(PIXMAN REQUIRED pixman-1)

# zlib for writing PNG, and mbedTLS for the network port's encryption. Both
# are needed by suites of their own, which is why they are found here rather
# than beside the compositor that also uses them.
pkg_check_modules(ZLIB REQUIRED zlib)

# Found by name rather than by pkg-config because mbedTLS 2.x does not ship a
# .pc file on the distributions this builds on -- worth saying, because the
# obvious first assumption on seeing find_library is that somebody did not know
# about pkg_check_modules.
find_library(MBEDTLS_LIB mbedtls REQUIRED)
find_library(MBEDX509_LIB mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)

if(NOT RECONOS_TESTS_ONLY)

pkg_check_modules(WLR REQUIRED wlroots)
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

# ReconOS's own protocol: the one thing the compositor tells a client that no
# standard protocol covers -- what this desktop looks like. Generated the same
# way as xdg-shell, from XML in protocol/.
set(RECON_THEME_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/recon-theme.xml")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.h"
    COMMAND "${WAYLAND_SCANNER_EXEC}" server-header
            "${RECON_THEME_XML}" "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.h"
    DEPENDS "${RECON_THEME_XML}"
    COMMENT "Generating recon-theme server header"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.c"
    COMMAND "${WAYLAND_SCANNER_EXEC}" private-code
            "${RECON_THEME_XML}" "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.c"
    DEPENDS "${RECON_THEME_XML}"
    COMMENT "Generating recon-theme protocol code"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER_EXEC}" client-header
            "${RECON_THEME_XML}" "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.h"
    DEPENDS "${RECON_THEME_XML}"
    COMMENT "Generating recon-theme client header"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.c"
    COMMAND "${WAYLAND_SCANNER_EXEC}" private-code
            "${RECON_THEME_XML}" "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.c"
    DEPENDS "${RECON_THEME_XML}"
    COMMENT "Generating recon-theme client code"
    VERBATIM)

# --- Executable ---

add_executable(ReconOS
    src/main.c
    # The keys, as ReconOS's own rather than as xkbcommon's. One typedef in
    # recon_ui.h used to put twenty-four of these files out of reach of a
    # compiler with no Linux under it.
    src/recon_key.c
    # The last inch of recon_ui.c: how a panel's pixels reach a screen.
    # Split out so the three thousand lines of drawing above it stop
    # needing a wayland header -- see src/recon_ui_internal.h.
    src/recon_ui_wlr.c
    # Where the wayland stops for an application: a few fields and one
    # action, asked for rather than reached into. See
    # include/recon_server_facts.h.
    src/recon_server_facts.c
    # And the other implementation of that seam: straight onto a screen,
    # which is what a program on the ReconOS kernel does.
    src/recon_ui_fb.c
    src/recon_apps.c
    src/recon_avatar.c
    src/recon_modules.c
    src/recon_capture.c
    src/recon_net.c
    src/recon_png.c
    src/recon_image.c
    src/recon_expr.c
    src/recon_data.c
    src/recon_sniff.c
    src/recon_themewl.c
    src/recon_fonts.c
    src/recon_wallpaper.c
    src/recon_appwin.c
    src/recon_loop_wl.c
    src/recon_cmd.c
    src/recon_calendar.c
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
    src/recon_photos.c
    src/recon_registry.c
    src/recon_stb.c
    src/recon_keyring.c
    src/recon_sealed.c
    src/recon_cookie_store.c
    src/recon_crypt.c
    src/recon_sign.c
    src/recon_users.c
    src/recon_service.c
    src/recon_session.c
    src/recon_access.c
    src/recon_help.c
    src/recon_package.c
    src/recon_manifest.c
    src/recon_clip.c
    src/recon_appicon.c
    src/recon_props.c
    src/recon_theme.c
    src/recon_titlebar.c
    src/recon_widget.c
    src/recon_widget_state.c
    src/recon_procinfo.c
    src/recon_shell.c
    src/recon_taskmgr.c
    src/recon_terminal.c
    src/recon_ui.c
    src/recon_version.c
    src/recon_audio.c
    src/recon_codec.c
    src/recon_color.c
    src/recon_mp4.c
    src/recon_movie.c
    src/recon_ocr.c
    src/recon_ocr_match.c
    src/recon_video.c
    src/recon_player.c
    src/recon_html.c
    src/recon_css.c
    src/recon_web.c
    src/recon_http.c
    src/recon_url.c
    src/recon_media.c
    src/recon_form.c
    src/recon_cookie.c
    src/recon_mail.c
    src/recon_smtp.c
    src/recon_smtp_message.c
    src/recon_mailwin.c
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
    "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-protocol.h")

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
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(ReconOS SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Modules are loaded with dlopen and call back into ReconOS directly, so the
# executable must export its own symbols rather than hiding them. Without this
# a module links but every ReconOS function it calls is undefined at load.
set_target_properties(ReconOS PROPERTIES ENABLE_EXPORTS ON)

# zlib and mbedTLS are found near the top now, because suites of their own
# need them and those are built even when the compositor is not.
#
# ALSA, for making a sound. Optional: a machine with no audio library still
# builds a desktop, and the path it then takes is the same one a machine with
# no sound *card* takes -- which is worth having be the same code rather than
# a second one nobody exercises.
#
# ALSA rather than a sound server on purpose. It is the lowest thing on Linux
# that is still an interface, so recon_audio.c has the shape a real driver has
# and is replaced later rather than rethought. THIRD_PARTY.md's line is
# "libraries may parse formats and talk to hardware"; this is the second half.
pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
    message(STATUS "ALSA found (${ALSA_VERSION}); ReconOS can play sound")
else()
    message(STATUS "ALSA not found; ReconOS will build without audio")
endif()

if(ALSA_FOUND)
    target_compile_definitions(ReconOS PRIVATE RECON_HAVE_ALSA)
    target_include_directories(ReconOS PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(ReconOS PRIVATE ${ALSA_LIBRARIES})
endif()

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

endif()   # NOT RECONOS_TESTS_ONLY

# --- Tests ---
#
# The filesystem operations are tested on their own because they are the ones
# that lose a user's work when they are wrong, and clicking through a desktop
# is a poor way to find that out. recon_fs.c has no dependency on wlroots, so
# the test binary needs nothing but the standard library.

add_executable(recon_fs_tests
    tests/test_fs.c
    src/recon_fs.c
    # The filesystem reports a path that resolves outside it, so the
    # code table comes along.
    src/recon_error.c)

target_include_directories(recon_fs_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The Recycle Bin, which nothing ran at all.
#
# `coverage.sh --zero` listed fourteen functions of recon_fs.c that no suite
# reached, and they were the whole of the bin. It is worth its own file rather
# than more of test_fs.c because what it is for is different: every other part
# of that file either works or says it did not, and the bin's entire job is
# that deleting is *recoverable* -- which fails silently, and costs somebody
# their files when it does.
add_executable(recon_trash_tests
    tests/test_trash.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_trash_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_test(NAME recycle-bin COMMAND recon_trash_tests)

# The registry's failures are quiet -- a value that reads back wrong makes the
# desktop behave slightly oddly forever and nobody connects the two -- so it is
# tested the same way, against a throwaway root.
add_executable(recon_registry_tests
    tests/test_registry.c
    src/recon_registry.c
    src/recon_fs.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

target_include_directories(recon_registry_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# A skin's failures are silent: a role no skin answers draws as nothing and
# looks like a missing feature rather than a missing colour.
add_executable(recon_theme_tests
    tests/test_theme.c
    src/recon_theme.c
    src/recon_titlebar.c
    src/recon_color.c
    src/recon_registry.c
    src/recon_fs.c
    # Installing a skin writes into /System, so recon_theme asks who is
    # signed in before it tries. Accounts and password hashing come along
    # with that -- neither needs a compositor, which is the property these
    # test targets are actually protecting.
    src/recon_users.c
    src/recon_crypt.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

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
    src/recon_fs.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

target_include_directories(recon_fonts_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The encryption's failures are invisible from a running system: a certificate
# remade on every start still works, and breaks every client that pinned the
# old one. A world-readable private key works perfectly too.
add_executable(recon_tls_tests
    tests/test_tls.c
    src/recon_tls.c
    src/recon_crypt.c
    src/recon_fs.c
    # The filesystem reports a path that resolves outside it, so the
    # code table comes along.
    src/recon_error.c)

target_include_directories(recon_tls_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_tls_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

# A panel drawn straight onto a screen, which is what a program on the ReconOS
# kernel does. It takes no system calls -- the caller passes the address and
# the shape it already asked the kernel for -- so the same code runs against a
# plain buffer here, and every canvas in the suite is padded at the end of each
# row because bytes-per-row is not width times four on real hardware.
add_executable(recon_ui_fb_tests
    tests/test_ui_fb.c
    src/recon_ui_fb.c
    src/recon_ui.c
    src/recon_color.c
    src/recon_clip.c
    src/recon_theme.c
    src/recon_error.c
    src/recon_fs.c
    src/recon_registry.c
    # The theme asks who may administer, and the UI asks what a key stands
    # for. Both come along the way the other suites take what they reach.
    src/recon_key.c
    src/recon_users.c
    src/recon_crypt.c)

target_include_directories(recon_ui_fb_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(recon_ui_fb_tests PRIVATE m)

# The keys, held against the library their numbers came from.
#
# include/recon_key.h exists so that twenty-four of the desktop's sources stop
# needing a Linux keyboard library in order to name a key. Its values were
# taken out of xkbcommon once, by asking it -- and a borrowed table with
# nothing checking the borrow is a copy that is right on the day it is made.
# This is the one file on the desktop side that still links xkbcommon, which is
# deliberate: its job is to disagree with it.
add_executable(recon_key_tests
    tests/test_key.c
    src/recon_key.c)

target_include_directories(recon_key_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_key_tests PRIVATE ${XKB_LIBRARIES})
target_include_directories(recon_key_tests PRIVATE ${XKB_INCLUDE_DIRS})

# The other direction: whether to believe somebody else's certificate.
#
# A suite of its own rather than more of test_tls.c, because it needs a
# certificate authority, five certificates with a different fault built into
# each, and a server to present them -- and because what it holds is different.
# test_tls.c is about the identity this machine presents; this is about the
# four sentences somebody reads when a connection is refused, each of which is
# chosen from a flag mbedTLS sets and none of which anything was checking.
add_executable(recon_tls_verify_tests
    tests/test_tls_verify.c
    src/recon_tls.c
    src/recon_crypt.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_tls_verify_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_tls_verify_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

# Calendar arithmetic is wrong in 2100 or it is not, and nothing but a test
# finds out which for seventy years. The twelve-hour clock's two special cases
# are wrong twice a day and nobody notices either.
add_executable(recon_clock_tests
    tests/test_clock.c
    src/recon_clock.c
    # The plain half of the event-loop seam, not the wayland one. A suite that
    # had to link a compositor to check how an SNTP reply is read would be a
    # suite nobody could run headless -- which is the whole reason the seam
    # exists.
    src/recon_loop.c
    # The clock asks recon_net whether there is a way out before it offers to
    # check the time, and recon_net asks the firewall before it opens
    # anything -- so both come along, the same way the network tests take them.
    src/recon_net.c
    src/recon_firewall.c
    src/recon_error.c
    src/recon_registry.c
    # recon_net can open an encrypted stream, so it reaches the TLS layer --
    # and the linker wants that whether or not this test opens one.
    src/recon_tls.c
    src/recon_crypt.c
    src/recon_fs.c)

target_include_directories(recon_clock_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_clock_tests PRIVATE ${WAYLAND_LIBRARIES}
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

# Deciding which pixels are ink, finding the bands text sits in, and splitting
# a band into marks all happen before any font is involved. That is what makes
# them testable without one -- and the images are built a rectangle at a time,
# so the right answer is known rather than inspected. An OCR test that runs on a
# photograph can only be checked by reading its output, which means it passes
# whenever the output looks plausible.
add_executable(recon_ocr_tests
    tests/test_ocr.c
    src/recon_ocr.c)

target_include_directories(recon_ocr_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The matching half needs a typeface, so it needs the vendored rasteriser --
# the first test target in the tree to reach into third_party. It still needs no
# display: recon_ocr_match.c deliberately does not include recon_ui.h, whose
# only implementation drags in wlroots.
add_executable(recon_ocr_match_tests
    tests/test_ocr_match.c
    src/recon_ocr_match.c
    # The marks being named come from the stages that need no font.
    src/recon_ocr.c)

target_include_directories(recon_ocr_match_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(recon_ocr_match_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# stb_truetype calls floor, ceil, sqrt, pow, fmod, cos and fabs.
target_link_libraries(recon_ocr_match_tests PRIVATE m)

# Colour conversion and scaling have a known right answer, so they are checked
# against it rather than against another implementation. The decoder is verified
# against ffmpeg on a real film, which is right for a decoder and needs a
# library, a file and a subprocess; this needs none of them and runs on a
# machine with no codec pack installed at all.
add_executable(recon_video_tests
    tests/test_video.c
    src/recon_video.c)

target_include_directories(recon_video_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_video_tests PRIVATE m)

# The writer checked against a decoder somebody else wrote. That is the whole
# point of this one: agreeing with itself would prove nothing about whether a
# file it saves opens anywhere. It needs recon_fs because recon_png_write goes
# through it, and zlib because the compression is zlib's.
add_executable(recon_png_tests
    tests/test_png.c
    src/recon_png.c
    src/recon_fs.c
    # The filesystem reports a path that resolves outside it, so the
    # code table comes along.
    src/recon_error.c)

target_include_directories(recon_png_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${ZLIB_INCLUDE_DIRS})
target_include_directories(recon_png_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(recon_png_tests PRIVATE ${ZLIB_LIBRARIES} m)

# Resampling, checked against properties rather than against a fixture: a
# fixture for a scaler is a picture the scaler made, which proves only that it
# agrees with itself.
add_executable(recon_image_tests
    tests/test_image.c
    src/recon_image.c)

target_include_directories(recon_image_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Composing a message, without a network stack behind it. The conversation with
# a server needs a server; the rules about what may appear in a header do not,
# and those are the ones that turn one message into two when they are wrong.
add_executable(recon_smtp_tests
    tests/test_smtp.c
    src/recon_media.c
    src/recon_smtp_message.c
    # For base64, which is how an attachment travels. Still no network here:
    # recon_crypt is arithmetic on bytes and nothing else.
    src/recon_crypt.c)

target_include_directories(recon_smtp_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The expression evaluator, which touches no screen and can be asked questions
# with known answers. Most of them are arithmetic anybody can check by hand --
# a test whose expected values came from running the code proves only that the
# code agrees with itself.
add_executable(recon_expr_tests
    tests/test_expr.c
    src/recon_expr.c)

target_include_directories(recon_expr_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_expr_tests PRIVATE m)

# What a file actually is, from its first bytes. Two properties carry this
# suite: that no two signatures can both match -- "BM" is two ordinary letters
# and an ftyp box sits four bytes in, so the table's order is correctness
# rather than tidiness -- and that the disagreement test does not fire on
# ordinary files, because a warning everybody dismisses is worse than none.
add_executable(recon_sniff_tests
    tests/test_sniff.c
    src/recon_sniff.c)

target_include_directories(recon_sniff_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Reading two columns of numbers out of a file. Worth a suite of its own for
# one reason: every way this can fail announces itself except one. A file
# written with decimal commas is not refused and not mangled by a dot-only
# reader -- it comes back as different numbers, with no bad lines counted. The
# suite names that wrong answer explicitly, so removing the check fails loudly
# rather than quietly plotting the wrong picture.
add_executable(recon_data_tests
    tests/test_data.c
    src/recon_data.c)

target_include_directories(recon_data_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_data_tests PRIVATE m)

# Comparing versions with strcmp is right for nine releases and wrong on the
# tenth, in the direction that makes an update system refuse the update.
add_executable(recon_version_tests
    tests/test_version.c
    src/recon_version.c)

target_include_directories(recon_version_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Whether the accessibility skins work is a question about people who are not
# in the room. It is answered by simulating each deficiency and measuring, not
# by looking at the palette and deciding it seems fine.
add_executable(recon_access_tests
    tests/test_access.c
    src/recon_theme.c
    src/recon_titlebar.c
    # The theme colours a role through the tint lens now, so it needs the
    # arithmetic that does it. Costs nothing: recon_color.c has no dependencies.
    src/recon_color.c
    src/recon_registry.c
    src/recon_fs.c
    src/recon_users.c
    src/recon_crypt.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

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
    src/recon_fs.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

target_include_directories(recon_users_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The keyring. The checks that matter here are the refusals: a locked keyring
# hands out nothing, an edited file does not decrypt to *something*, and a
# ciphertext moved to another name does not become that name's secret. A test
# that only proved a secret comes back would pass against a version storing
# everything in plain text.
add_executable(recon_keyring_tests
    tests/test_keyring.c
    src/recon_keyring.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_fs.c
    # Settings and accounts record a
    # fault when one will not save, so the code table comes along.
    src/recon_error.c)

target_include_directories(recon_keyring_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# What the parsers do with input that is wrong, which every other suite here
# does not ask. Truncated at every length, one byte changed at every position,
# and random bytes behind a real header -- all deterministic, because a finding
# that cannot be reproduced has not been found.
#
# It earns most of its keep under scripts/check.sh: the failure it is looking
# for is usually a read past the end, and the sanitizer is what sees that.
# --- Reading CSS ---
#
# Everything this reads can hide text, so the suite spends half its checks on
# selectors that must NOT match: a rule fired too eagerly is worse than one
# never fired at all.
add_executable(recon_css_tests
    tests/test_css.c
    src/recon_css.c)

target_include_directories(recon_css_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_test(NAME css COMMAND recon_css_tests)

# --- Reading HTML ---
#
# There was no suite for the parser at all, which is how BG-160 got in: a page
# with an SVG inside it put "Wikipedia Close" on the title bar. A parser that
# turns somebody else's file into what a window draws wants tests.
add_executable(recon_html_tests
    tests/test_html.c
    src/recon_html.c
    src/recon_css.c)

target_include_directories(recon_html_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_test(NAME html COMMAND recon_html_tests)

# Reading an address.
#
# There was no suite for this at all, which is how the fragment bug got in:
# `<a href="#main">` parsed to an empty fragment, because the code that pulls
# the fragment out searches the path -- and the path that branch writes is the
# base's, whose hash was stripped long before. Every link within a page
# resolved to "no named place".
#
# One file, and it opens nothing. That is the point of having split it out of
# recon_http.c: the parser used to be untestable without sockets, TLS, the
# registry and a Wayland event loop, so it went untested.
add_executable(recon_url_tests
    tests/test_url.c
    src/recon_url.c)

target_include_directories(recon_url_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_test(NAME url COMMAND recon_url_tests)

# What a form sends.
#
# The same split, one layer up. These rules decide what leaves the machine --
# which controls contribute, what an unchecked box does, what a menu sends --
# and every one of them is wrong in one case out of eight without anything
# looking wrong: a request the server rejects for a reason nobody can see, or
# one it accepts with an answer missing.
#
# recon_form.c opens nothing and draws nothing, which is why this suite links
# four files instead of the whole compositor.
add_executable(recon_form_tests
    tests/test_form.c
    src/recon_media.c
    src/recon_form.c
    src/recon_html.c
    src/recon_css.c
    src/recon_url.c)

target_include_directories(recon_form_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_test(NAME form COMMAND recon_form_tests)

# Cookies.
#
# The third file split out for the same reason, and the one where it matters
# most: these rules decide what a server is told about somebody on every
# request they make. A cookie policy that is too generous behaves identically
# to a correct one until the day it does not, so the refusals are the part that
# has to be exercised deliberately -- nothing reaches them by accident.
#
# recon_cookie.c opens nothing and has no idea what a socket is -- and it still
# does not, which is the point of src/recon_cookie_store.c being a separate
# file. The store brings a cipher, the keyring and a filesystem; the jar brings
# none of them and stays on the freestanding list because of it.
add_executable(recon_cookie_tests
    tests/test_cookie.c
    src/recon_cookie.c
    src/recon_cookie_store.c
    src/recon_sealed.c
    src/recon_keyring.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_cookie_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_cookie_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_test(NAME cookies COMMAND recon_cookie_tests)

# The C library the desktop will be built against when it runs on our kernel.
#
# Compiled *for the host*, with every function renamed by `prefix.h`, so that
# ReconOS's implementation and the system's are in one program and can be
# compared directly. That is the only kind of test worth having for these:
# a `strcmp` returning the wrong sign still sorts, backwards, and would pass
# any test written from memory of what it should do.
#
# -fno-builtin matters and is not belt and braces. Without it the compiler
# recognises the byte loop inside `memcpy` and replaces it with a call to
# `memcpy`, which builds and then recurses until the stack is gone.
add_executable(recon_libc_tests
    userland/tests/test_libc.c
    userland/libc/string.c
    userland/libc/printf.c)

# The file layer, the number parsing and the character classes, in a second
# program because the first one already has a `main`.
#
# `hostsys.c` is what makes the FILE half testable at all: it answers the five
# primitives `stdio.c` is built on with POSIX calls instead of ReconOS system
# calls, so the same buffering code that will run on the kernel can be made to
# read a real file here and be held against what the host's stdio returns for
# the same file. `userland/libc/syscalls.c` -- the real ones -- is deliberately
# *not* in this list; it is the one file that could not be in it.
add_executable(recon_libc_files_tests
    userland/tests/test_libc_files.c
    userland/tests/hostsys.c
    userland/libc/string.c
    userland/libc/printf.c
    userland/libc/ctype.c
    userland/libc/stdlib.c
    userland/libc/stdio.c
    userland/libc/time.c
    userland/libc/math.c)

# Every source under libc/, and only those. `hostsys.c` is excluded on purpose:
# it has to see the host's own <stdlib.h> and <unistd.h> unrenamed, and it
# defines the `recon_sys_*` names, which prefix.h does not touch.
set_source_files_properties(
    userland/libc/string.c
    userland/libc/printf.c
    userland/libc/ctype.c
    userland/libc/stdlib.c
    userland/libc/stdio.c
    userland/libc/time.c
    userland/libc/math.c
    userland/libc/malloc.c
    userland/libc/errno.c
    userland/libc/posix.c
    userland/libc/scanf.c
    userland/libc/inet.c
    userland/libc/socket.c
    userland/libc/signal.c
    PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/prefix.h;-fno-builtin;-fno-math-errno")

# There is deliberately no `-I userland/include` on either of these targets.
# Those headers exist so desktop source can say `#include <stdio.h>` and get
# ReconOS's; putting them on the include path *here* would hand the same header
# to the test file, and the comparison would become the implementation against
# itself. The library's own sources include "internal.h" by relative path and
# never need the public headers -- which is the whole reason internal.h exists.

# Addresses and byte order, against the host's.
#
# The five of the socket group that need nothing from the kernel. The byte
# order is the interesting half: comparing `htons` against glibc is weaker than
# it looks, because both architectures here are little-endian and a version
# written as a byte swap agrees on every machine in the rig. So the suite also
# checks the bytes *directly* -- most significant first, which is what network
# order means on a machine of either endianness.
add_executable(recon_libc_inet_tests
    userland/tests/test_libc_inet.c
    userland/libc/inet.c
    userland/libc/errno.c
    userland/libc/printf.c
    userland/libc/string.c)

# Sockets, against the host's -- and the first thing to exercise the five
# system calls the kernel took numbers for on 15 September.
#
# Differential where it can be: hostsys.c answers the five primitives with real
# POSIX sockets, so ReconOS's connect and the host's talk to the same listener
# and are required to behave the same way. The one place it cannot be is
# `accept`, which on this kernel does not block -- so the stand-in is made
# non-blocking too, because a stand-in looser than the real call is the one way
# a stand-in can be worse than none.
add_executable(recon_libc_socket_tests
    userland/tests/test_libc_socket.c
    userland/tests/hostsys.c
    userland/libc/socket.c
    userland/libc/posix.c
    userland/libc/inet.c
    userland/libc/errno.c
    userland/libc/stdio.c
    userland/libc/printf.c
    userland/libc/string.c
    userland/libc/ctype.c
    userland/libc/stdlib.c
    userland/libc/malloc.c)

# No include directory and no -include here, for the reason stated above the
# first of these targets: both would hand the test file ReconOS's own headers
# and its own names, and the comparison would become the implementation against
# itself. socket.c reaches its headers by relative path, as posix.c does, and
# gets prefix.h from the list above.

add_test(NAME libc_sockets COMMAND recon_libc_socket_tests)

# Signals, against the host's.
#
# Differential where it can be: hostsys.c answers the three primitives with
# POSIX kill and sigaction, so a handler installed through ReconOS's signal()
# is installed with the real delivery machinery underneath and has to actually
# run when the signal arrives.
#
# The one part a host cannot exercise is the restorer -- glibc provides its
# own and hostsys.c drops ours. That is said in both files rather than left
# for somebody to read a green suite as covering it; what proves the restorer
# is a program in ring 3.
add_executable(recon_libc_signal_tests
    userland/tests/test_libc_signal.c
    userland/tests/hostsys.c
    userland/libc/signal.c
    userland/libc/posix.c
    userland/libc/errno.c
    userland/libc/stdio.c
    userland/libc/printf.c
    userland/libc/string.c
    userland/libc/ctype.c
    userland/libc/stdlib.c
    userland/libc/malloc.c)

add_test(NAME libc_signals COMMAND recon_libc_signal_tests)

# <limits.h>, against the host's.
#
# A header of nothing but macros, which makes it look like the one file here
# that could not be wrong. It is the opposite: a wrong limit is silent. Code
# that checks INT_MAX before doing arithmetic gets a wrong answer and carries
# on, and nothing that does not look at this header would disagree.
#
# No library sources at all -- the test includes the host's <limits.h>, copies
# its values, undefines them, and includes ReconOS's by relative path.
add_executable(recon_libc_limits_tests
    userland/tests/test_libc_limits.c)

add_test(NAME libc_limits COMMAND recon_libc_limits_tests)

# sscanf, against the host's.
#
# The failure that matters for a scanner is assigning the right value to the
# *wrong variable*, so every case in this suite compares a whole block of
# storage rather than the fields it expected to be filled -- a variable that
# should not have been touched is checked to be untouched.
#
# Its corpus opens with the desktop's own fourteen formats, verbatim.
add_executable(recon_libc_scanf_tests
    userland/tests/test_libc_scanf.c
    userland/tests/hostsys.c
    userland/libc/scanf.c
    userland/libc/stdlib.c
    userland/libc/ctype.c
    userland/libc/string.c
    userland/libc/printf.c
    userland/libc/math.c)

# The descriptor layer -- open, read, write, directories -- against the host's.
#
# `hostsys.c` is what makes it testable: it answers the eight primitives the
# library is built on with POSIX, *including a recon_sys_list that behaves like
# the kernel's* rather than like POSIX readdir. That last point is the one that
# matters -- a stand-in with a looser contract than the real call would let the
# layer pass here while reading half a directory on a machine.
#
# It reads `kernel/include/recon/kernel/process.h` while it runs, to compare
# the descriptor limit against the kernel's own, so it has a WORKING_DIRECTORY.
add_executable(recon_libc_posix_tests
    userland/tests/test_libc_posix.c
    userland/tests/hostsys.c
    userland/libc/posix.c
    userland/libc/errno.c
    userland/libc/malloc.c
    userland/libc/printf.c
    userland/libc/string.c)

# errno and strerror, in a program of its own.
#
# It reads `userland/include/recon.h` while it runs -- the kernel's own list of
# error numbers -- and compares it with what the translation covers. That is
# why it has a WORKING_DIRECTORY: the check is worthless if the suite gets its
# idea of the kernel's errors from the same place the translation did.
add_executable(recon_libc_errno_tests
    userland/tests/test_libc_errno.c
    userland/libc/errno.c
    userland/libc/printf.c
    userland/libc/string.c)

# The allocator, in a program of its own.
#
# `mem_recon.c` is deliberately not in this list. It is the one file of the
# library that makes a system call, and the suite's whole method is to hand the
# allocator a source it controls: one that can release memory, one that cannot
# -- which is the configuration ReconOS is in today -- and one that refuses
# after a while. None of those can be arranged by booting a kernel, and all
# three are configurations the desktop will meet.
#
# It links against nothing but the host's own libc for the reference half:
# `calloc`'s overflow refusal, `realloc(p, 0)` and `strdup` are all held
# against what the library this replaces does, because the standard leaves two
# of those implementation-defined and the desktop will be compiled against one
# and run on the other.
add_executable(recon_libc_malloc_tests
    userland/tests/test_libc_malloc.c
    userland/libc/malloc.c
    userland/libc/string.c)

# The maths, in a program of its own.
#
# Separate because it needs no hostsys.c -- math.c makes no system call and
# opens nothing, so the one thing the other two suites need a kernel stand-in
# for, this one does not. It links against the host math library because the
# *reference* half of every comparison is the host's.
add_executable(recon_libc_math_tests
    userland/tests/test_libc_math.c
    userland/libc/math.c)

target_link_libraries(recon_libc_math_tests PRIVATE m)

# The first screen ReconOS draws, rendered on the host.
#
# userland/init/screen.c makes no system call and knows nothing about a
# kernel, which is the whole reason it is a separate file: the picture a
# machine will show can be produced here in a millisecond instead of by
# writing a stick and walking to a machine.
#
# Every canvas it draws on has padding on the end of each row, and the suite
# checks that nothing touched it -- because on a screen where bytes per row
# happen to equal width times four, a program that confuses the two draws a
# perfect picture and shears on the first laptop.
add_executable(recon_init_screen_tests
    userland/tests/test_init_screen.c
    userland/init/screen.c)

target_include_directories(recon_init_screen_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/boot/include")

add_test(NAME first_screen COMMAND recon_init_screen_tests)

# Waiting.
#
# src/recon_loop.c reads no clock and makes no system call -- the time arrives
# as an argument -- so every interesting question about a timer can be asked
# here instead of by watching a machine and counting. Eight thousand lines of
# session, player, photos, task manager, clock and audio sit on it.
#
# One source and one test, because that is all it needs: the wayland half is
# the compositor's own loop and is exercised by the desktop running.
add_executable(recon_loop_tests
    tests/test_loop.c
    src/recon_loop.c)

target_include_directories(recon_loop_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_test(NAME event_loop COMMAND recon_loop_tests)

# The desktop program itself, run.
#
# It could not be, until the calls it makes of the machine went behind
# userland/desktop/machine.h. recon_screen and recon_map are static inline in
# <recon.h> and expand to a raw syscall instruction, so there was no function
# for a shim to replace -- which made the one file that decides between a
# desktop and a black screen the only one in its directory nothing could run.
#
# The fake machine here is malloc and a table of keystrokes. What it holds is
# everything main.c decides, which is all of it about what happens when
# something is wrong: eight exit codes, the order the screen and the keyboard
# are opened in, and a read whose length is not a whole number of events.
#
# machine_recon.c is deliberately absent: it is the ReconOS implementation and
# the only file here a host cannot run.
add_executable(recon_desktop_main_tests
    userland/tests/test_desktop_main.c
    userland/desktop/desktop.c
    userland/desktop/keyboard.c
    userland/desktop/shell_frame.c
    src/recon_key.c
    src/recon_ui.c
    src/recon_ui_fb.c
    src/recon_theme.c
    src/recon_fonts.c
    src/recon_fs.c
    src/recon_error.c
    src/recon_widget.c
    src/recon_color.c
    src/recon_clip.c
    src/recon_widget_state.c
    src/recon_icons.c
    src/recon_registry.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_ico.c
    src/recon_stb.c)

target_include_directories(recon_desktop_main_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/userland/desktop")

# userland/include goes to main.c alone, and that is not fussiness.
#
# It has to be on main.c's path: recon_machine.h includes <recon.h> with angle
# brackets, so the directory must be findable. It must NOT be on the test's:
# ReconOS's <stdio.h> would then answer instead of the host's, and it
# deliberately has no printf -- its own header argues that there is no printf
# to a stream this library cannot usefully flush, which is a decision rather
# than a gap.
#
# So the two sources get different paths. main.c compiles against ReconOS's
# headers, which is what it will be built with; the test compiles against the
# host's, which is the older rule -- a test given ReconOS's headers is a test
# comparing ReconOS against itself.
set_source_files_properties(userland/desktop/desktop.c
    TARGET_DIRECTORY recon_desktop_main_tests
    PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/userland/include")

target_include_directories(recon_desktop_main_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(recon_desktop_main_tests PRIVATE m)

add_test(NAME desktop_program COMMAND recon_desktop_main_tests)

# Events from /dev/input, turned into keystrokes.
#
# keyboard.c makes no system call, so the stream can be driven from a table of
# events written by hand -- including the ones a real keyboard only produces
# once something has already gone wrong, which are the ones worth testing and
# the hardest to arrange on a machine.
#
# It runs from the source directory because one of its checks reads the
# kernel's own header and requires the keycodes to agree. That duplication is
# real -- the two headers cannot be one file -- so it is tested rather than
# trusted.
add_executable(recon_desktop_keyboard_tests
    userland/tests/test_desktop_keyboard.c
    userland/desktop/keyboard.c
    src/recon_key.c)

# Deliberately NOT userland/include: a test compiled against this library's
# own <stdio.h> is a test comparing ReconOS against ReconOS, and the immediate
# symptom is losing the host's printf. sys/input.h is reached by relative path
# instead, which is the exception libc/posix.c already takes.
target_include_directories(recon_desktop_keyboard_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/userland/desktop")

add_test(NAME desktop_keyboard COMMAND recon_desktop_keyboard_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# The desktop's first frame, drawn on the host.
#
# The same argument as the first-boot screen above, one layer up, and with one
# difference that is the point of it: `screen.c` writes pixels itself because
# it has to work before there is a volume to read a theme off. This draws with
# the *real* layer -- recon_theme, recon_ui, the font loader -- so the day the
# frame appears on a machine, what appeared is the desktop's own drawing code
# rather than a picture of it.
#
# It links what a drawing program links and no more, which is also a claim
# worth having a build enforce: if this target ever needs the compositor added
# to it, the seam has moved.
add_executable(recon_desktop_frame_tests
    userland/tests/test_desktop_frame.c
    userland/desktop/shell_frame.c
    src/recon_ui.c
    src/recon_ui_fb.c
    src/recon_theme.c
    src/recon_fonts.c
    src/recon_fs.c
    src/recon_error.c
    src/recon_widget.c
    src/recon_color.c
    src/recon_clip.c
    src/recon_widget_state.c
    src/recon_icons.c
    src/recon_key.c
    src/recon_registry.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_ico.c
    src/recon_stb.c)

target_include_directories(recon_desktop_frame_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/userland/desktop")

# The text rasteriser is vendored. -isystem rather than -I, because
# THIRD_PARTY.md says these are not held to this project's warnings.
target_include_directories(recon_desktop_frame_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(recon_desktop_frame_tests PRIVATE m)

add_test(NAME desktop_frame COMMAND recon_desktop_frame_tests)

# The shape a ReconOS volume has, checked without one.
#
# layout.c is handed the thing that makes a directory, so the suite hands it a
# fake -- and every decision it makes is checked on the host with no volume, no
# kernel and no disk.
#
# It also reads include/recon_fs.h and requires the two lists of directories to
# agree in both directions. They cannot be one file: the header is the
# compositor's and reaches for <time.h> and a filesystem interface a program on
# this kernel does not have. A real duplication is a thing to test.
add_executable(recon_init_layout_tests
    userland/tests/test_init_layout.c
    userland/init/layout.c)

add_test(NAME volume_layout COMMAND recon_init_layout_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME libc COMMAND recon_libc_tests)
add_test(NAME libc_files COMMAND recon_libc_files_tests)
add_test(NAME libc_inet COMMAND recon_libc_inet_tests)
add_test(NAME libc_scanf COMMAND recon_libc_scanf_tests)
add_test(NAME libc_posix COMMAND recon_libc_posix_tests)
set_tests_properties(libc_posix PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME libc_errno COMMAND recon_libc_errno_tests)
set_tests_properties(libc_errno PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME libc_malloc COMMAND recon_libc_malloc_tests)
add_test(NAME libc_math COMMAND recon_libc_math_tests)

# The same parser against markup nobody here wrote.
#
# Every other suite in this project was written by whoever wrote the code it
# tests, which is the failure BG-162 records: the cause was diagnosed by
# reading the source, a test was written for the markup that diagnosis
# assumed, it passed, and the page still rendered wrong. These fifteen hundred
# cases come from the html5lib tree-construction suite, written by people
# whose purpose was to break HTML parsers.
#
# WORKING_DIRECTORY because the corpus is a file the test reads, and ctest
# runs from the build directory where that path does not resolve.
add_executable(recon_html_corpus_tests
    tests/test_html_corpus.c
    src/recon_html.c
    src/recon_css.c)

target_include_directories(recon_html_corpus_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_test(NAME html-corpus COMMAND recon_html_corpus_tests)
set_tests_properties(html-corpus PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(recon_malformed_tests
    tests/test_malformed.c
    src/recon_ico.c
    src/recon_stb.c
    src/recon_mp4.c
    src/recon_html.c
    src/recon_css.c
    # Addresses come off somebody else's page too. recon_url.c opens nothing,
    # which is why it could be added here without dragging in a socket.
    src/recon_url.c
    # And what a form sends, which is the far end of the same journey: a page
    # somebody else wrote deciding which bytes this machine puts on the wire.
    src/recon_media.c
    src/recon_form.c
    # And cookies, where a header somebody else wrote decides what is sent on
    # every request after it.
    src/recon_cookie.c
    src/recon_expr.c)

# The ICO decoder is built on stb_image, so this reaches into third_party the
# same way the image suite does. It still needs no display, which is the
# property every test target here exists to keep.
# Borrowed code, and the rule is that borrowed code is owned once it is here --
# which means not editing somebody else's decoder to satisfy a warning switch
# this project chose. Turned off for this one file rather than for the build.
set_source_files_properties(src/recon_stb.c PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(recon_malformed_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(recon_malformed_tests PRIVATE m)

# The firewall, which had no suite at all until scripts/coverage.sh said so:
# 747 lines, compiled into the network target as a dependency and never once
# called. What is tested is the answer rather than the bookkeeping -- order
# deciding, a rule that is off not being consulted, a rule for one program not
# answering for another, and a rule that ships not being removable.
add_executable(recon_firewall_tests
    tests/test_firewall.c
    src/recon_firewall.c
    src/recon_registry.c
    src/recon_error.c
    src/recon_fs.c)

target_include_directories(recon_firewall_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# What state a control is in, and what colour that makes it.
#
# The widget layer is the answer to "why does nothing in this system react to
# the pointer", and most of that answer is pixels. This is the part that is not:
# four rules about three numbers, one of which -- held, but the pointer slid
# off -- looks correct in the code and is the difference between a close button
# somebody can change their mind about and one they cannot.
add_executable(recon_widget_tests
    tests/test_widget.c
    src/recon_widget_state.c
    src/recon_theme.c
    src/recon_titlebar.c
    src/recon_color.c
    # A skin is read through the registry and installing one asks who is
    # signed in, which is the same tail every other theme-touching suite
    # carries. None of it needs a compositor, which is the property these
    # targets exist to protect.
    src/recon_users.c
    src/recon_crypt.c
    src/recon_registry.c
    src/recon_error.c
    src/recon_fs.c)

target_include_directories(recon_widget_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Where the things on a title bar go -- the other file coverage found at zero.
# Small, and it holds the rule this project states most loudly: close is always
# there and always outermost. That had been proved by photographing one skin;
# this proves it for all eight sets of buttons on both sides, without a screen.
add_executable(recon_titlebar_tests
    tests/test_titlebar.c
    src/recon_titlebar.c
    src/recon_theme.c
    # Installing and copying a skin is administration, so the theme asks who
    # is signed in. Nobody is, in a test, and the answer to that is the one
    # recon_users gives rather than a stub that would answer differently.
    src/recon_users.c
    src/recon_crypt.c
    src/recon_color.c
    src/recon_registry.c
    src/recon_error.c
    src/recon_fs.c)

target_include_directories(recon_titlebar_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The error catalogue, and what raising one does. Linked into eight targets and
# exercised by none of them, which coverage said and reading did not. The rules
# at the top of include/recon_errors.def -- a number never reused, an area
# letter that is one of the listed areas -- were stated and unenforced; a
# duplicate is two hundred lines from its twin and invisible in the file.
add_executable(recon_error_tests
    tests/test_error.c
    src/recon_error.c
    src/recon_fs.c)

target_include_directories(recon_error_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# AES-GCM comes from mbedTLS, for the reason stated in the keyring's header:
# the thing that actually protects a password is not written here.
target_link_libraries(recon_keyring_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

# Networking reports rather than computes, which is exactly the kind of code
# that goes wrong quietly: a wrong answer looks like a true fact about a
# different machine. These check the parts that are true of any machine --
# loopback exists, an interface is listed once -- rather than of this one.
add_executable(recon_net_tests
    tests/test_net.c
    src/recon_net.c
    # The plain half of the event-loop seam. recon_net.c goes through it now
    # rather than holding a compositor's loop -- which is what makes
    # `stream_event`, 106 lines of state machine, reachable from a suite at
    # all. The clock suite has linked this for the same reason since v0.4.58.
    src/recon_loop.c
    src/recon_registry.c
    # Opening a connection asks the firewall, and a refusal is an error with
    # a code. Neither needs a compositor, which is the property these test
    # targets exist to protect.
    src/recon_firewall.c
    src/recon_error.c
    # An encrypted stream is opened from here, so the TLS layer comes along.
    # It brings mbedTLS and no compositor, which keeps the property below.
    src/recon_tls.c
    src/recon_crypt.c
    src/recon_fs.c)

target_include_directories(recon_net_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Only wayland-server, because that is genuinely all recon_net needs: an
# event loop to hang a socket on. No wlroots, no generated protocol headers,
# no compositor.
target_link_libraries(recon_net_tests PRIVATE ${WAYLAND_LIBRARIES}
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
target_include_directories(recon_net_tests PRIVATE ${WAYLAND_INCLUDE_DIRS})

#
# Every suite, registered.
#
# Eleven of the twenty-five were, and the other fourteen -- including the
# malformed-input sweep, the firewall, the keyring and the error catalogue --
# had to be found and run by hand. That is not a small gap: `ctest` reporting
# "100% tests passed" while more than half the suites had not been run is
# exactly the shape of green this project keeps writing tests against.
#
# The names are what a person would call the thing rather than the target's
# filename, because they are what ctest prints.
#
enable_testing()
add_test(NAME filesystem COMMAND recon_fs_tests)
add_test(NAME registry COMMAND recon_registry_tests)
add_test(NAME theme COMMAND recon_theme_tests)
add_test(NAME fonts COMMAND recon_fonts_tests)
add_test(NAME tls COMMAND recon_tls_tests)
add_test(NAME tls-verify COMMAND recon_tls_verify_tests)
add_test(NAME keys COMMAND recon_key_tests)
add_test(NAME screen-panel COMMAND recon_ui_fb_tests)
add_test(NAME clock COMMAND recon_clock_tests)
add_test(NAME versions COMMAND recon_version_tests)
add_test(NAME accessibility COMMAND recon_access_tests)
add_test(NAME hashing COMMAND recon_crypt_tests)
add_test(NAME accounts COMMAND recon_users_tests)
add_test(NAME network COMMAND recon_net_tests)
add_test(NAME expressions COMMAND recon_expr_tests)
add_test(NAME reading-numbers COMMAND recon_data_tests)
add_test(NAME what-a-file-is COMMAND recon_sniff_tests)
add_test(NAME text-in-a-picture COMMAND recon_ocr_tests)
add_test(NAME matching-letters COMMAND recon_ocr_match_tests)
add_test(NAME video COMMAND recon_video_tests)
add_test(NAME png COMMAND recon_png_tests)
add_test(NAME images COMMAND recon_image_tests)
add_test(NAME sending-mail COMMAND recon_smtp_tests)
add_test(NAME keyring COMMAND recon_keyring_tests)

# Sealed files: a file only the signed-in account can read, which is what the
# keyring's own header says belongs somewhere else -- "something larger belongs
# in a file that is encrypted, which is a different thing and is not this".
#
# The checks that matter are the same shape as the keyring's, plus one a
# keyring cannot have: a sealed file is a *file*, and somebody with the disk
# can move one. The name is authenticated as associated data so that moving it
# is a decryption failure, and that is the check a careful round-trip test
# still misses.
add_executable(recon_sealed_tests
    tests/test_sealed.c
    src/recon_sealed.c
    src/recon_keyring.c
    src/recon_users.c
    src/recon_crypt.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_sealed_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_sealed_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_test(NAME sealed-files COMMAND recon_sealed_tests)

# Signatures: who made a package, and has it changed since.
#
# The claims tested are the ones recon_sign.c decides rather than the ones
# mbedTLS does -- that a changed byte stops verifying, that an untrusted key
# does not count, that a private key cannot be added to the trust store, and
# that a key name cannot escape the trust directory.
#
# One of them is load-bearing in a way that is easy to miss: signing the same
# bytes twice must give the same signature. That is RFC 6979 determinism, and
# it is what stops one repeated per-signature secret from handing over the
# private key. recon_sign.c has an #error if mbedTLS is built without it; this
# is what would notice if the #error were removed.
add_executable(recon_sign_tests
    tests/test_sign.c
    src/recon_sign.c
    src/recon_crypt.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_sign_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_sign_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_test(NAME signatures COMMAND recon_sign_tests)

# What a package signature covers.
#
# There was no suite for packages at all, which meant the install path -- the
# one that takes somebody else's shared object and loads it into this process
# -- was the least tested thing in the system.
#
# The claim is narrow and is the point: the signature covers every file the
# package brings, not just its manifest. A manifest-only signature binds the
# names of the files and none of their contents, leaving the module free to be
# swapped, and the module is the only file that can do anything.
# One file, and it loads nothing.
#
# recon_manifest.c reads a manifest and takes digests of what it names;
# recon_package.c installs. They were one file, and this target needed a stub
# that aborted on the module loader, the icon cache and the version comparison
# just to link -- five functions stubbed to make one file testable, which is
# the shape of a file that wants splitting. recon_http.c had already been split
# for the same reason, and the fragment bug is what living with it cost.
add_executable(recon_package_sign_tests
    tests/test_package_sign.c
    src/recon_manifest.c
    src/recon_sign.c
    src/recon_crypt.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_package_sign_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_package_sign_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_test(NAME package-signatures COMMAND recon_package_sign_tests)


# The other half of what a package does: installing it, upgrading it and
# taking it away. See the header of tests/test_package_install.c for why the
# module path is tested from the failure side rather than the success side.
add_executable(recon_package_install_tests
    tests/test_package_install.c
    src/recon_package.c
    src/recon_manifest.c
    src/recon_sign.c
    src/recon_crypt.c
    src/recon_registry.c
    src/recon_users.c
    src/recon_version.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_package_install_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(recon_package_install_tests PRIVATE
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_test(NAME package-install COMMAND recon_package_install_tests)


#
# The module loader, with a real module in front of it.
#
# --- What kept this out of the tree until now ---
#
# src/recon_modules.c is 1,514 lines and nothing had ever linked it. Compiled
# on its own it asks the linker for eighteen symbols, and sixteen are ordinary
# ReconOS sources. The other two are `_wlr_log` and `recon_appwin_destroy` --
# one log call and one destructor -- and those two were the whole reason the
# one place ReconOS runs somebody else's machine code had no suite. Both are
# stood in for in tests/test_modules.c, and no compositor is linked here.
#
# wlroots' *headers* are still needed to compile it, because recon_appwin.h
# names its types. Headers, not libraries: the same arrangement recon_net_tests
# uses, and the property worth keeping is that this suite runs headless.
#

#
# The module, built twice.
#
# Two builds because the tamper test needs a swap that is competent: a valid,
# loadable shared object that is simply not the one the receipt vouches for.
# Replacing a module with rubbish would pass with no gate at all, since dlopen
# refuses rubbish unaided.
#
# MODULE rather than SHARED: this is something to be dlopened, never linked
# against, which is exactly what CMake means by the word.
#
add_library(example_module_one MODULE tests/example_module.c)
target_include_directories(example_module_one PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(example_module_one PRIVATE EXAMPLE_MODULE_MARK="one")
set_target_properties(example_module_one PROPERTIES PREFIX "" SUFFIX ".rex")

add_library(example_module_two MODULE tests/example_module.c)
target_include_directories(example_module_two PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(example_module_two PRIVATE EXAMPLE_MODULE_MARK="two")
set_target_properties(example_module_two PROPERTIES PREFIX "" SUFFIX ".rex")

add_executable(recon_modules_tests
    tests/test_modules.c
    src/recon_modules.c
    src/recon_package.c
    src/recon_manifest.c
    src/recon_sign.c
    src/recon_crypt.c
    src/recon_registry.c
    src/recon_users.c
    src/recon_version.c
    src/recon_fs.c
    src/recon_error.c)

target_include_directories(recon_modules_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${WLR_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS})

target_compile_definitions(recon_modules_tests PRIVATE
    WLR_USE_UNSTABLE
    # The same definition the real build makes, so this suite compiles the
    # file as it ships rather than a variant of it: without it the shipped-
    # modules path compiles to `return 0` and two functions it is the only
    # caller of become unused, which is a warning and, worse, code that is
    # not in the binary being tested.
    RECONOS_MODULE_DIR="${CMAKE_BINARY_DIR}/modules"
    EXAMPLE_MODULE_ONE="$<TARGET_FILE:example_module_one>"
    EXAMPLE_MODULE_TWO="$<TARGET_FILE:example_module_two>")

# The loader itself needs libdl, and -rdynamic so a module can reach back into
# the program that loaded it -- which is how ReconOS is linked and therefore
# the arrangement this has to reproduce to mean anything.
set_target_properties(recon_modules_tests PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(recon_modules_tests PRIVATE
    ${CMAKE_DL_LIBS} ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})

add_dependencies(recon_modules_tests example_module_one example_module_two)

add_test(NAME modules COMMAND recon_modules_tests)


add_test(NAME malformed-input COMMAND recon_malformed_tests)
add_test(NAME firewall COMMAND recon_firewall_tests)
add_test(NAME title-bars COMMAND recon_titlebar_tests)
add_test(NAME widgets COMMAND recon_widget_tests)
add_test(NAME error-catalogue COMMAND recon_error_tests)

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
# thing under test rather than something the test can arrange -- so it is not
# built at all under RECONOS_TESTS_ONLY, where by definition there is none. It
# also needs the protocol generator, which is part of the compositor half.

if(NOT RECONOS_TESTS_ONLY)

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
    "${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h"
    "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/recon-theme-client-protocol.h")

target_include_directories(decor_client PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${WAYLAND_CLIENT_INCLUDE_DIRS})
target_link_libraries(decor_client PRIVATE ${WAYLAND_CLIENT_LIBRARIES} rt)

endif()   # NOT RECONOS_TESTS_ONLY
