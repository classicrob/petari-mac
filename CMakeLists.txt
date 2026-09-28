cmake_minimum_required(VERSION 3.25)
project(PetariNative LANGUAGES C CXX)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(petari_native_config INTERFACE)
target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
target_include_directories(petari_native_config INTERFACE
    native/include include libs/RVL_SDK/include libs/JSystem/include libs/nw4r/include)
target_compile_options(petari_native_config INTERFACE
    -fno-strict-aliasing -ffp-contract=off -Werror=return-type -Wno-register)
if(PETARI_SANITIZERS)
    target_compile_options(petari_native_config INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(petari_native_config INTERFACE -fsanitize=address,undefined)
endif()

add_library(petari_core STATIC
    native/src/mtx.cpp
    native/src/intrinsics.cpp
    native/src/jmath.cpp
    src/JSystem/JAudio2/JASSeqReader.cpp
    src/RVL_SDK/mtx/mtx.c
    src/RVL_SDK/mtx/mtx44.c
    src/RVL_SDK/mtx/quat.c
    src/RVL_SDK/mtx/vec.c
    src/JSystem/JMath/random.cpp
    src/JSystem/JMath/JMath.cpp
    src/JSystem/JMath/JMATrigonometric.cpp
    src/Game/LiveActor/Nerve.cpp
    src/Game/LiveActor/Spine.cpp
    src/Game/LiveActor/ActorStateBase.cpp
    src/Game/LiveActor/ActorStateKeeper.cpp
    src/Game/System/NerveExecutor.cpp)
set_source_files_properties(
    src/RVL_SDK/mtx/mtx.c src/RVL_SDK/mtx/quat.c src/RVL_SDK/mtx/vec.c
    PROPERTIES LANGUAGE CXX)
target_link_libraries(petari_core PUBLIC petari_native_config)

file(GLOB_RECURSE PETARI_GAME_SOURCES CONFIGURE_DEPENDS src/Game/*.cpp)
add_library(petari_game_objects OBJECT EXCLUDE_FROM_ALL ${PETARI_GAME_SOURCES})
target_link_libraries(petari_game_objects PRIVATE petari_native_config)
target_include_directories(petari_game_objects PRIVATE libs/RVLFaceLib/include)
target_compile_options(petari_game_objects PRIVATE -Wno-inconsistent-missing-override)

include(CTest)
if(BUILD_TESTING)
    add_executable(petari_core_tests native/tests/core_tests.cpp)
    target_link_libraries(petari_core_tests PRIVATE petari_core)
    add_test(NAME native_core COMMAND petari_core_tests)
endif()

option(PETARI_BUILD_GX_PROBE "Build the native GX/Metal integration probe (downloads Aurora dependencies)" OFF)
if(PETARI_BUILD_GX_PROBE)
    set(_petari_build_testing "${BUILD_TESTING}")
    add_subdirectory(native/gx)
    set(BUILD_TESTING "${_petari_build_testing}" CACHE BOOL "Build tests" FORCE)
endif()
