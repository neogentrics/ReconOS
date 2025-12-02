cmake_minimum_required (VERSION 3.8)

# 1. Project Name
project ("ReconOS")

# 2. We are writing C code
enable_language(C)
set(CMAKE_C_STANDARD 11)

# 3. Tell wlroots we are doing unstable dev (Required!)
add_definitions(-DWLR_USE_UNSTABLE)

# 4. Find the libraries on the Linux VM
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLR REQUIRED wlroots)
pkg_check_modules(WAYLAND REQUIRED wayland-server)
pkg_check_modules(XKB REQUIRED xkbcommon)

# 5. Define the Executable
add_executable (ReconOS "main.c")

# 6. Link the libraries to our executable
target_link_libraries(ReconOS PRIVATE ${WLR_LIBRARIES} ${WAYLAND_LIBRARIES} ${XKB_LIBRARIES})
target_include_directories(ReconOS PRIVATE ${WLR_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS} ${XKB_INCLUDE_DIRS})