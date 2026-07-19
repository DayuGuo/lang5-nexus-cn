cmake_minimum_required(VERSION 3.20)

project(Lang5NexusCn LANGUAGES CXX)

option(LANG5_ENABLE_UNSAFE_LANGUAGE_CALL "Allow the addon to call the discovered GW2 language setter." OFF)
option(LANG5_ENABLE_DIRECT_SETTER_CALL "Call the discovered GW2 language setter instead of only writing the language value." OFF)
option(LANG5_ENABLE_MEMORY_WRITE "Write the resolved GW2 language value directly. This crashed in CtlDropList testing." OFF)
option(LANG5_ENABLE_DEFERRED_CALLER_HOOK "Install a ViewAdvanceText caller hook and defer the language setter call into that game path." OFF)
option(LANG5_ENABLE_TEXT_CONVERSION "Install the CParser::Validate hook that converts simplified Chinese UI text to traditional." OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Lang5NexusCn must be built for Windows x64.")
endif()

# Dear ImGui 1.80, vendored verbatim from the Nexus source tree so the struct
# layouts match the Nexus-hosted ImGui context exactly (required for ABI safety).
# Built as a separate library so its third-party warnings do not pollute our target.
add_library(imgui STATIC
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/imgui_tables.cpp
)
target_include_directories(imgui PUBLIC thirdparty/imgui)
target_compile_features(imgui PRIVATE cxx_std_17)
target_link_libraries(imgui PUBLIC imm32 gdi32 user32 dwmapi)
if(MSVC)
    target_compile_options(imgui PRIVATE /w)
else()
    target_compile_options(imgui PRIVATE -w)
endif()

# Embed the base simplified-to-traditional dictionary into the DLL so players
# only need to copy a single file. data/jianfan.json remains the source of truth.
set(LANG5_EMBED_HEADER "${CMAKE_BINARY_DIR}/generated/jianfan_embedded.h")
add_custom_command(
    OUTPUT "${LANG5_EMBED_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_SOURCE_DIR}/data/jianfan.json
        -DOUTPUT=${LANG5_EMBED_HEADER}
        -DSYMBOL=kEmbeddedJianfanJson
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS data/jianfan.json cmake/EmbedFile.cmake
    COMMENT "Embedding data/jianfan.json into jianfan_embedded.h"
)

add_library(Lang5NexusCn SHARED
    src/ModuleMain.cpp
    src/TextConverter.cpp
    "${LANG5_EMBED_HEADER}"
)

target_compile_features(Lang5NexusCn PRIVATE cxx_std_20)
target_include_directories(Lang5NexusCn PRIVATE include "${CMAKE_BINARY_DIR}/generated")

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

if(LANG5_ENABLE_TEXT_CONVERSION)
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_TEXT_CONVERSION=1)
else()
    target_compile_definitions(Lang5NexusCn PRIVATE LANG5_ENABLE_TEXT_CONVERSION=0)
endif()

if(MSVC)
    target_compile_options(Lang5NexusCn PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(Lang5NexusCn PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(Lang5NexusCn PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(Lang5NexusCn PRIVATE psapi imgui)

set_target_properties(Lang5NexusCn PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Lang5NexusCn"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
