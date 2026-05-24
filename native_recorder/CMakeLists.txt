cmake_minimum_required(VERSION 3.15)
project(NativeRecorder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(native_recorder WIN32 src/main.cpp)
target_link_libraries(native_recorder PRIVATE nlohmann_json::nlohmann_json zlibstatic Ws2_32 Comdlg32)
