# Native keyboard and mouse input behind the SDK's WPAD/KPAD API.
#
# Root integration:   add_subdirectory(native/input)   (after native/platform)
# Standalone build:   cmake -S native/input -B build/input -DPETARI_SANITIZERS=ON
#                     cmake --build build/input && ctest --test-dir build/input
cmake_minimum_required(VERSION 3.25)

set(_petari_input_standalone OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_petari_input_standalone ON)
    project(PetariNativeInput LANGUAGES C CXX)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
    include(CTest)
endif()

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(_petari_input_standalone)
    # The root build's shared targets, recreated for a standalone build.
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(PETARI_LEGACY_ROOT "${CMAKE_BINARY_DIR}/legacy-sjis")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/tools/encode_legacy_sources.py"
            --root "${PETARI_REPO_ROOT}" --output "${PETARI_LEGACY_ROOT}"
        RESULT_VARIABLE _petari_encoding_result)
    if(NOT _petari_encoding_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare Shift-JIS legacy sources")
    endif()
    # Regenerate the copies when sources change, as the root build does.
    file(GLOB_RECURSE _petari_legacy_inputs CONFIGURE_DEPENDS
        "${PETARI_REPO_ROOT}/src/*.c" "${PETARI_REPO_ROOT}/src/*.cpp" "${PETARI_REPO_ROOT}/src/*.h"
        "${PETARI_REPO_ROOT}/include/*.h" "${PETARI_REPO_ROOT}/include/*.hpp"
        "${PETARI_REPO_ROOT}/libs/*.h" "${PETARI_REPO_ROOT}/libs/*.hpp")
    set(_petari_encoding_stamp "${PETARI_LEGACY_ROOT}/.prepared")
    add_custom_command(OUTPUT "${_petari_encoding_stamp}"
        COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/tools/encode_legacy_sources.py"
            --root "${PETARI_REPO_ROOT}" --output "${PETARI_LEGACY_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_petari_encoding_stamp}"
        DEPENDS "${PETARI_REPO_ROOT}/native/tools/encode_legacy_sources.py" ${_petari_legacy_inputs}
        COMMENT "Updating Shift-JIS literals in generated native source inputs")
    add_custom_target(petari_legacy_sources DEPENDS "${_petari_encoding_stamp}")
    function(petari_encode_target_sources target)
        get_target_property(_sources "${target}" SOURCES)
        set(_encoded)
        foreach(_source IN LISTS _sources)
            set(_relative "")
            if(IS_ABSOLUTE "${_source}")
                file(RELATIVE_PATH _relative "${PETARI_REPO_ROOT}" "${_source}")
            endif()
            if(_relative MATCHES "^src/")
                list(APPEND _encoded "${PETARI_LEGACY_ROOT}/${_relative}")
            else()
                list(APPEND _encoded "${_source}")
            endif()
        endforeach()
        set_property(TARGET "${target}" PROPERTY SOURCES "${_encoded}")
        add_dependencies("${target}" petari_legacy_sources)
    endfunction()

    add_library(petari_native_config INTERFACE)
    target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
    target_include_directories(petari_native_config INTERFACE
        "${PETARI_LEGACY_ROOT}/include" "${PETARI_LEGACY_ROOT}/libs/RVL_SDK/include"
        "${PETARI_LEGACY_ROOT}/libs/JSystem/include" "${PETARI_LEGACY_ROOT}/libs/nw4r/include"
        ${PETARI_REPO_ROOT}/native/include ${PETARI_REPO_ROOT}/include
        ${PETARI_REPO_ROOT}/libs/RVL_SDK/include ${PETARI_REPO_ROOT}/libs/JSystem/include
        ${PETARI_REPO_ROOT}/libs/nw4r/include)
    target_compile_options(petari_native_config INTERFACE
        -fno-strict-aliasing -ffp-contract=off -fcheck-new -Werror=return-type -Wno-register)
    if(PETARI_SANITIZERS)
        target_compile_options(petari_native_config INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(petari_native_config INTERFACE -fsanitize=address,undefined)
    endif()

    add_library(petari_host_runtime STATIC
        ${PETARI_REPO_ROOT}/native/src/host_allocation.cpp ${PETARI_REPO_ROOT}/native/text/utf16.cpp)
    target_link_libraries(petari_host_runtime PUBLIC petari_native_config)
    add_library(petari_memory STATIC
        ${PETARI_REPO_ROOT}/native/memory/address_api.cpp ${PETARI_REPO_ROOT}/native/memory/arena.cpp
        ${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c)
    set_source_files_properties(${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c PROPERTIES LANGUAGE CXX)
    target_link_libraries(petari_memory PUBLIC petari_native_config petari_host_runtime)
    add_library(petari_core STATIC
        ${PETARI_REPO_ROOT}/native/src/mtx.cpp ${PETARI_REPO_ROOT}/native/src/intrinsics.cpp
        ${PETARI_REPO_ROOT}/native/src/jmath.cpp
        ${PETARI_REPO_ROOT}/src/JSystem/JMath/JMath.cpp ${PETARI_REPO_ROOT}/src/JSystem/JMath/JMATrigonometric.cpp)
    petari_encode_target_sources(petari_core)
    target_link_libraries(petari_core PUBLIC petari_native_config)
    set(_petari_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    add_subdirectory(${PETARI_REPO_ROOT}/native/platform ${CMAKE_BINARY_DIR}/platform)
    set(BUILD_TESTING "${_petari_testing}")
endif()

# The SDK's KPAD.c, compiled unchanged. It relies on memcpy and memset
# without including a header, which CodeWarrior's prefix file provided.
set(PETARI_KPAD_SOURCE ${PETARI_REPO_ROOT}/src/RVL_SDK/kpad/KPAD.c)
add_library(petari_input STATIC
    bindings.cpp
    remote_model.cpp
    wpad_host.cpp
    ${PETARI_KPAD_SOURCE})
petari_encode_target_sources(petari_input)
set_source_files_properties("${PETARI_LEGACY_ROOT}/src/RVL_SDK/kpad/KPAD.c" PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-include;cstring;-Wno-unknown-pragmas")
target_include_directories(petari_input PUBLIC include)
target_link_libraries(petari_input PUBLIC petari_platform_os petari_platform_sc petari_core petari_native_config petari_host_runtime)
target_compile_features(petari_input PUBLIC cxx_std_17)

# SDL3 event adapter (header-only; needs SDL3's headers, not its library).
# Linked by the application together with its own SDL3 target.
add_library(petari_input_sdl3 INTERFACE)
target_include_directories(petari_input_sdl3 INTERFACE include)
target_link_libraries(petari_input_sdl3 INTERFACE petari_input)
set(PETARI_SDL3_INCLUDE_DIR "" CACHE PATH "SDL3 include directory for the input adapter test (optional)")
if(NOT PETARI_SDL3_INCLUDE_DIR AND EXISTS "${PETARI_REPO_ROOT}/build/aurora/_deps/sdl-src/include/SDL3/SDL_events.h")
    set(PETARI_SDL3_INCLUDE_DIR "${PETARI_REPO_ROOT}/build/aurora/_deps/sdl-src/include")
endif()

if(BUILD_TESTING)
    # The game's own Wii Remote classes read the emulated KPAD output, so the
    # tests check what gameplay sees. Dead stripping drops the parts of these
    # files that reach the rest of the game.
    set(PETARI_INPUT_GAME_SOURCES
        ${PETARI_REPO_ROOT}/src/Game/System/WPad.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadAcceleration.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadButton.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadHVSwing.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadHolder.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadInfoChecker.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadLeaveWatcher.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadPointer.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadRumble.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadRumbleData.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/WPadStick.cpp
        ${PETARI_REPO_ROOT}/src/Game/Util/GamePadUtil.cpp
        ${PETARI_REPO_ROOT}/src/Game/Util/HashUtil.cpp
        ${PETARI_REPO_ROOT}/src/Game/Util/MemoryUtil.cpp
        ${PETARI_REPO_ROOT}/src/Game/Util/TriggerChecker.cpp)
    add_executable(petari_input_tests ${PETARI_REPO_ROOT}/native/tests/input_tests.cpp ${PETARI_INPUT_GAME_SOURCES})
    petari_encode_target_sources(petari_input_tests)
    target_include_directories(petari_input_tests PRIVATE ${PETARI_REPO_ROOT})
    target_compile_options(petari_input_tests PRIVATE -Wno-inconsistent-missing-override)
    target_link_options(petari_input_tests PRIVATE -Wl,-dead_strip)
    target_link_libraries(petari_input_tests PRIVATE petari_input)
    if(PETARI_SDL3_INCLUDE_DIR)
        target_include_directories(petari_input_tests PRIVATE ${PETARI_SDL3_INCLUDE_DIR})
        target_compile_definitions(petari_input_tests PRIVATE PETARI_INPUT_TEST_SDL3=1)
    endif()
    add_test(NAME native_input COMMAND petari_input_tests)
endif()
