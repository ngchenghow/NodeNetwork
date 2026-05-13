cmake_minimum_required(VERSION 3.16)
project(NodeNetwork CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# SDL2 + SDL2_ttf. Try CMake config packages first; fall back to pkg-config /
# manual find for MSYS2-style installs.
find_package(SDL2 CONFIG QUIET)
find_package(SDL2_ttf CONFIG QUIET)

if(NOT SDL2_FOUND)
    find_package(SDL2 REQUIRED)
endif()
if(NOT SDL2_ttf_FOUND)
    find_package(SDL2_ttf QUIET)
endif()

add_executable(NodeNetwork src/main.cpp)

if(TARGET SDL2::SDL2main)
    target_link_libraries(NodeNetwork PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2)
    target_link_libraries(NodeNetwork PRIVATE SDL2::SDL2)
else()
    target_include_directories(NodeNetwork PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(NodeNetwork PRIVATE ${SDL2_LIBRARIES})
endif()

if(TARGET SDL2_ttf::SDL2_ttf)
    target_link_libraries(NodeNetwork PRIVATE SDL2_ttf::SDL2_ttf)
elseif(TARGET SDL2_ttf::SDL2_ttf-static)
    target_link_libraries(NodeNetwork PRIVATE SDL2_ttf::SDL2_ttf-static)
else()
    # Last-resort manual link (works for MSYS2 mingw64).
    find_library(SDL2_TTF_LIB NAMES SDL2_ttf)
    find_path(SDL2_TTF_INC SDL_ttf.h PATH_SUFFIXES SDL2)
    if(SDL2_TTF_LIB AND SDL2_TTF_INC)
        target_include_directories(NodeNetwork PRIVATE ${SDL2_TTF_INC})
        target_link_libraries(NodeNetwork PRIVATE ${SDL2_TTF_LIB})
    else()
        message(FATAL_ERROR "SDL2_ttf not found. Install libsdl2-ttf-dev / mingw-w64-x86_64-SDL2_ttf.")
    endif()
endif()

# Copy the assets folder next to the binary so it can find graph.txt / font.ttf.
add_custom_command(TARGET NodeNetwork POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:NodeNetwork>/assets")

# On MinGW builds, gather the runtime DLLs next to the exe so it can be
# launched from Explorer or shipped without depending on PATH containing
# C:\msys64\mingw64\bin. Set NODENETWORK_GATHER_DLLS=OFF to skip.
option(NODENETWORK_GATHER_DLLS "Copy MinGW runtime DLLs next to the binary" ON)
if(WIN32 AND MINGW AND NODENETWORK_GATHER_DLLS)
    find_program(POWERSHELL_EXE NAMES pwsh.exe powershell.exe)
    if(POWERSHELL_EXE)
        add_custom_command(TARGET NodeNetwork POST_BUILD
            COMMAND ${POWERSHELL_EXE} -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/scripts/gather_dlls.ps1"
                    -Exe "$<TARGET_FILE:NodeNetwork>"
            COMMENT "Gathering MinGW runtime DLLs"
            VERBATIM)
    else()
        message(WARNING "PowerShell not found; skipping DLL gather. "
                        "Run scripts/gather_dlls.ps1 manually.")
    endif()
endif()
