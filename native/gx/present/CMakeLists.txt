# Standalone GPU tests for display copies and XFB presentation (README.md).
# Root integrates the sources in its own build (see README.md, "Wiring");
# this project only exists to test them against a real Metal device.
#
#   cmake -S native/gx/present -B build/present -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/present -j8 && ctest --test-dir build/present --output-on-failure
#
# Aurora comes from the local aurora-reference checkout and its dependencies
# from an existing Aurora build's _deps, so nothing is downloaded.
cmake_minimum_required(VERSION 3.25)
project(PetariPresentTests LANGUAGES C CXX OBJC)
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
# Aurora forces BUILD_TESTING off in its cache, so this project has its own switch.
option(PETARI_PRESENT_TESTS "Build the present tests" ON)

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PETARI_AURORA_DIR "${PETARI_REPO_ROOT}/../aurora-reference" CACHE PATH "Aurora sources")
set(PETARI_AURORA_DEPS "${PETARI_REPO_ROOT}/build/aurora/_deps" CACHE PATH "Fetched Aurora dependencies")
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

foreach(_dep sdl imgui dawn_prebuilt tracy xxhash)
    string(TOUPPER "${_dep}" _upper)
    if(EXISTS "${PETARI_AURORA_DEPS}/${_dep}-src")
        set(FETCHCONTENT_SOURCE_DIR_${_upper} "${PETARI_AURORA_DEPS}/${_dep}-src" CACHE PATH "" FORCE)
    endif()
endforeach()
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
# As root does: a SQLite with sqlite3_changes64, not the macOS SDK framework.
set(CMAKE_FIND_FRAMEWORK LAST)
if(EXISTS /opt/homebrew/opt/sqlite)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/sqlite)
endif()
find_package(SQLite3 3.37 REQUIRED GLOBAL)
add_subdirectory("${PETARI_AURORA_DIR}" aurora EXCLUDE_FROM_ALL)

# The product patches, as root applies them.
function(petari_present_patch target relative output)
    set(_source "${PETARI_AURORA_DIR}/${relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PETARI_REPO_ROOT}/native/gx/patch_aurora_present.py" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/gx/patch_aurora_present.py"
        "${_source}" "${output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources ${target} SOURCES)
    list(REMOVE_ITEM _sources "${relative}" "${_source}")
    list(APPEND _sources "${output}")
    set_property(TARGET ${target} PROPERTY SOURCES "${_sources}")
    get_filename_component(_dir "${_source}" DIRECTORY)
    set_source_files_properties("${output}" TARGET_DIRECTORY ${target} PROPERTIES
        COMPILE_OPTIONS "-iquote${_dir};-iquote${PETARI_AURORA_DIR}/lib;-iquote${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
petari_present_patch(aurora_gx lib/dolphin/gx/GXFrameBuffer.cpp "${CMAKE_BINARY_DIR}/patched/GXFrameBuffer.cpp")
petari_present_patch(aurora_core lib/aurora.cpp "${CMAKE_BINARY_DIR}/patched/aurora.cpp")
target_sources(aurora_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/present_aurora.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/present_aurora.cpp" TARGET_DIRECTORY aurora_core PROPERTIES
    COMPILE_OPTIONS "-iquote${PETARI_AURORA_DIR}/lib")
# GXGetNumXfbLines and the other SDK additions the game build gets from api_compat.cpp.
target_sources(aurora_gx PRIVATE "${PETARI_REPO_ROOT}/native/gx/api_compat.cpp")
set_source_files_properties("${PETARI_REPO_ROOT}/native/gx/api_compat.cpp" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${PETARI_AURORA_DIR}/lib")

# Root's blocking-pipeline patch, so first-use copies are not dropped while
# their pipeline compiles (as in the game build).
set(_pipeline_source "${PETARI_AURORA_DIR}/lib/gfx/pipeline_cache.cpp")
set(_pipeline_output "${CMAKE_BINARY_DIR}/patched/pipeline_cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PETARI_REPO_ROOT}/native/gx/patch_aurora_pipeline.py" "${_pipeline_source}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/gx/patch_aurora_pipeline.py"
    "${_pipeline_source}" "${_pipeline_output}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_sources aurora_gx SOURCES)
list(REMOVE_ITEM _sources lib/gfx/pipeline_cache.cpp "${_pipeline_source}")
list(APPEND _sources "${_pipeline_output}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
set_source_files_properties("${_pipeline_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${PETARI_AURORA_DIR}/lib/gfx;-iquote${PETARI_AURORA_DIR}/lib")

# Test only: render textures also get CopySrc so the test can read copies back.
set(_texture_source "${PETARI_AURORA_DIR}/lib/gfx/texture.cpp")
set(_texture_output "${CMAKE_BINARY_DIR}/patched/texture.cpp")
file(READ "${_texture_source}" _texture)
set(_usage "wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment,")
string(FIND "${_texture}" "${_usage}" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "render texture usage anchor not found in ${_texture_source}")
endif()
string(REPLACE "${_usage}"
    "wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,"
    _texture "${_texture}")
file(WRITE "${_texture_output}.tmp" "${_texture}")
file(COPY_FILE "${_texture_output}.tmp" "${_texture_output}" ONLY_IF_DIFFERENT)
get_target_property(_sources aurora_gx SOURCES)
list(REMOVE_ITEM _sources lib/gfx/texture.cpp "${_texture_source}")
list(APPEND _sources "${_texture_output}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
set_source_files_properties("${_texture_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${PETARI_AURORA_DIR}/lib/gfx;-iquote${PETARI_AURORA_DIR}/lib")

if(PETARI_PRESENT_TESTS)
    add_test(NAME native_present_patch
        COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/tests/present_patch_tests.py"
            "${PETARI_AURORA_DIR}" "${PETARI_REPO_ROOT}/native/gx")
    add_executable(petari_present_gpu_tests "${PETARI_REPO_ROOT}/native/tests/present_gpu_tests.cpp")
    target_include_directories(petari_present_gpu_tests PRIVATE "${PETARI_AURORA_DIR}/lib" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(petari_present_gpu_tests PRIVATE aurora::core aurora::gx aurora::vi aurora::main)
    add_test(NAME native_present_gpu COMMAND petari_present_gpu_tests)
    set_tests_properties(native_present_gpu PROPERTIES TIMEOUT 120)
endif()
