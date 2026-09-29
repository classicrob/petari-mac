# The native application: entry point, platform start-up, and the frame seam
# (petari/app.hpp, INTEGRATION_PLAN.md).
#
# Root integration: add_subdirectory(native/app) after native/gx, native/input,
# native/home_menu and native/audio. It defines:
#   petari_milestone     petari/milestone.hpp; link it into the game library that
#                        compiles src/Game/Map/FileSelector.cpp (plain C, no deps)
#   petari_app_smoke     the smoke script (smoke.hpp), no SDK or Aurora headers
#   petari_app_platform  platform start-up and exit (SDK headers, no Aurora)
#   petari_app_host      frame seam and window events (Aurora/SDL headers, no SDK)
#   petari              the executable; root links the game libraries, the
#                        renderer (petari_vi_bridge and its presentation hooks)
#                        and the GX sync bridge into it.
# Standalone (seam tests only, no Aurora library):
#   cmake -S native/app -B build/app && cmake --build build/app && ctest --test-dir build/app
cmake_minimum_required(VERSION 3.25)

set(_petari_app_standalone OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_petari_app_standalone ON)
    project(PetariNativeApp LANGUAGES CXX)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
    include(CTest)
endif()

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(petari_milestone STATIC milestone.cpp)
target_include_directories(petari_milestone PUBLIC ${PETARI_REPO_ROOT}/native/include)
target_compile_features(petari_milestone PUBLIC cxx_std_17)
add_library(petari_app_smoke STATIC smoke.cpp)
target_compile_features(petari_app_smoke PUBLIC cxx_std_17)

if(NOT _petari_app_standalone)
    add_library(petari_app_platform STATIC platform_host.cpp smoke_game.cpp frame_phase.cpp)
    target_link_libraries(petari_app_platform PUBLIC petari_native_config petari_boot
        petari_platform_os petari_platform_vi petari_platform_dvd petari_platform_nand petari_platform_sc
        petari_audio_sdl petari_host_runtime petari_app_smoke petari_milestone)

    add_library(petari_app_host STATIC frame_seam.cpp frame_stats.cpp input_events.cpp)
    target_include_directories(petari_app_host PUBLIC ${PETARI_REPO_ROOT}/native/include)
    # Deliberately not petari_native_config: no SDK include paths next to Aurora's.
    target_link_libraries(petari_app_host PUBLIC aurora::core petari_input_sdl3 petari_home_menu_imgui
        petari_host_runtime petari_app_smoke)

    add_executable(petari MACOSX_BUNDLE app_main.cpp)
    target_link_libraries(petari PRIVATE petari_app_host petari_app_platform aurora::core aurora::main)
    set_target_properties(petari PROPERTIES OUTPUT_NAME "Petari")
    set(PETARI_PIPELINE_SEED "" CACHE FILEPATH "Explicit curated Aurora pipeline-config seed (not Dawn cache)")
    if(PETARI_PIPELINE_SEED)
        if(NOT EXISTS "${PETARI_PIPELINE_SEED}")
            message(FATAL_ERROR "PETARI_PIPELINE_SEED does not exist: ${PETARI_PIPELINE_SEED}")
        endif()
        configure_file("${PETARI_PIPELINE_SEED}" "${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db" COPYONLY)
        target_sources(petari PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db")
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
    endif()
else()
    set(PETARI_AURORA_INCLUDE_DIR "${PETARI_REPO_ROOT}/../aurora-reference/include" CACHE PATH "Aurora headers")
    set(PETARI_SDL3_INCLUDE_DIR "${PETARI_REPO_ROOT}/build/aurora/_deps/sdl-src/include" CACHE PATH "SDL3 headers")
    if(PETARI_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME native_observatory_fixture COMMAND "${Python3_EXECUTABLE}" "${PETARI_REPO_ROOT}/native/tests/test_fixture_tests.py")
    # The seam against recording fakes of Aurora, SDL and the app's other units.
    add_executable(petari_app_seam_tests ${PETARI_REPO_ROOT}/native/tests/app_seam_tests.cpp frame_seam.cpp frame_stats.cpp)
    target_link_libraries(petari_app_seam_tests PRIVATE petari_app_smoke)
    target_include_directories(petari_app_seam_tests PRIVATE ${PETARI_REPO_ROOT}/native/include)
    if(_petari_app_standalone)
        target_include_directories(petari_app_seam_tests PRIVATE ${PETARI_AURORA_INCLUDE_DIR} ${PETARI_SDL3_INCLUDE_DIR})
    else()
        target_include_directories(petari_app_seam_tests PRIVATE
            $<TARGET_PROPERTY:aurora::core,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    add_test(NAME native_app_seam COMMAND petari_app_seam_tests)

    # Physical input is counted separately from the smoke driver's injected input.
    add_executable(petari_app_input_events_tests ${PETARI_REPO_ROOT}/native/tests/app_input_events_tests.cpp
        input_events.cpp ${PETARI_REPO_ROOT}/native/input/bindings.cpp)
    target_include_directories(petari_app_input_events_tests PRIVATE ${PETARI_REPO_ROOT}/native/include
        ${PETARI_REPO_ROOT}/native/input/include)
    if(_petari_app_standalone)
        target_include_directories(petari_app_input_events_tests PRIVATE ${PETARI_SDL3_INCLUDE_DIR})
    else()
        target_include_directories(petari_app_input_events_tests PRIVATE
            $<TARGET_PROPERTY:aurora::core,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    add_test(NAME native_app_input_events COMMAND petari_app_input_events_tests)

    # The smoke script's decisions and the milestone history, without the game.
    add_executable(petari_app_smoke_tests ${PETARI_REPO_ROOT}/native/tests/app_smoke_tests.cpp)
    target_include_directories(petari_app_smoke_tests PRIVATE ${PETARI_REPO_ROOT}/native/include)
    target_link_libraries(petari_app_smoke_tests PRIVATE petari_app_smoke petari_milestone)
    add_test(NAME native_app_smoke COMMAND petari_app_smoke_tests)
endif()
