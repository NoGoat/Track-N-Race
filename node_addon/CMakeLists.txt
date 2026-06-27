cmake_minimum_required(VERSION 3.16)
project(protocol_parser_addon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Make sure it builds as a node addon (.node)
add_definitions(-DNAPI_VERSION=6)

# Force Position Independent Code (-fPIC) so libtnrp.a can be linked into a shared .node module
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# Define tnrp library without Qt
set(TNRP_USE_QT OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol_parser_library ${CMAKE_BINARY_DIR}/tnrp_build)

# Ensure cmake-js provides CMAKE_JS_INC and CMAKE_JS_LIB
include_directories(${CMAKE_JS_INC})

# Get node-addon-api path
execute_process(COMMAND node -p "require('node-addon-api').include"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE NODE_ADDON_API_DIR
)
string(REPLACE "\n" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")
string(REPLACE "\"" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")

file(GLOB SOURCE_FILES "addon.cpp")

add_library(protocol_parser MODULE ${SOURCE_FILES})

# Link node-addon-api and Node itself
target_include_directories(protocol_parser PRIVATE ${CMAKE_JS_INC} ${NODE_ADDON_API_DIR})
target_link_libraries(protocol_parser PRIVATE ${CMAKE_JS_LIB} tnrp)

# We want the output to be protocol_parser.node
set_target_properties(protocol_parser PROPERTIES PREFIX "" SUFFIX ".node")

if(NOT MSVC)
    target_compile_options(protocol_parser PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
else()
    target_compile_options(protocol_parser PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
    target_compile_definitions(protocol_parser PRIVATE NOMINMAX)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_out)
if(_ipo_ok)
    set_property(TARGET tnrp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET protocol_parser PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
