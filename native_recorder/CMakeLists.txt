cmake_minimum_required(VERSION 3.16)
project(NativeRecorder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(FetchContent)

# Fetch nlohmann/json for JSON serialization
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Fetch zlib for .tnrd gzip streaming
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(zlib)

# Find Qt6 (fallback to Qt5)
find_package(Qt6 QUIET COMPONENTS Core Widgets Network Svg Charts)
if(Qt6_FOUND)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Charts)
    message(STATUS "Using Qt6")
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network Svg Charts)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Network Qt5::Svg Qt5::Charts)
    message(STATUS "Using Qt5 (Qt6 not found)")
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/TnrdPlayer.cpp
    src/TnrdPlayer.h
    src/TelemetryChart.cpp
    src/TelemetryChart.h
    src/components/OverviewPage.cpp
    src/components/SettingsPage.cpp
    src/components/TyresPage.cpp
    src/components/StandingsPage.cpp
    src/components/SessionPage.cpp
    src/protocols/protocol.cpp
    src/protocols/f1_24.cpp
    src/protocols/f1_25.cpp
    resources.qrc
)

if(WIN32)
    list(APPEND SOURCES resources.rc)
    add_executable(native_recorder WIN32 ${SOURCES})
    target_compile_options(native_recorder PRIVATE /utf-8)
else()
    add_executable(native_recorder ${SOURCES})
endif()

set_target_properties(native_recorder PROPERTIES OUTPUT_NAME "Track N Race Background Recorder")
target_link_libraries(native_recorder PRIVATE nlohmann_json::nlohmann_json zlibstatic ${QT_LIBS})
