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

# Find Qt6 (fallback to Qt5). PrintSupport is required by the vendored QCustomPlot.
# OpenGL/OpenGLWidgets let QCustomPlot rasterize on the GPU — software QPainter
# rendering of the Speed/RPM/ERS chart costs ~100 ms/frame (and seconds/frame at
# a 10-min window), which GPU rendering cuts to single-digit ms. See setOpenGl().
find_package(Qt6 QUIET COMPONENTS Core Widgets Network Svg PrintSupport OpenGL OpenGLWidgets)
if(Qt6_FOUND)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::PrintSupport
                Qt6::OpenGL Qt6::OpenGLWidgets)
    set(QCP_OPENGL ON)
    message(STATUS "Using Qt6 (QCustomPlot OpenGL rendering enabled)")
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network Svg PrintSupport)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Network Qt5::Svg Qt5::PrintSupport)
    message(STATUS "Using Qt5 (Qt6 not found; software chart rendering)")
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/TnrdPlayer.cpp
    src/TnrdPlayer.h
    src/SessionModel.cpp
    src/SessionModel.h
    src/TelemetryChart.cpp
    src/TelemetryChart.h
    src/components/ChartView.cpp
    src/components/ChartView.h
    src/third_party/qcustomplot/qcustomplot.cpp
    src/third_party/qcustomplot/qcustomplot.h
    src/components/OverviewLayout.h
    src/components/OverviewLayout.cpp
    src/components/EditOverviewLayoutDialog.h
    src/components/EditOverviewLayoutDialog.cpp
    src/components/OverviewPage.cpp
    src/components/SettingsDialog.h
    src/components/SettingsDialog.cpp
    src/components/TyresPage.cpp
    src/components/StandingsPage.cpp
    src/components/SessionPage.cpp
    src/components/TrackMapPage.cpp
    src/components/TrackMapWidget.cpp
    src/components/TrackMapWidget.h
    src/protocols/protocol.cpp
    src/protocols/f1_24.cpp
    src/protocols/f1_25.cpp
    resources.qrc
    maps.qrc
)

if(WIN32)
    list(APPEND SOURCES resources.rc)
    add_executable(native_recorder WIN32 ${SOURCES})
    target_compile_options(native_recorder PRIVATE /utf-8 /FI"windows.h")
    # NOMINMAX prevents windows.h min/max macros from shadowing std::min/std::max.
    # The /FI force-include ensures tagMSG is fully defined before Qt forward-declares
    # it, so nlohmann::json's type-trait templates compile cleanly under MSVC.
    target_compile_definitions(native_recorder PRIVATE NOMINMAX)
else()
    add_executable(native_recorder ${SOURCES})
endif()

set_target_properties(native_recorder PROPERTIES OUTPUT_NAME "Track N Race Background Recorder")
target_link_libraries(native_recorder PRIVATE nlohmann_json::nlohmann_json zlibstatic ${QT_LIBS})

# GPU rendering for QCustomPlot (Qt6 only). The macro must reach qcustomplot.cpp,
# so define it on the whole target. OpenGL::GL provides the GL symbols the
# QCustomPlot OpenGL paint path references directly.
if(QCP_OPENGL)
    find_package(OpenGL REQUIRED)
    target_compile_definitions(native_recorder PRIVATE QCUSTOMPLOT_USE_OPENGL)
    target_link_libraries(native_recorder PRIVATE OpenGL::GL)
endif()
