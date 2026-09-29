# Native host platform services (Wii SDK replacements).
#
# Root integration:   add_subdirectory(native/platform)
# Standalone build:   cmake -S native/platform -B build/platform && cmake --build build/platform && ctest --test-dir build/platform
cmake_minimum_required(VERSION 3.25)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(PetariNativePlatform LANGUAGES C CXX)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(PETARI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
    include(CTest)
endif()

get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Same interface the root CMakeLists.txt defines; recreated only for standalone builds.
if(NOT TARGET petari_native_config)
    add_library(petari_native_config INTERFACE)
    target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
    target_include_directories(petari_native_config INTERFACE
        ${PETARI_REPO_ROOT}/native/include ${PETARI_REPO_ROOT}/include
        ${PETARI_REPO_ROOT}/libs/RVL_SDK/include ${PETARI_REPO_ROOT}/libs/JSystem/include
        ${PETARI_REPO_ROOT}/libs/nw4r/include)
    target_compile_options(petari_native_config INTERFACE
        -fno-strict-aliasing -ffp-contract=off -Werror=return-type -Wno-register)
    if(PETARI_SANITIZERS)
        target_compile_options(petari_native_config INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(petari_native_config INTERFACE -fsanitize=address,undefined)
    endif()
endif()

find_package(Threads REQUIRED)

# PetariNative::HostAllocationScope. The root build defines petari_host_runtime; a
# standalone build compiles the same source. Target names resolve at generate
# time, so the root may define it before or after this directory.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_library(petari_host_runtime STATIC ${PETARI_REPO_ROOT}/native/src/host_allocation.cpp)
    target_link_libraries(petari_host_runtime PUBLIC petari_native_config)
    target_compile_features(petari_host_runtime PUBLIC cxx_std_17)
    # Root's MEM1/MEM2 arenas (native/memory), which ARAM is carved from.
    add_library(petari_memory STATIC ${PETARI_REPO_ROOT}/native/memory/arena.cpp ${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c)
    set_source_files_properties(${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSAlloc.c PROPERTIES LANGUAGE CXX)
    target_link_libraries(petari_memory PUBLIC petari_native_config petari_host_runtime)
    target_compile_features(petari_memory PUBLIC cxx_std_17)
endif()

# Replaces src/RVL_SDK/dvd/*.c except the portable dvdidutils.c, which is
# compiled unchanged. Do not also link the Wii DVD driver sources.
add_library(petari_platform_dvd STATIC
    dvd/dvd_fst.cpp
    dvd/dvd_host.cpp
    ${PETARI_REPO_ROOT}/src/RVL_SDK/dvd/dvdidutils.c)
set_source_files_properties(${PETARI_REPO_ROOT}/src/RVL_SDK/dvd/dvdidutils.c PROPERTIES LANGUAGE CXX)
target_include_directories(petari_platform_dvd PUBLIC include PRIVATE dvd os)
target_link_libraries(petari_platform_dvd PUBLIC petari_platform_os petari_native_config petari_host_runtime Threads::Threads)
target_compile_features(petari_platform_dvd PUBLIC cxx_std_17)

# Native OS: interrupts, uniprocessor thread scheduler, alarms, time,
# reporting, and the Wii low-memory clock values. OSMutex.c and OSMessage.c are
# the SDK's own sources, compiled unchanged on top of the native scheduler.
set(PETARI_SDK_OS_SOURCES
    ${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSMutex.c
    ${PETARI_REPO_ROOT}/src/RVL_SDK/os/OSMessage.c)
add_library(petari_platform_os STATIC
    os/os_clock.cpp
    os/os_interrupts.cpp
    os/os_thread.cpp
    os/os_alarm.cpp
    os/os_time.cpp
    os/os_report.cpp
    os/os_cache.cpp
    os/os_power.cpp
    os/os_crash.cpp
    ${PETARI_SDK_OS_SOURCES})
set_source_files_properties(${PETARI_SDK_OS_SOURCES} PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/os/os_sdk_private.h")
target_include_directories(petari_platform_os PUBLIC include PRIVATE os)
target_link_libraries(petari_platform_os PUBLIC petari_native_config petari_host_runtime Threads::Threads)
target_compile_features(petari_platform_os PUBLIC cxx_std_17)

# NAND save storage (host IOS file system + port of the synchronous SDK NAND
# library) and ES title identity. Replaces src/RVL_SDK/nand/*.c and the ES
# calls the NAND library makes.
add_library(petari_platform_nand STATIC
    nand/isfs_host.cpp
    nand/nand_host.cpp
    nand/nand_async.cpp)
target_include_directories(petari_platform_nand PUBLIC include PRIVATE nand)
target_link_libraries(petari_platform_nand PUBLIC petari_platform_dvd petari_platform_os petari_native_config petari_host_runtime)
target_compile_features(petari_platform_nand PUBLIC cxx_std_17)

# SC system settings from native preferences. Replaces src/RVL_SDK/sc/*.c.
add_library(petari_platform_sc STATIC sc/sc_host.cpp)
target_include_directories(petari_platform_sc PUBLIC include)
target_link_libraries(petari_platform_sc PUBLIC petari_platform_os petari_platform_nand petari_native_config petari_host_runtime)
target_compile_features(petari_platform_sc PUBLIC cxx_std_17)

# ARAM in MEM2 (port of src/RVL_SDK/aralt/aralt.c). Replaces aralt.c.
add_library(petari_platform_aram STATIC aram/aram_host.cpp)
target_include_directories(petari_platform_aram PUBLIC include PRIVATE os)
target_link_libraries(petari_platform_aram PUBLIC petari_platform_os petari_memory petari_native_config)
target_compile_features(petari_platform_aram PUBLIC cxx_std_17)

# VI retrace interrupts and display state for the renderer bridge.
# Replaces src/RVL_SDK/vi/*.c.
add_library(petari_platform_vi STATIC vi/vi_host.cpp)
target_include_directories(petari_platform_vi PUBLIC include PRIVATE os)
target_link_libraries(petari_platform_vi PUBLIC petari_platform_os petari_platform_sc petari_native_config petari_host_runtime)
target_compile_features(petari_platform_vi PUBLIC cxx_std_17)

# Audio: AI output device and the native renderer for the JAudio2 DSP
# microcode. Replaces src/RVL_SDK/ai/ai.c.
add_library(petari_platform_audio STATIC audio/ai_host.cpp audio/dsp_renderer.cpp audio/dsp_device.cpp)
target_include_directories(petari_platform_audio PUBLIC include PRIVATE os audio)
target_link_libraries(petari_platform_audio PUBLIC petari_platform_os petari_platform_aram petari_native_config petari_host_runtime)
target_compile_features(petari_platform_audio PUBLIC cxx_std_17)

# WiiConnect24: unavailable, as a console with it disabled. Replaces src/RVL_SDK/nwc24/*.c.
add_library(petari_platform_nwc24 STATIC nwc24/nwc24_host.cpp nwc24/vf_host.cpp)
target_link_libraries(petari_platform_nwc24 PUBLIC petari_platform_os petari_native_config)
target_compile_features(petari_platform_nwc24 PUBLIC cxx_std_17)

# GP interrupts and GX synchronisation (draw sync, draw done, FIFO
# breakpoints) for the renderer's GX wrappers. See GX_SYNC_PLAN.md.
add_library(petari_platform_gx_sync STATIC gx_sync/gx_sync.cpp)
target_include_directories(petari_platform_gx_sync PUBLIC include PRIVATE os)
target_link_libraries(petari_platform_gx_sync PUBLIC petari_platform_os petari_native_config petari_host_runtime)
target_compile_features(petari_platform_gx_sync PUBLIC cxx_std_17)

add_library(petari_platform INTERFACE)
target_link_libraries(petari_platform INTERFACE petari_platform_dvd petari_platform_os petari_platform_nand petari_platform_sc
    petari_platform_aram petari_platform_vi petari_platform_audio petari_platform_nwc24 petari_platform_gx_sync)

# Manual check against a real extracted disc (not run by ctest: no retail data in the repo).
add_executable(petari_dvd_smoke tools/dvd_smoke.cpp)
target_link_libraries(petari_dvd_smoke PRIVATE petari_platform)

if(BUILD_TESTING)
    add_executable(petari_platform_dvd_tests ${PETARI_REPO_ROOT}/native/tests/platform_dvd_tests.cpp)
    target_include_directories(petari_platform_dvd_tests PRIVATE dvd)
    target_link_libraries(petari_platform_dvd_tests PRIVATE petari_platform)
    add_test(NAME native_platform_dvd COMMAND petari_platform_dvd_tests)

    add_executable(petari_platform_os_tests ${PETARI_REPO_ROOT}/native/tests/platform_os_tests.cpp)
    target_include_directories(petari_platform_os_tests PRIVATE os dvd)
    target_link_libraries(petari_platform_os_tests PRIVATE petari_platform)
    add_test(NAME native_platform_os COMMAND petari_platform_os_tests)

    add_executable(petari_platform_power_tests ${PETARI_REPO_ROOT}/native/tests/platform_power_tests.cpp)
    target_link_libraries(petari_platform_power_tests PRIVATE petari_platform)
    add_test(NAME native_platform_power COMMAND petari_platform_power_tests)

    add_executable(petari_platform_nwc24_tests ${PETARI_REPO_ROOT}/native/tests/platform_nwc24_tests.cpp)
    target_link_libraries(petari_platform_nwc24_tests PRIVATE petari_platform)
    add_test(NAME native_platform_nwc24 COMMAND petari_platform_nwc24_tests)

    add_executable(petari_platform_crash_tests ${PETARI_REPO_ROOT}/native/tests/platform_crash_tests.cpp)
    target_link_libraries(petari_platform_crash_tests PRIVATE petari_platform)
    add_test(NAME native_platform_crash COMMAND petari_platform_crash_tests)

    add_executable(petari_platform_gx_sync_tests ${PETARI_REPO_ROOT}/native/tests/platform_gx_sync_tests.cpp)
    target_link_libraries(petari_platform_gx_sync_tests PRIVATE petari_platform)
    add_test(NAME native_platform_gx_sync COMMAND petari_platform_gx_sync_tests)
    # native/gx/sync_{bridge,backend}.cpp against a fake Aurora FIFO (root
    # wires the real Aurora build).
    add_executable(petari_gx_sync_backend_tests ${PETARI_REPO_ROOT}/native/tests/gx_sync_backend_tests.cpp
        ${PETARI_REPO_ROOT}/native/gx/sync_bridge.cpp ${PETARI_REPO_ROOT}/native/gx/sync_backend.cpp)
    target_link_libraries(petari_gx_sync_backend_tests PRIVATE petari_platform)
    add_test(NAME native_gx_sync_backend COMMAND petari_gx_sync_backend_tests)

    add_executable(petari_platform_nand_tests ${PETARI_REPO_ROOT}/native/tests/platform_nand_tests.cpp)
    target_link_libraries(petari_platform_nand_tests PRIVATE petari_platform)
    add_test(NAME native_platform_nand COMMAND petari_platform_nand_tests)

    add_executable(petari_platform_aram_tests ${PETARI_REPO_ROOT}/native/tests/platform_aram_tests.cpp)
    target_link_libraries(petari_platform_aram_tests PRIVATE petari_platform)
    add_test(NAME native_platform_aram COMMAND petari_platform_aram_tests)

    add_executable(petari_platform_vi_tests ${PETARI_REPO_ROOT}/native/tests/platform_vi_tests.cpp)
    target_link_libraries(petari_platform_vi_tests PRIVATE petari_platform)
    add_test(NAME native_platform_vi COMMAND petari_platform_vi_tests)

    add_executable(petari_platform_audio_tests ${PETARI_REPO_ROOT}/native/tests/platform_audio_tests.cpp)
    target_include_directories(petari_platform_audio_tests PRIVATE audio)
    target_link_libraries(petari_platform_audio_tests PRIVATE petari_platform)
    add_test(NAME native_platform_audio COMMAND petari_platform_audio_tests)

    # JAudio2's own DSP host code driving the native DSP device.
    set(PETARI_JAUDIO_DSP_SOURCES
        ${PETARI_REPO_ROOT}/src/JSystem/JAudio2/dsptask.cpp
        ${PETARI_REPO_ROOT}/src/JSystem/JAudio2/dspproc.cpp
        ${PETARI_REPO_ROOT}/src/JSystem/JAudio2/osdsp.cpp
        ${PETARI_REPO_ROOT}/src/JSystem/JAudio2/osdsp_task.cpp)
    add_executable(petari_platform_audio_dsp_tests ${PETARI_REPO_ROOT}/native/tests/platform_audio_dsp_tests.cpp ${PETARI_JAUDIO_DSP_SOURCES})
    target_link_libraries(petari_platform_audio_dsp_tests PRIVATE petari_platform)
    add_test(NAME native_platform_audio_dsp COMMAND petari_platform_audio_dsp_tests)

    # Audio boot smoke (root integration only, not built by default): the real
    # game audio initialisation (AudNewAudSystem: JAS/JAI, JASAudioThread, the
    # DSP device, AI) on the extracted disc, with output pulled
    # deterministically. Static libraries of the needed JSystem and game
    # audio sources; only referenced objects are linked.
    # Allocation ownership under the game's replacement operator new/delete
    # (repository-root build only: needs the JKR heaps; standalone subprojects
    # such as native/input also add this directory).
    if(CMAKE_SOURCE_DIR STREQUAL PETARI_REPO_ROOT)
        add_executable(petari_platform_allocation_tests ${PETARI_REPO_ROOT}/native/tests/platform_allocation_tests.cpp
            ${PETARI_REPO_ROOT}/native/tests/heap_diagnostics.cpp
            ${PETARI_JAUDIO_DSP_SOURCES})
        petari_encode_target_sources(petari_platform_allocation_tests)
        target_link_libraries(petari_platform_allocation_tests PRIVATE petari_platform petari_heaps petari_boot)
        add_test(NAME native_platform_allocation COMMAND petari_platform_allocation_tests)
    endif()

    option(PETARI_AUDIO_BOOT_SMOKE "Build and register the audio boot smoke test (needs the extracted disc in build/game-data/RMGE01)" OFF)
    if(PETARI_AUDIO_BOOT_SMOKE AND CMAKE_SOURCE_DIR STREQUAL PETARI_REPO_ROOT)
        file(GLOB_RECURSE PETARI_AUDIO_BOOT_JSYSTEM CONFIGURE_DEPENDS
            ${PETARI_REPO_ROOT}/src/JSystem/*.cpp ${PETARI_REPO_ROOT}/src/nw4r/*.cpp)
        list(FILTER PETARI_AUDIO_BOOT_JSYSTEM EXCLUDE REGEX "/JSystem/(JKernel|JMath|J3DGraphBase|J3DGraphAnimator|J3DGraphLoader)/")
        list(FILTER PETARI_AUDIO_BOOT_JSYSTEM EXCLUDE REGEX "/JASSeqReader\\.cpp$")
        list(FILTER PETARI_AUDIO_BOOT_JSYSTEM EXCLUDE REGEX "/JSU(List|InputStream|FileStream)\\.cpp$")
        list(FILTER PETARI_AUDIO_BOOT_JSYSTEM EXCLUDE REGEX "/JUT(NameTab|NativeResource|Assert)\\.cpp$")
        add_library(petari_audio_boot_jsystem STATIC EXCLUDE_FROM_ALL ${PETARI_AUDIO_BOOT_JSYSTEM})
        petari_encode_target_sources(petari_audio_boot_jsystem)
        target_compile_options(petari_audio_boot_jsystem PRIVATE -fno-rtti -w)
        target_link_libraries(petari_audio_boot_jsystem PUBLIC petari_kernel petari_core petari_platform)

        file(GLOB PETARI_AUDIO_BOOT_GAME CONFIGURE_DEPENDS
            ${PETARI_REPO_ROOT}/src/Game/AudioLib/*.cpp ${PETARI_REPO_ROOT}/src/Game/RhythmLib/*.cpp
            ${PETARI_REPO_ROOT}/src/Game/Speaker/*.cpp)
        # GameSystem's own audio start-up (heaps, file loading,
        # AudSystemWrapper) and the game's library overrides come from
        # Game/System and Game/Util; the static library links only the objects
        # the test reaches.
        # Listed, not globbed: an archive of all of Game/System and Game/Util
        # lets the linker satisfy references from dead audio code with scene
        # objects whose static constructors then pull in the scene graph.
        foreach(_source IN ITEMS
                System/HeapMemoryWatcher System/AudSystemWrapper System/FileLoader System/FileLoaderThread
                System/FileHolder System/ArchiveHolder System/FileRipper System/OSThreadWrapper
                System/Overwrite System/Language Util/FileUtil Util/StringUtil Util/MemoryUtil)
            list(APPEND PETARI_AUDIO_BOOT_GAME ${PETARI_REPO_ROOT}/src/Game/${_source}.cpp)
        endforeach()
        list(APPEND PETARI_AUDIO_BOOT_GAME ${PETARI_REPO_ROOT}/src/RVL_SDK/wenc/wenc.c)
        add_library(petari_audio_boot_game STATIC EXCLUDE_FROM_ALL ${PETARI_AUDIO_BOOT_GAME})
        set_source_files_properties("${PETARI_LEGACY_ROOT}/src/RVL_SDK/wenc/wenc.c" PROPERTIES LANGUAGE CXX)
        petari_encode_target_sources(petari_audio_boot_game)
        target_include_directories(petari_audio_boot_game PRIVATE ${PETARI_REPO_ROOT}
            "${PETARI_LEGACY_ROOT}/libs/RVLFaceLib/include" ${PETARI_REPO_ROOT}/libs/RVLFaceLib/include)
        target_compile_options(petari_audio_boot_game PRIVATE -fno-rtti -w -Wno-inconsistent-missing-override)
        target_link_libraries(petari_audio_boot_game PUBLIC petari_audio_boot_jsystem petari_input petari_home_menu)

        add_executable(petari_audio_boot_tests EXCLUDE_FROM_ALL ${PETARI_REPO_ROOT}/native/tests/audio_boot_tests.cpp)
        petari_encode_target_sources(petari_audio_boot_tests)
        target_include_directories(petari_audio_boot_tests PRIVATE ${PETARI_REPO_ROOT})
        target_compile_options(petari_audio_boot_tests PRIVATE -fno-rtti)
        target_link_libraries(petari_audio_boot_tests PRIVATE petari_audio_boot_game petari_audio_boot_jsystem petari_boot)
        target_link_options(petari_audio_boot_tests PRIVATE -Wl,-dead_strip)
        if(EXISTS "${PETARI_REPO_ROOT}/build/game-data/RMGE01/files")
            add_test(NAME native_audio_boot COMMAND petari_audio_boot_tests --disc "${PETARI_REPO_ROOT}/build/game-data/RMGE01")
        endif()
    endif()
endif()
