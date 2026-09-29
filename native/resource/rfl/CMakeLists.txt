# Native RVLFaceLib (Miis) support: big-endian resource access, the packed
# Mii record codec, and RFL_Res.dat validation.
#
# Root integration:   add_subdirectory(native/resource/rfl)
# Standalone build:   cmake -S native/resource/rfl -B build/rfl -DPETARI_SANITIZERS=ON
#                     cmake --build build/rfl && ctest --test-dir build/rfl
cmake_minimum_required(VERSION 3.25)

set(_petari_rfl_standalone OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_petari_rfl_standalone ON)
    project(PetariNativeRFL LANGUAGES C CXX)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
    include(CTest)
endif()

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

if(_petari_rfl_standalone)
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
        -fno-strict-aliasing -ffp-contract=off $<$<COMPILE_LANGUAGE:CXX>:-fcheck-new> -Werror=return-type
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>)
    if(PETARI_SANITIZERS)
        target_compile_options(petari_native_config INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(petari_native_config INTERFACE -fsanitize=address,undefined)
    endif()
    add_library(petari_resources STATIC ${PETARI_REPO_ROOT}/native/resource/archive.cpp)
    target_include_directories(petari_resources PUBLIC ${PETARI_REPO_ROOT}/native/resource)
    target_link_libraries(petari_resources PUBLIC petari_native_config)
    add_library(petari_core STATIC
        ${PETARI_REPO_ROOT}/native/src/mtx.cpp ${PETARI_REPO_ROOT}/native/src/intrinsics.cpp
        ${PETARI_REPO_ROOT}/native/src/jmath.cpp
        ${PETARI_REPO_ROOT}/src/JSystem/JMath/JMath.cpp ${PETARI_REPO_ROOT}/src/JSystem/JMath/JMATrigonometric.cpp)
    petari_encode_target_sources(petari_core)
    target_link_libraries(petari_core PUBLIC petari_native_config)
    add_library(petari_host_runtime STATIC
        ${PETARI_REPO_ROOT}/native/src/host_allocation.cpp ${PETARI_REPO_ROOT}/native/text/utf16.cpp)
    target_link_libraries(petari_host_runtime PUBLIC petari_native_config)
    add_library(petari_memory STATIC
        ${PETARI_REPO_ROOT}/native/memory/address_api.cpp ${PETARI_REPO_ROOT}/native/memory/arena.cpp
        ${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c)
    set_source_files_properties(${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c PROPERTIES LANGUAGE CXX)
    target_link_libraries(petari_memory PUBLIC petari_native_config petari_host_runtime)
    set(_petari_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    add_subdirectory(${PETARI_REPO_ROOT}/native/platform ${CMAKE_BINARY_DIR}/platform)
    add_subdirectory(${PETARI_REPO_ROOT}/native/memory/sdk_mem ${CMAKE_BINARY_DIR}/sdk_mem)
    set(BUILD_TESTING "${_petari_testing}")
endif()

# RVLFaceLib headers; the generated copies convert legacy wide characters.
add_library(petari_rfl_headers INTERFACE)
target_include_directories(petari_rfl_headers INTERFACE
    "${PETARI_LEGACY_ROOT}/libs/RVLFaceLib/include" ${PETARI_REPO_ROOT}/libs/RVLFaceLib/include
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(petari_rfl_native STATIC rfl_native.cpp)
target_link_libraries(petari_rfl_native PUBLIC petari_rfl_headers petari_native_config)
target_compile_features(petari_rfl_native PUBLIC cxx_std_17)
# It includes the generated RVLFaceLib header copies.
add_dependencies(petari_rfl_native petari_legacy_sources)

# The whole library, compiled as C as on the Wii. The game links it with
# the SDK MEM heap, the native NAND (petari_platform_nand), and the GX bridge.
set(PETARI_RFL_SOURCES
    RFL_Controller.c RFL_DataUtility.c RFL_Database.c RFL_DefaultDatabase.c RFL_Format.c
    RFL_HiddenDatabase.c RFL_Icon.c RFL_MakeRandomFace.c RFL_MakeTex.c RFL_MiddleDatabase.c
    RFL_Model.c RFL_NANDAccess.c RFL_NANDLoader.c RFL_System.c)
list(TRANSFORM PETARI_RFL_SOURCES PREPEND "${PETARI_REPO_ROOT}/src/RVLFaceLib/")
add_library(petari_rfl STATIC ${PETARI_RFL_SOURCES})
petari_encode_target_sources(petari_rfl)
target_link_libraries(petari_rfl PUBLIC petari_rfl_native petari_platform_nand petari_platform_os petari_core)
target_compile_options(petari_rfl PRIVATE -Wno-missing-declarations -Wno-nonportable-include-path)

# RFL allocates from the SDK MEM expanded heap (petari_sdk_mem, root adds
# native/memory/sdk_mem before this directory).
target_link_libraries(petari_rfl PUBLIC petari_sdk_mem)

if(BUILD_TESTING)
    # RFL's own record conversion and validation (C, as on the Wii); dead
    # stripping drops their NAND, heap, and GX dependencies.
    set(PETARI_RFL_TEST_SOURCES
        ${PETARI_REPO_ROOT}/src/RVLFaceLib/RFL_DefaultDatabase.c
        ${PETARI_REPO_ROOT}/src/RVLFaceLib/RFL_Database.c
        ${PETARI_REPO_ROOT}/src/RVLFaceLib/RFL_DataUtility.c)
    add_executable(petari_rfl_tests ${PETARI_REPO_ROOT}/native/tests/rfl_tests.cpp ${PETARI_RFL_TEST_SOURCES})
    petari_encode_target_sources(petari_rfl_tests)
    target_link_libraries(petari_rfl_tests PRIVATE petari_rfl_native petari_resources)
    target_link_options(petari_rfl_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_rfl COMMAND petari_rfl_tests)
    # RFL booted as the game boots it: with and without a Mii database.
    add_executable(petari_rfl_boot_tests ${PETARI_REPO_ROOT}/native/tests/rfl_boot_tests.cpp)
    target_link_libraries(petari_rfl_boot_tests PRIVATE petari_rfl petari_resources)
    target_link_options(petari_rfl_boot_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_rfl_boot COMMAND petari_rfl_boot_tests)
    set(_petari_rfl_assets "${PETARI_REPO_ROOT}/build/game-data/RMGE01/files")
    if(EXISTS "${_petari_rfl_assets}/ObjectData/MiiFaceDatabase.arc")
        add_test(NAME native_rfl_assets COMMAND petari_rfl_tests --assets "${_petari_rfl_assets}")
        add_test(NAME native_rfl_boot_assets COMMAND petari_rfl_boot_tests --assets "${_petari_rfl_assets}")
    endif()
endif()
