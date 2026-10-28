cmake_minimum_required(VERSION 3.20)
project(ProjectAtlas VERSION 0.11.1 LANGUAGES CXX)

set(LIBCLANG_ROOT "" CACHE PATH "Root of an LLVM installation with libclang")
if(NOT LIBCLANG_ROOT)
    set(_local_llvm
        "${CMAKE_CURRENT_SOURCE_DIR}/../.vendor/llvm23/clang+llvm-23.1.3-x86_64-pc-windows-msvc")
    if(EXISTS "${_local_llvm}/include/clang-c/Index.h")
        set(LIBCLANG_ROOT "${_local_llvm}")
    endif()
endif()
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
    HINTS "${LIBCLANG_ROOT}/include" REQUIRED)
find_library(LIBCLANG_LIBRARY NAMES libclang clang
    HINTS "${LIBCLANG_ROOT}/lib" REQUIRED)
if(WIN32)
    find_file(LIBCLANG_DLL libclang.dll HINTS "${LIBCLANG_ROOT}/bin" REQUIRED)
endif()

add_library(project_atlas_core STATIC
    src/scan.cpp
    src/uml.cpp
    src/interactions.cpp
    src/interaction_presentation.cpp
    src/context.cpp
    src/agent_tools.cpp
    src/report.cpp
    src/card_archive.cpp
    src/facts.cpp
    src/agent.cpp
    src/trace.cpp
    src/runtime.cpp
    src/capture.cpp
    src/ui_control.cpp
    src/debugger.cpp
)
target_include_directories(project_atlas_core PUBLIC include)
target_include_directories(project_atlas_core PRIVATE "${LIBCLANG_INCLUDE_DIR}")
target_compile_features(project_atlas_core PUBLIC cxx_std_23)
target_link_libraries(project_atlas_core PUBLIC "${LIBCLANG_LIBRARY}")
if(WIN32)
    target_link_libraries(project_atlas_core PUBLIC dbghelp user32 advapi32)
endif()
set_target_properties(project_atlas_core PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(project_atlas_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(project_atlas_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(project_atlas src/main.cpp)
target_link_libraries(project_atlas PRIVATE project_atlas_core)
if(WIN32)
    target_link_libraries(project_atlas PRIVATE shell32)
endif()

function(copy_libclang target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LIBCLANG_DLL}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
copy_libclang(project_atlas)

# Opt-in for the Atlas development checkout: refresh adjacent agent cards after
# source edits without making ordinary consumer/CI builds write into source.
option(ATLAS_SELF_HOST_CARDS "Refresh this checkout's agent cards during builds" OFF)
if(ATLAS_SELF_HOST_CARDS)
    file(GLOB_RECURSE ATLAS_CARD_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    set(ATLAS_CARD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/atlas-cards.stamp")
    add_custom_command(OUTPUT "${ATLAS_CARD_STAMP}"
        COMMAND $<TARGET_FILE:project_atlas> scan-in-place
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/.project-atlas/generated"
            "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${ATLAS_CARD_STAMP}"
        DEPENDS project_atlas ${ATLAS_CARD_INPUTS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Refreshing Atlas self-hosting agent cards"
        VERBATIM)
    add_custom_target(atlas_self_cards ALL DEPENDS "${ATLAS_CARD_STAMP}")
endif()

if(WIN32)
    # Add this object file to a C++ executable to capture its ordinary
    # new/delete events without editing the executable's source code.
    add_library(project_atlas_auto_runtime OBJECT src/auto_runtime.cpp)
    target_compile_features(project_atlas_auto_runtime PUBLIC cxx_std_23)
    target_compile_definitions(project_atlas_auto_runtime PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_executable(project_atlas_auto_demo examples/auto_runtime_demo.cpp
        $<TARGET_OBJECTS:project_atlas_auto_runtime>)
    target_compile_features(project_atlas_auto_demo PRIVATE cxx_std_23)

    add_executable(project_atlas_debug_demo examples/debugger_demo.cpp)
    target_compile_features(project_atlas_debug_demo PRIVATE cxx_std_23)
    add_executable(project_atlas_debug_threads_demo examples/debugger_threads_demo.cpp)
    target_compile_features(project_atlas_debug_threads_demo PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(project_atlas_debug_demo PRIVATE /Od /Zi)
        target_link_options(project_atlas_debug_demo PRIVATE /DEBUG)
        target_compile_options(project_atlas_debug_threads_demo PRIVATE /Od /Zi)
        target_link_options(project_atlas_debug_threads_demo PRIVATE /DEBUG)
    endif()

    add_executable(project_atlas_gui WIN32 src/gui.cpp)
    target_link_libraries(project_atlas_gui PRIVATE
        project_atlas_core comctl32 ole32 shell32 gdi32 user32)
    target_compile_definitions(project_atlas_gui PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    copy_libclang(project_atlas_gui)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(project_atlas_tests tests/tests.cpp)
    target_link_libraries(project_atlas_tests PRIVATE project_atlas_core)
    copy_libclang(project_atlas_tests)
    add_test(NAME project_atlas_tests COMMAND project_atlas_tests)
    if(WIN32)
        target_compile_definitions(project_atlas_tests PRIVATE
            ATLAS_AUTO_DEMO_PATH="$<TARGET_FILE:project_atlas_auto_demo>"
            ATLAS_DEBUG_DEMO_PATH="$<TARGET_FILE:project_atlas_debug_demo>"
            ATLAS_DEBUG_DEMO_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/examples/debugger_demo.cpp"
            ATLAS_DEBUG_THREADS_DEMO_PATH="$<TARGET_FILE:project_atlas_debug_threads_demo>"
            ATLAS_DEBUG_THREADS_DEMO_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/examples/debugger_threads_demo.cpp"
            ATLAS_CLI_PATH="$<TARGET_FILE:project_atlas>")
        add_test(NAME project_atlas_gui_smoke
            COMMAND project_atlas_gui --smoke
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/memory_expected.facts"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/memory_actual.facts"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/runtime_memory.trace")
    endif()
endif()
