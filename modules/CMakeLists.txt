# ReconOS modules.
#
# Each one builds to a shared object with a ReconOS extension: .rex for an
# application, .rts for a system module. They are not linked against ReconOS --
# they resolve its symbols at load time from the executable, which is why the
# main target sets ENABLE_EXPORTS.
#
# Undefined symbols are therefore expected at link time and only checked when
# the module is loaded. RTLD_NOW in the loader is what turns "a symbol ReconOS
# does not have" into a refusal at load rather than a crash later.

function(recon_add_module NAME EXTENSION)
    add_library(${NAME} MODULE ${ARGN})

    set_target_properties(${NAME} PROPERTIES
        PREFIX ""
        SUFFIX "${EXTENSION}"
        # Collected in one place so the loader has a single directory to
        # install from on first run.
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
        # Only the descriptor needs to be visible; everything else is the
        # module's own business.
        C_VISIBILITY_PRESET hidden)

    target_include_directories(${NAME} PRIVATE
        ${WLR_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
        ${XKB_INCLUDE_DIRS}
        ${PIXMAN_INCLUDE_DIRS}
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}")

    # Resolved against ReconOS when loaded, not now.
    target_link_options(${NAME} PRIVATE -Wl,--allow-shlib-undefined)

    add_dependencies(ReconOS ${NAME})
endfunction()

recon_add_module(Calculator ".rex" calculator/recon_calc.c)

# --- The codec pack ---
#
# Decoders ReconOS does not write, in a module it can do without. Built only
# where libavcodec is present, and the desktop is complete without it: it plays
# WAV and MP3 either way and says which decoder is missing when it cannot play
# something.
#
# A module rather than part of the core so that somebody who never plays video
# does not carry the dependency, and so that it can be taken off again -- a
# codec pack that cannot be removed is not a pack.
pkg_check_modules(AVCODEC libavcodec libavutil)

if(AVCODEC_FOUND)
    message(STATUS "libavcodec found (${AVCODEC_libavcodec_VERSION}); "
                   "building the codec pack")
    recon_add_module(CodecPack ".rts" codecpack/recon_codecpack.c)
    target_include_directories(CodecPack PRIVATE ${AVCODEC_INCLUDE_DIRS})
    target_link_libraries(CodecPack PRIVATE ${AVCODEC_LIBRARIES})
else()
    message(STATUS "libavcodec not found; the codec pack will not be built, "
                   "and ReconOS will play only what it decodes itself")
endif()
