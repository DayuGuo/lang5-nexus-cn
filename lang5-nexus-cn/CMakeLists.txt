cmake_minimum_required(VERSION 3.20)

project(Lang5NexusCn LANGUAGES CXX)

option(LANG5_ENABLE_UNSAFE_LANGUAGE_CALL "Allow the addon to call the discovered GW2 language setter." OFF)
option(LANG5_ENABLE_DIRECT_SETTER_CALL "Call the discovered GW2 language setter instead of only writing the language value." OFF)
option(LANG5_ENABLE_MEMORY_WRITE "Write the resolved GW2 language value directly. This crashed in CtlDropList testing." OFF)
option(LANG5_ENABLE_DEFERRED_CALLER_HOOK "Install a ViewAdvanceText caller hook and defer the language setter call into that game path." OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Lang5NexusCn must be built for Windows x64.")
endif()

add_library(Lang5NexusCn SHARED
    src/ModuleMain.cpp
)

target_compile_features(Lang5NexusCn PRIVATE cxx_std_20)
target_include_directories(Lang5NexusCn PRIVATE include)

target_compile_definitions(Lang5NexusCn PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(LANG5_ENABLE_UNSAFE_LANGUAGE_CALL)
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_UNSAFE_LANGUAGE_CALL=1)
else()
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_UNSAFE_LANGUAGE_CALL=0)
endif()

if(LANG5_ENABLE_DIRECT_SETTER_CALL)
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_DIRECT_SETTER_CALL=1)
else()
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_DIRECT_SETTER_CALL=0)
endif()

if(LANG5_ENABLE_MEMORY_WRITE)
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_MEMORY_WRITE=1)
else()
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_MEMORY_WRITE=0)
endif()

if(LANG5_ENABLE_DEFERRED_CALLER_HOOK)
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_DEFERRED_CALLER_HOOK=1)
else()
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_DEFERRED_CALLER_HOOK=0)
endif()

if(MSVC)
    target_compile_options(Lang5NexusCn PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(Lang5NexusCn PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(Lang5NexusCn PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(Lang5NexusCn PRIVATE psapi)

set_target_properties(Lang5NexusCn PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Lang5NexusCn"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
