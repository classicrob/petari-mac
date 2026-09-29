include(FetchContent)
enable_language(OBJC)
set(CMAKE_FIND_FRAMEWORK LAST)
find_package(SQLite3 3.37 REQUIRED GLOBAL)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
FetchContent_Declare(aurora
    GIT_REPOSITORY https://github.com/encounter/aurora.git
    GIT_TAG 08122911e8621acb7ded6563813b264bec1494b5)
FetchContent_MakeAvailable(aurora)

set(_petari_regs "${CMAKE_CURRENT_BINARY_DIR}/aurora-regs.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_regs.py" "${aurora_SOURCE_DIR}/lib/gx/regs.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_regs.py"
    "${aurora_SOURCE_DIR}/lib/gx/regs.cpp" "${_petari_regs}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_aurora_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _aurora_gx_sources lib/gx/regs.cpp "${aurora_SOURCE_DIR}/lib/gx/regs.cpp")
list(APPEND _aurora_gx_sources "${_petari_regs}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_aurora_gx_sources}")
target_include_directories(aurora_gx PRIVATE "${aurora_SOURCE_DIR}/lib/gx")

set(_petari_vi "${CMAKE_CURRENT_BINARY_DIR}/aurora-vi.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_vi.py" "${aurora_SOURCE_DIR}/lib/dolphin/vi/vi.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_vi.py"
    "${aurora_SOURCE_DIR}/lib/dolphin/vi/vi.cpp" "${_petari_vi}" COMMAND_ERROR_IS_FATAL ANY)
set_property(TARGET aurora_vi PROPERTY SOURCES "${_petari_vi}")
target_include_directories(aurora_vi PRIVATE "${aurora_SOURCE_DIR}/lib/dolphin/vi")
set(_petari_transform "${CMAKE_CURRENT_BINARY_DIR}/aurora-transform.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_transform.py" "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXTransform.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_transform.py"
    "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXTransform.cpp" "${_petari_transform}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gx_sources lib/dolphin/gx/GXTransform.cpp "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXTransform.cpp")
list(APPEND _gx_sources "${_petari_transform}" "${CMAKE_CURRENT_SOURCE_DIR}/api_compat.cpp")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gx_sources}")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/api_compat.cpp" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib")
set_source_files_properties("${_petari_transform}" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/dolphin/gx")
set(_texture_source "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXTexture.cpp")
set(_texture_output "${CMAKE_CURRENT_BINARY_DIR}/aurora-texture.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_texture.py" "${_texture_source}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_texture.py"
    "${_texture_source}" "${_texture_output}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_sources aurora_gx SOURCES)
list(REMOVE_ITEM _sources lib/dolphin/gx/GXTexture.cpp "${_texture_source}")
list(APPEND _sources "${_texture_output}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
set_source_files_properties("${_texture_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/dolphin/gx")
add_library(petari_vi_bridge STATIC vi_bridge.cpp vi_backend.cpp)
target_include_directories(petari_vi_bridge PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_vi_bridge PUBLIC petari_boot petari_platform_vi aurora::vi)

function(petari_isolate_aurora_allocations target source)
    set(_source "${aurora_SOURCE_DIR}/${source}")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/allocation-safe/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_allocations.py" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_allocations.py"
        "${_source}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_filename_component(_original_dir "${_source}" DIRECTORY)
    set_source_files_properties("${_output}" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-iquote${_original_dir}")
    get_target_property(_sources ${target} SOURCES)
    list(REMOVE_ITEM _sources "${source}" "${_source}")
    list(APPEND _sources "${_output}")
    set_property(TARGET ${target} PROPERTY SOURCES "${_sources}")
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/native/include")
    target_link_libraries(${target} PUBLIC "$<LINK_ONLY:petari_host_runtime>")
endfunction()
petari_isolate_aurora_allocations(aurora_core lib/aurora.cpp)
petari_isolate_aurora_allocations(aurora_gx lib/dolphin/gx/GXManage.cpp)
petari_isolate_aurora_allocations(aurora_gx lib/gx/fifo.cpp)
petari_isolate_aurora_allocations(aurora_gx lib/gfx/render_worker.cpp)
petari_isolate_aurora_allocations(aurora_gx lib/gfx/texture_replacement.cpp)

include("${CMAKE_SOURCE_DIR}/native/cmake/GraphicsPresentation.cmake")

# Synchronisation patches compose with the allocation-safe FIFO and GX init.
foreach(_relative lib/gx/fifo.cpp lib/dolphin/gx/GXManage.cpp
        lib/gx/command_processor.cpp lib/dolphin/gx/GXFifo.cpp lib/dolphin/gx/GXCpu2Efb.cpp)
    set(_original "${aurora_SOURCE_DIR}/${_relative}")
    if(_relative MATCHES "(fifo|GXManage)\\.cpp$")
        set(_input "${CMAKE_CURRENT_BINARY_DIR}/allocation-safe/${_relative}")
    else()
        set(_input "${_original}")
    endif()
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/sync/${_relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_sync.py" "${_original}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_sync.py"
        "${_input}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources aurora_gx SOURCES)
    list(REMOVE_ITEM _sources "${_relative}" "${_original}" "${_input}")
    list(APPEND _sources "${_output}")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    get_filename_component(_original_dir "${_original}" DIRECTORY)
    set_source_files_properties("${_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
        COMPILE_OPTIONS "-iquote${_original_dir}")
endforeach()
target_include_directories(aurora_gx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(petari_gx_sync_bridge STATIC sync_bridge.cpp sync_backend.cpp efb_peek.cpp)
target_link_libraries(petari_gx_sync_bridge PUBLIC petari_platform_gx_sync petari_platform_os)
target_link_libraries(aurora_gx PUBLIC "$<LINK_ONLY:petari_gx_sync_bridge>")

foreach(_file recording frame)
    set(_source "${aurora_SOURCE_DIR}/lib/gfx/${_file}.cpp")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/readback/${_file}.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_readback.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/efb_recording.inc" "${CMAKE_CURRENT_SOURCE_DIR}/efb_frame.inc" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_readback.py"
        "${_source}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources aurora_gx SOURCES)
    list(REMOVE_ITEM _sources "lib/gfx/${_file}.cpp" "${_source}")
    list(APPEND _sources "${_output}")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    set_source_files_properties("${_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
        COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/gfx;-iquote${aurora_SOURCE_DIR}/lib;-iquote${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
target_sources(aurora_gx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/efb_snapshot.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/efb_snapshot.cpp" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib")
set(_pipeline_source "${aurora_SOURCE_DIR}/lib/gfx/pipeline_cache.cpp")
set(_pipeline_output "${CMAKE_CURRENT_BINARY_DIR}/aurora-pipeline-cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline.py" "${_pipeline_source}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline.py"
    "${_pipeline_source}" "${_pipeline_output}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_sources aurora_gx SOURCES)
list(REMOVE_ITEM _sources lib/gfx/pipeline_cache.cpp "${_pipeline_source}")
list(APPEND _sources "${_pipeline_output}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
set_source_files_properties("${_pipeline_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/gfx")

add_executable(petari_gx_probe MACOSX_BUNDLE probe.cpp probe_backend.cpp probe_sync.cpp game_math.cpp model_scene.cpp texture_cache.cpp legacy_commands.cpp probe_heaps.cpp probe_watchdog.cpp probe_macos.m
    "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp")
set_target_properties(petari_gx_probe PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.petari.gx-probe"
    MACOSX_BUNDLE_BUNDLE_NAME "Petari GX Probe"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0")
target_compile_features(petari_gx_probe PRIVATE cxx_std_20)
target_include_directories(petari_gx_probe PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_gx_probe PRIVATE petari_j3d petari_core petari_heaps petari_memory aurora::core aurora::gx aurora::main petari_vi_bridge)

if(_petari_build_testing)
    add_executable(petari_gx_abi_tests abi_test.cpp backend_abi.cpp texture_cache.cpp)
    target_compile_features(petari_gx_abi_tests PRIVATE cxx_std_20)
    target_include_directories(petari_gx_abi_tests PRIVATE "${aurora_SOURCE_DIR}/lib")
    target_link_libraries(petari_gx_abi_tests PRIVATE petari_native_config petari_memory aurora::gx aurora::core petari_vi_bridge)
    add_test(NAME native_gx_abi COMMAND petari_gx_abi_tests)
endif()

file(GLOB PETARI_J3D_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/JSystem/J3DGraphBase/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/JSystem/J3DGraphAnimator/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/JSystem/J3DGraphLoader/*.cpp")
add_library(petari_j3d STATIC EXCLUDE_FROM_ALL ${PETARI_J3D_SOURCES} model_overrides.cpp
    "${CMAKE_SOURCE_DIR}/src/JSystem/JUtility/JUTNameTab.cpp"
    "${CMAKE_SOURCE_DIR}/src/JSystem/JUtility/JUTNativeResource.cpp"
    "${CMAKE_SOURCE_DIR}/src/JSystem/JUtility/JUTAssert.cpp")
petari_encode_target_sources(petari_j3d)
target_compile_options(petari_j3d PRIVATE -fno-rtti)
target_link_libraries(petari_j3d PUBLIC petari_kernel petari_core petari_resources aurora::gd aurora::gx aurora::core petari_vi_bridge)
if(_petari_build_testing)
    add_executable(petari_j3d_load_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/j3d_load_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp" legacy_commands.cpp)
    target_link_libraries(petari_j3d_load_tests PRIVATE petari_j3d)
    target_link_options(petari_j3d_load_tests PRIVATE -Wl,-dead_strip)
    add_executable(petari_j3d_anim_load_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/j3d_anim_load_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp" legacy_commands.cpp)
    target_link_libraries(petari_j3d_anim_load_tests PRIVATE petari_j3d)
    target_link_options(petari_j3d_anim_load_tests PRIVATE -Wl,-dead_strip)
    file(GLOB PETARI_JPA_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/JSystem/JParticle/*.cpp")
    add_executable(petari_jpa_load_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/jpa_load_tests.cpp" "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        legacy_commands.cpp ${PETARI_JPA_SOURCES}
        "${CMAKE_SOURCE_DIR}/src/JSystem/JUtility/JUTTexture.cpp"
        "${CMAKE_SOURCE_DIR}/src/JSystem/JUtility/JUTPalette.cpp"
        particle_overrides.cpp
        "${CMAKE_SOURCE_DIR}/src/Game/Util/MathUtil.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Util/MtxUtil.cpp")
    petari_encode_target_sources(petari_jpa_load_tests)
    target_link_libraries(petari_jpa_load_tests PRIVATE petari_j3d)
    target_link_options(petari_jpa_load_tests PRIVATE -Wl,-dead_strip)
    add_executable(petari_game_data_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/game_data_tests.cpp" "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/System/FileRipper.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Player/GhostPacket.cpp")
    petari_encode_target_sources(petari_game_data_tests)
    target_link_libraries(petari_game_data_tests PRIVATE petari_j3d petari_platform_dvd)
    add_executable(petari_speaker_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/speaker_tests.cpp" "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Speaker/SpkTable.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Speaker/SpkWave.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Speaker/SpkMixingBuffer.cpp"
        "${CMAKE_SOURCE_DIR}/src/JSystem/JAudio2/JASCalc.cpp")
    petari_encode_target_sources(petari_speaker_tests)
    target_link_libraries(petari_speaker_tests PRIVATE petari_j3d)
    target_link_options(petari_speaker_tests PRIVATE -Wl,-dead_strip)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_speaker_assets COMMAND petari_speaker_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
    endif()
endif()

add_library(petari_audio_sdl STATIC "${CMAKE_SOURCE_DIR}/native/audio/sdl_sink.cpp")
target_link_libraries(petari_audio_sdl PUBLIC petari_platform_audio PRIVATE SDL3::SDL3 petari_host_runtime)
if(_petari_build_testing)
    add_executable(petari_audio_sdl_tests "${CMAKE_SOURCE_DIR}/native/tests/audio_sdl_tests.cpp")
    target_link_libraries(petari_audio_sdl_tests PRIVATE petari_audio_sdl)
    add_test(NAME native_audio_sdl COMMAND petari_audio_sdl_tests)
    set_tests_properties(native_audio_sdl PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 15)
endif()

include("${CMAKE_SOURCE_DIR}/native/cmake/GameLibraries.cmake")
