# Native replacement for the HOME Button Menu module (see petari/home_menu.hpp).
#
# Root integration:   add_subdirectory(native/home_menu)   (after native/gx, so aurora's imgui target exists)
#   - link petari_home_menu into the target that compiles src/Game/System/HomeButtonMenuWrapper.cpp;
#   - link petari_home_menu_imgui into the application and call
#     PetariNative::HomeMenu::drawImGuiOverlay between aurora_begin_frame() and aurora_end_frame().
# Standalone build:   cmake -S native/home_menu -B build/home-menu -DPETARI_SANITIZERS=ON
#                     cmake --build build/home-menu && ctest --test-dir build/home-menu
cmake_minimum_required(VERSION 3.25)

set(_petari_home_menu_standalone OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_petari_home_menu_standalone ON)
    project(PetariNativeHomeMenu LANGUAGES C CXX)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
    include(CTest)
endif()

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(_petari_home_menu_standalone)
    # The part of the root build's shared configuration these sources need.
    add_library(petari_native_config INTERFACE)
    target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
    target_include_directories(petari_native_config INTERFACE
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
endif()

# The menu and the HBM entry points. home_menu.cpp has no SDK dependency; the
# bridge reads the SDK's HBM and KPAD structures.
add_library(petari_home_menu STATIC home_menu.cpp hbm_bridge.cpp launch_stage.cpp)
target_include_directories(petari_home_menu PUBLIC include ${PETARI_REPO_ROOT}/native/include)
target_link_libraries(petari_home_menu PUBLIC petari_native_config)
target_compile_features(petari_home_menu PUBLIC cxx_std_17)
# The Controls page lists the live bindings when the input layer is in the
# build (the root adds native/input first). Standalone builds show no lines.
if(TARGET petari_input)
    target_link_libraries(petari_home_menu PUBLIC petari_input petari_platform_dvd)  # Mod folder page: mod_folder.hpp
    target_compile_definitions(petari_home_menu PRIVATE PETARI_HOME_MENU_INPUT=1)
endif()

# The Dear ImGui overlay. Uses aurora's imgui target (IMGUI_USER_CONFIG and all);
# a standalone build compiles ImGui's core from aurora's fetched sources.
set(PETARI_IMGUI_SOURCE_DIR "${PETARI_REPO_ROOT}/build/aurora/_deps/imgui-src" CACHE PATH
    "Dear ImGui sources for a standalone overlay test")
if(NOT TARGET imgui AND _petari_home_menu_standalone AND EXISTS "${PETARI_IMGUI_SOURCE_DIR}/imgui.h")
    add_library(imgui STATIC
        ${PETARI_IMGUI_SOURCE_DIR}/imgui.cpp ${PETARI_IMGUI_SOURCE_DIR}/imgui_draw.cpp
        ${PETARI_IMGUI_SOURCE_DIR}/imgui_tables.cpp ${PETARI_IMGUI_SOURCE_DIR}/imgui_widgets.cpp)
    target_include_directories(imgui PUBLIC ${PETARI_IMGUI_SOURCE_DIR})
endif()
if(TARGET imgui)
    add_library(petari_home_menu_imgui STATIC imgui_overlay.cpp)
    # Deliberately not petari_native_config: nothing here sees the SDK headers.
    target_include_directories(petari_home_menu_imgui PUBLIC ${PETARI_REPO_ROOT}/native/include)
    target_link_libraries(petari_home_menu_imgui PUBLIC petari_home_menu imgui)
    if(TARGET petari_input)
        target_compile_definitions(petari_home_menu_imgui PRIVATE PETARI_HOME_MENU_INPUT=1)
    endif()
else()
    message(STATUS "petari_home_menu: no imgui target; the overlay is not built")
endif()

if(BUILD_TESTING)
    # HomeButtonMenuWrapper.cpp is compiled here too, so the tests go through
    # the game's own RSO::HBM* calls and prove the native branch links without
    # the RSO loader.
    add_executable(petari_home_menu_tests
        ${PETARI_REPO_ROOT}/native/tests/home_menu_tests.cpp
        ${PETARI_REPO_ROOT}/src/Game/System/HomeButtonMenuWrapper.cpp)
    if(COMMAND petari_encode_target_sources)
        petari_encode_target_sources(petari_home_menu_tests)
    endif()
    target_compile_options(petari_home_menu_tests PRIVATE -Wno-inconsistent-missing-override)
    if(TARGET petari_home_menu_imgui)
        target_link_libraries(petari_home_menu_tests PRIVATE petari_home_menu_imgui)
        target_compile_definitions(petari_home_menu_tests PRIVATE PETARI_HOME_MENU_TEST_IMGUI=1)
    else()
        target_link_libraries(petari_home_menu_tests PRIVATE petari_home_menu)
    endif()
    add_test(NAME native_home_menu COMMAND petari_home_menu_tests)
endif()
