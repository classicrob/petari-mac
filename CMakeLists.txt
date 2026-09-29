cmake_minimum_required(VERSION 3.25)
project(PetariNative LANGUAGES C CXX ASM)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
include(native/cmake/LegacySources.cmake)

add_library(petari_native_config INTERFACE)
target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
target_include_directories(petari_native_config INTERFACE
    "${PETARI_LEGACY_ROOT}/include"
    "${PETARI_LEGACY_ROOT}/libs/RVL_SDK/include"
    "${PETARI_LEGACY_ROOT}/libs/JSystem/include"
    "${PETARI_LEGACY_ROOT}/libs/nw4r/include"
    native/include include libs/RVL_SDK/include libs/JSystem/include libs/nw4r/include)
add_dependencies(petari_native_config petari_legacy_sources)
target_compile_options(petari_native_config INTERFACE
    -fno-strict-aliasing -ffp-contract=off -fcheck-new -Werror=return-type -Wno-register)
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
petari_encode_target_sources(petari_core)
set_source_files_properties(
    "${PETARI_LEGACY_ROOT}/src/RVL_SDK/mtx/mtx.c"
    "${PETARI_LEGACY_ROOT}/src/RVL_SDK/mtx/quat.c"
    "${PETARI_LEGACY_ROOT}/src/RVL_SDK/mtx/vec.c"
    PROPERTIES LANGUAGE CXX)
target_link_libraries(petari_core PUBLIC petari_native_config)

file(GLOB_RECURSE PETARI_GAME_SOURCES CONFIGURE_DEPENDS src/Game/*.cpp)
add_library(petari_game_objects OBJECT EXCLUDE_FROM_ALL ${PETARI_GAME_SOURCES})
petari_encode_target_sources(petari_game_objects)
target_sources(petari_game_objects PRIVATE src/Game/System/BCSVInclude_native.S)
target_include_directories(petari_game_objects PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(petari_game_objects PRIVATE petari_native_config)
target_include_directories(petari_game_objects PRIVATE "${PETARI_LEGACY_ROOT}/libs/RVLFaceLib/include" libs/RVLFaceLib/include)
target_compile_options(petari_game_objects PRIVATE -Wno-inconsistent-missing-override -fno-rtti)

include(CTest)
add_library(petari_host_runtime STATIC native/src/host_allocation.cpp native/text/utf16.cpp)
target_link_libraries(petari_host_runtime PUBLIC petari_native_config)
add_library(petari_memory STATIC native/memory/address_api.cpp native/memory/arena.cpp src/RVL_SDK/os/OSAlloc.c)
petari_encode_target_sources(petari_memory)
set_source_files_properties("${PETARI_LEGACY_ROOT}/src/RVL_SDK/os/OSAlloc.c" PROPERTIES LANGUAGE CXX)
target_link_libraries(petari_memory PUBLIC petari_native_config petari_host_runtime)
add_subdirectory(native/platform)
add_subdirectory(native/memory/sdk_mem)
add_subdirectory(native/input)
add_subdirectory(native/resource/rfl)
add_library(petari_boot STATIC native/memory/boot.cpp)
target_link_libraries(petari_boot PUBLIC petari_memory petari_platform_os petari_platform_sc)
add_library(petari_heaps STATIC
    src/JSystem/JKernel/JKRHeap.cpp src/JSystem/JKernel/JKRExpHeap.cpp
    src/JSystem/JKernel/JKRDisposer.cpp src/JSystem/JSupport/JSUList.cpp
    src/JSystem/JKernel/JKRSolidHeap.cpp src/JSystem/JKernel/JKRUnitHeap.cpp
    native/memory/unit_overrides.cpp native/memory/heap_overrides.cpp)
petari_encode_target_sources(petari_heaps)
target_link_libraries(petari_heaps PUBLIC petari_boot)
file(GLOB PETARI_KERNEL_SOURCES CONFIGURE_DEPENDS src/JSystem/JKernel/*.cpp)
list(FILTER PETARI_KERNEL_SOURCES EXCLUDE REGEX "/JKR(Heap|ExpHeap|Disposer|SolidHeap|UnitHeap)\\.cpp$")
add_library(petari_kernel STATIC ${PETARI_KERNEL_SOURCES}
    src/JSystem/JSupport/JSUInputStream.cpp src/JSystem/JSupport/JSUFileStream.cpp
    native/memory/aram_overrides.cpp native/memory/stream_overrides.cpp)
petari_encode_target_sources(petari_kernel)
target_link_libraries(petari_kernel PUBLIC petari_heaps petari_platform)
add_library(petari_resources STATIC native/resource/jpa_resource.cpp native/resource/j3d_animation.cpp native/resource/kcl_collision.cpp native/resource/j3d_model.cpp native/resource/archive.cpp native/resource/u8_archive.cpp src/RVL_SDK/arc/arc.c)
petari_encode_target_sources(petari_resources)
set_source_files_properties("${PETARI_LEGACY_ROOT}/src/RVL_SDK/arc/arc.c" PROPERTIES LANGUAGE CXX)
target_include_directories(petari_resources PUBLIC native/resource)
target_link_libraries(petari_resources PUBLIC petari_native_config petari_host_runtime)
add_executable(petari_asset_check native/resource/check_assets.cpp)
target_link_libraries(petari_asset_check PRIVATE petari_resources)
if(BUILD_TESTING)
    add_executable(petari_jpa_tests native/tests/jpa_tests.cpp)
    target_link_libraries(petari_jpa_tests PRIVATE petari_resources)
    add_test(NAME native_jpa COMMAND petari_jpa_tests)
    add_executable(petari_core_tests native/tests/core_tests.cpp)
    target_link_libraries(petari_core_tests PRIVATE petari_core)
    add_test(NAME native_core COMMAND petari_core_tests)
    add_executable(petari_nw4r_resource_tests native/tests/nw4r_resource_tests.cpp
        src/nw4r/lyt/lyt_nativeResource.cpp src/nw4r/lyt/lyt_common.cpp src/nw4r/lyt/lyt_texMap.cpp
        src/nw4r/lyt/lyt_layout.cpp src/nw4r/lyt/lyt_animation.cpp src/nw4r/lyt/lyt_pane.cpp
        src/nw4r/lyt/lyt_material.cpp src/nw4r/lyt/lyt_group.cpp src/nw4r/lyt/lyt_resourceAccessor.cpp
        src/nw4r/ut/ut_ResFont.cpp src/nw4r/ut/ut_ResFontBase.cpp src/nw4r/ut/ut_Font.cpp
        src/nw4r/ut/ut_CharStrmReader.cpp src/nw4r/ut/ut_binaryFileFormat.cpp src/nw4r/ut/ut_LinkList.cpp)
    petari_encode_target_sources(petari_nw4r_resource_tests)
    target_link_libraries(petari_nw4r_resource_tests PRIVATE petari_resources petari_platform_os petari_memory petari_host_runtime
        petari_sdk_mem petari_boot petari_platform_sc petari_platform_nand)
    target_link_options(petari_nw4r_resource_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_nw4r_resources COMMAND petari_nw4r_resource_tests)
    add_executable(petari_kcl_tests native/tests/kcl_tests.cpp)
    target_link_libraries(petari_kcl_tests PRIVATE petari_resources)
    add_test(NAME native_kcl COMMAND petari_kcl_tests)
    add_executable(petari_j3d_animation_tests native/tests/j3d_animation_tests.cpp)
    target_link_libraries(petari_j3d_animation_tests PRIVATE petari_resources)
    add_test(NAME native_j3d_animations COMMAND petari_j3d_animation_tests)
    add_executable(petari_canm_tests native/tests/canm_tests.cpp)
    target_link_libraries(petari_canm_tests PRIVATE petari_resources)
    add_test(NAME native_canm COMMAND petari_canm_tests)
    add_executable(petari_bitfield_tests native/tests/bitfield_layout_tests.cpp src/Game/Player/RushEndInfo.cpp)
    petari_encode_target_sources(petari_bitfield_tests)
    target_link_libraries(petari_bitfield_tests PRIVATE petari_native_config)
    add_test(NAME native_bitfields COMMAND petari_bitfield_tests)
    add_executable(petari_j3d_model_tests native/tests/j3d_model_tests.cpp)
    target_link_libraries(petari_j3d_model_tests PRIVATE petari_resources)
    add_test(NAME native_j3d_models COMMAND petari_j3d_model_tests)
    add_executable(petari_resource_tests native/tests/resource_tests.cpp)
    target_link_libraries(petari_resource_tests PRIVATE petari_resources)
    add_test(NAME native_resources COMMAND petari_resource_tests)
    add_executable(petari_library_tests native/tests/library_tests.cpp src/JSystem/J3DGraphBase/J3DTransform.cpp
        src/JSystem/JUtility/JUTNativeResource.cpp src/JSystem/JUtility/JUTNameTab.cpp)
    petari_encode_target_sources(petari_library_tests)
    target_link_libraries(petari_library_tests PRIVATE petari_core petari_host_runtime)
    add_test(NAME native_library COMMAND petari_library_tests)
    add_executable(petari_memory_tests native/tests/memory_tests.cpp)
    target_link_libraries(petari_memory_tests PRIVATE petari_memory)
    add_test(NAME native_memory COMMAND petari_memory_tests)
    add_executable(petari_heap_tests native/tests/heap_tests.cpp native/tests/heap_diagnostics.cpp)
    target_link_libraries(petari_heap_tests PRIVATE petari_heaps)
    add_test(NAME native_heaps COMMAND petari_heap_tests)
    add_executable(petari_jkr_archive_tests native/tests/jkr_archive_tests.cpp native/tests/heap_diagnostics.cpp
        src/JSystem/JUtility/JUTNativeResource.cpp)
    petari_encode_target_sources(petari_jkr_archive_tests)
    target_link_libraries(petari_jkr_archive_tests PRIVATE petari_kernel petari_resources)
    target_link_options(petari_jkr_archive_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_jkr_archives COMMAND petari_jkr_archive_tests)
    add_executable(petari_jmap_tests native/tests/jmap_tests.cpp src/Game/Util/JMapInfo.cpp src/JSystem/JGadget/hashcode.cpp)
    petari_encode_target_sources(petari_jmap_tests)
    target_link_libraries(petari_jmap_tests PRIVATE petari_resources)
    add_test(NAME native_jmap COMMAND petari_jmap_tests)
    add_executable(petari_message_tests native/tests/message_tests.cpp
        src/Game/System/MessageHolder.cpp src/Game/Screen/MessageEditorMessageTagNative.cpp
        src/Game/NPC/TalkMessageInfo.cpp src/Game/Util/JMapInfo.cpp src/JSystem/JGadget/hashcode.cpp)
    petari_encode_target_sources(petari_message_tests)
    target_link_libraries(petari_message_tests PRIVATE petari_resources petari_platform_os)
    add_test(NAME native_messages COMMAND petari_message_tests)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_jkr_disc COMMAND petari_jkr_archive_tests --disc "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01")
        add_test(NAME native_jpa_assets COMMAND petari_jpa_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_j3d_animation_assets COMMAND petari_j3d_animation_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_canm_assets COMMAND petari_canm_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_nw4r_resource_assets COMMAND petari_nw4r_resource_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_kcl_assets COMMAND petari_kcl_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_j3d_model_assets COMMAND petari_j3d_model_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_jmap_assets COMMAND petari_jmap_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        set_tests_properties(native_jmap_assets PROPERTIES ENVIRONMENT "PETARI_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
        add_test(NAME native_message_assets COMMAND petari_message_tests --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
    endif()
    add_executable(petari_game_compat_tests native/tests/game_compat_tests.cpp src/Game/Util/HashUtil.cpp)
    petari_encode_target_sources(petari_game_compat_tests)
    target_link_libraries(petari_game_compat_tests PRIVATE petari_native_config)
    target_compile_options(petari_game_compat_tests PRIVATE -Wno-deprecated-declarations)
    add_test(NAME native_game_compat COMMAND petari_game_compat_tests)
    add_test(NAME native_source_encoding COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/native/tests/source_encoding_tests.py")
endif()

option(PETARI_BUILD_GX_PROBE "Build the native GX/Metal integration probe (downloads Aurora dependencies)" OFF)
if(PETARI_BUILD_GX_PROBE)
    set(_petari_build_testing "${BUILD_TESTING}")
    add_subdirectory(native/gx)
    set(BUILD_TESTING "${_petari_build_testing}" CACHE BOOL "Build tests" FORCE)
endif()

add_subdirectory(native/home_menu)
target_link_libraries(petari_game_objects PRIVATE petari_home_menu)
if(TARGET petari_link_check)
    target_link_libraries(petari_link_check PRIVATE petari_home_menu petari_sdk_mem)
endif()

if(PETARI_BUILD_GX_PROBE)
    add_subdirectory(native/resource/rfl/render)
    add_subdirectory(native/movie)
    target_link_libraries(petari_sdk_helpers PUBLIC petari_movie)
    add_subdirectory(native/app EXCLUDE_FROM_ALL)
    target_link_libraries(petari_game_objects PRIVATE petari_milestone)
    target_link_libraries(petari_app_platform PUBLIC petari_present)
    target_link_libraries(petari_link_check PRIVATE petari_app_host petari_app_platform petari_rfl)
    target_sources(petari PRIVATE native/gx/legacy_commands.cpp native/gx/texture_cache.cpp)
    target_link_libraries(petari PRIVATE petari_game_objects petari_game_libraries petari_sdk_helpers
        petari_home_menu petari_sdk_mem petari_rfl petari_vi_bridge petari_gx_sync_bridge)
    target_link_options(petari PRIVATE -Wl,-dead_strip)
endif()
