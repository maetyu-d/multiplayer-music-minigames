cmake_minimum_required(VERSION 3.16)
project(MultiplayerMusicGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(multiplayer_music
    src/main.cpp
    src/JuceAudioEngine.cpp
)
include(CheckIncludeFileCXX)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(SDL2_PC QUIET sdl2)
endif()

if (TARGET SDL2::SDL2)
    target_link_libraries(multiplayer_music PRIVATE SDL2::SDL2)
elseif (SDL2_PC_FOUND)
    target_include_directories(multiplayer_music PRIVATE ${SDL2_PC_INCLUDE_DIRS})
    target_link_directories(multiplayer_music PRIVATE ${SDL2_PC_LIBRARY_DIRS})
    target_link_libraries(multiplayer_music PRIVATE ${SDL2_PC_LIBRARIES})
else()
    find_path(SDL2_INCLUDE_DIR
        NAMES SDL.h
        PATH_SUFFIXES SDL2
        PATHS
            /opt/homebrew/include
            /usr/local/include
            /Library/Frameworks/SDL2.framework/Headers
    )
    find_library(SDL2_LIBRARY
        NAMES SDL2
        PATHS
            /opt/homebrew/lib
            /usr/local/lib
            /Library/Frameworks
    )

    if (NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
        message(FATAL_ERROR "SDL2 not found. Install SDL2 dev package or set SDL2 include/lib paths.")
    endif()
    target_include_directories(multiplayer_music PRIVATE ${SDL2_INCLUDE_DIR})
    target_link_libraries(multiplayer_music PRIVATE ${SDL2_LIBRARY})
endif()

set(JUCE_DIR "$ENV{HOME}/JUCE" CACHE PATH "Path to JUCE repository")
if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_DIR}. Set -DJUCE_DIR=/path/to/JUCE")
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
target_link_libraries(multiplayer_music PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_events
)
target_compile_definitions(multiplayer_music PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
