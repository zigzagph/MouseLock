cmake_minimum_required(VERSION 3.15)
project(CursorLocker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32 = build as a GUI app (uses wWinMain, no console window)
add_executable(CursorLocker WIN32
    CursorLocker.cpp
    CursorLocker.rc
    CursorLocker.manifest
)

# Unicode everywhere
target_compile_definitions(CursorLocker PRIVATE UNICODE _UNICODE)

# MSVC: embed manifest, link required libs
if(MSVC)
    target_link_libraries(CursorLocker PRIVATE user32 gdi32 shell32)
    set_target_properties(CursorLocker PROPERTIES
        LINK_FLAGS "/MANIFEST:NO")  # we embed our own manifest below
endif()

# MinGW
if(MINGW)
    target_link_libraries(CursorLocker PRIVATE user32 gdi32 shell32)
    target_link_options(CursorLocker PRIVATE -municode -mwindows)
endif()
