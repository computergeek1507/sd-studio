cmake_minimum_required(VERSION 3.21)
project(sd_studio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point at the installed Qt 6 kit. On CI (and other machines) pass
# -DCMAKE_PREFIX_PATH=<qt>; locally fall back to a common install path only if
# it actually exists, so this doesn't break when Qt lives elsewhere.
if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "D:/Qt/6.8.3/msvc2022_64")
    set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(sd_studio WIN32 sd_studio.cpp icon.qrc)
if(WIN32)
    target_sources(sd_studio PRIVATE app.rc)   # embeds sd_studio.ico as the exe icon
endif()
target_link_libraries(sd_studio PRIVATE Qt6::Widgets Qt6::Network)
