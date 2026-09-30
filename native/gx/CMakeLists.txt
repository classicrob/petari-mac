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
# Create ImGui's device objects (its render pipeline) during Aurora initialization, before the
# game holds the CPU, instead of lazily on the first frame.
set(_imgui_source "${aurora_SOURCE_DIR}/lib/imgui.cpp")
set(_imgui_output "${CMAKE_CURRENT_BINARY_DIR}/pipeline-imgui.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline_imgui.py" "${_imgui_source}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline_imgui.py"
    "${_imgui_source}" "${_imgui_output}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_sources aurora_core SOURCES)
list(REMOVE_ITEM _sources lib/imgui.cpp "${_imgui_source}")
list(APPEND _sources "${_imgui_output}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_sources}")
set_source_files_properties("${_imgui_output}" TARGET_DIRECTORY aurora_core PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib")

set(_pipeline_source "${aurora_SOURCE_DIR}/lib/gfx/pipeline_cache.cpp")
set(_pipeline_output "${CMAKE_CURRENT_BINARY_DIR}/aurora-pipeline-cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline.py" "${_pipeline_source}"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_profile.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_runtime.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_worker.inc" "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_stage.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_global.inc")
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

    # The generated FIFO (patch_aurora_sync.py) with a fake command processor:
    # a compile burst inside a batch must not stop other game threads.
    set(_fifo_aurora_sources "${CMAKE_SOURCE_DIR}/native/tests/gx_fifo_processor_fake.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/sync/lib/gx/fifo.cpp")
    add_executable(petari_gx_fifo_processor_tests "${CMAKE_SOURCE_DIR}/native/tests/gx_fifo_processor_tests.cpp"
        ${_fifo_aurora_sources} sync_backend.cpp)
    target_compile_features(petari_gx_fifo_processor_tests PRIVATE cxx_std_20)
    set_source_files_properties(${_fifo_aurora_sources} PROPERTIES
        COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/gx"
        INCLUDE_DIRECTORIES "${aurora_SOURCE_DIR}/lib/gx;${aurora_SOURCE_DIR}/lib;${aurora_SOURCE_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR};$<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_INCLUDE_DIRECTORIES>")
    target_link_libraries(petari_gx_fifo_processor_tests PRIVATE petari_platform petari_host_runtime fmt::fmt)
    add_test(NAME native_gx_fifo_processor COMMAND petari_gx_fifo_processor_tests)
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
    add_executable(petari_j3d_material_anm_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/j3d_material_anm_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp" legacy_commands.cpp)
    target_link_libraries(petari_j3d_material_anm_tests PRIVATE petari_j3d)
    target_link_options(petari_j3d_material_anm_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_j3d_material_anm COMMAND petari_j3d_material_anm_tests)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_j3d_material_anm_assets COMMAND petari_j3d_material_anm_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_j3d_material_anm_corpus COMMAND petari_j3d_material_anm_tests
            --corpus "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
    endif()
    add_executable(petari_host_image_heap_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/host_image_heap_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp" legacy_commands.cpp)
    target_link_libraries(petari_host_image_heap_tests PRIVATE petari_j3d)
    target_link_options(petari_host_image_heap_tests PRIVATE -Wl,-dead_strip)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_host_image_heap COMMAND petari_host_image_heap_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        set_tests_properties(native_host_image_heap PROPERTIES TIMEOUT 60)
    endif()
    add_executable(petari_file_cache_heap_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/file_cache_heap_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/System/HeapMemoryWatcher.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Util/MemoryUtil.cpp")
    petari_encode_target_sources(petari_file_cache_heap_tests)
    target_link_libraries(petari_file_cache_heap_tests PRIVATE petari_j3d)
    target_link_options(petari_file_cache_heap_tests PRIVATE -Wl,-dead_strip)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_file_cache_heap_assets COMMAND petari_file_cache_heap_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        set_tests_properties(native_file_cache_heap_assets PROPERTIES TIMEOUT 120)
    endif()
    # Every scene archive's native resource-object cost on the file cache, and the per-stage
    # projection against the file cache's reserve (native/tests/file_cache_demand_tests.cpp).
    add_executable(petari_file_cache_demand_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/file_cache_demand_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/System/HeapMemoryWatcher.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Util/MemoryUtil.cpp")
    petari_encode_target_sources(petari_file_cache_demand_tests)
    target_link_libraries(petari_file_cache_demand_tests PRIVATE petari_j3d)
    target_link_options(petari_file_cache_demand_tests PRIVATE -Wl,-dead_strip)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_file_cache_demand COMMAND petari_file_cache_demand_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files"
            --stages "${CMAKE_SOURCE_DIR}/native/tests/data/file_cache_stage_archives.txt")
        set_tests_properties(native_file_cache_demand PROPERTIES TIMEOUT 300)
    endif()
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
    add_executable(petari_kcl_server_tests
        "${CMAKE_SOURCE_DIR}/native/tests/kcl_server_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Map/KCollision.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Map/KCollisionPlus.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Util/JMapInfo.cpp"
        "${CMAKE_SOURCE_DIR}/src/Game/Util/MathUtil.cpp"
        "${CMAKE_SOURCE_DIR}/src/JSystem/JGadget/hashcode.cpp")
    petari_encode_target_sources(petari_kcl_server_tests)
    target_link_libraries(petari_kcl_server_tests PRIVATE petari_j3d)
    target_link_options(petari_kcl_server_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_kcl_server COMMAND petari_kcl_server_tests)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_kcl_server_assets COMMAND petari_kcl_server_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
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

foreach(_file shader shader_info)
    set(_source "${aurora_SOURCE_DIR}/lib/gx/${_file}.cpp")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/indirect/${_file}.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_indirect_texcoord.py" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_indirect_texcoord.py"
        "${_source}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources aurora_gx SOURCES)
    list(REMOVE_ITEM _sources "lib/gx/${_file}.cpp" "${_source}")
    list(APPEND _sources "${_output}")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    set_source_files_properties("${_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
        COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/gx")
endforeach()

# Pipeline stage timing composes on the indirect-texcoord output for shader.cpp.
foreach(_pair "${CMAKE_CURRENT_BINARY_DIR}/indirect/shader.cpp|shader.cpp" "${aurora_SOURCE_DIR}/lib/gx/gx.cpp|gx.cpp")
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _source)
    list(GET _pair 1 _kind)
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/stages/${_kind}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline_stages.py" "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_profile.hpp" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_pipeline_stages.py"
        "${_source}" "${_output}" "${_kind}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources aurora_gx SOURCES)
    list(REMOVE_ITEM _sources "lib/gx/${_kind}" "${aurora_SOURCE_DIR}/lib/gx/${_kind}" "${_source}")
    list(APPEND _sources "${_output}")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    set_source_files_properties("${_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
        COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib/gx")
endforeach()

add_executable(petari_gx_shader_tests "${CMAKE_SOURCE_DIR}/native/tests/gx_shader_tests.cpp")
target_compile_features(petari_gx_shader_tests PRIVATE cxx_std_20)
target_include_directories(petari_gx_shader_tests PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_gx_shader_tests PRIVATE aurora::gx aurora::core)
target_link_options(petari_gx_shader_tests PRIVATE -Wl,-dead_strip)
if(_petari_build_testing)
    add_test(NAME native_gx_shader COMMAND petari_gx_shader_tests)
endif()

add_executable(petari_texture_format_tests "${CMAKE_SOURCE_DIR}/native/tools/texture_format_tests.cpp")
target_compile_features(petari_texture_format_tests PRIVATE cxx_std_20)
target_include_directories(petari_texture_format_tests PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_texture_format_tests PRIVATE aurora::gx aurora::core)
target_link_options(petari_texture_format_tests PRIVATE -Wl,-dead_strip)
if(_petari_build_testing)
    add_test(NAME native_texture_format COMMAND petari_texture_format_tests)
endif()

add_executable(petari_pipeline_cache_inspect EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/native/tools/pipeline_cache_inspect.cpp")
target_compile_features(petari_pipeline_cache_inspect PRIVATE cxx_std_20)
target_include_directories(petari_pipeline_cache_inspect PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_pipeline_cache_inspect PRIVATE aurora::gx aurora::core SQLite::SQLite3)
target_link_options(petari_pipeline_cache_inspect PRIVATE -Wl,-dead_strip)

if(_petari_build_testing AND EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files/LayoutData/GalaxyNamePlate.arc")
    add_test(NAME native_galaxy_name_plate COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/native/tests/galaxy_name_plate_tests.py"
        "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files/LayoutData/GalaxyNamePlate.arc")
endif()

if(_petari_build_testing)
    # ImGui device objects must be created during Aurora initialization, not on the first frame.
    if(DEFINED imgui_SOURCE_DIR)
        set(_petari_imgui_wgpu "${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp")
    else()
        set(_petari_imgui_wgpu "${CMAKE_SOURCE_DIR}/build/aurora/_deps/imgui-src/backends/imgui_impl_wgpu.cpp")
    endif()
    if(EXISTS "${_petari_imgui_wgpu}")
        add_test(NAME native_pipeline_imgui COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/native/tools/pipeline_imgui_tests.py"
            --aurora "${aurora_SOURCE_DIR}" --backend "${_petari_imgui_wgpu}")
        set_tests_properties(native_pipeline_imgui PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 60)
    endif()

    # These regressions use synthetic inputs and CPU stubs, without disc data or a GPU.
    find_package(Threads REQUIRED)
    foreach(_kind worker stage seed_sort)
        add_executable(petari_pipeline_${_kind}_tests
            "${CMAKE_SOURCE_DIR}/native/tools/pipeline_${_kind}_tests.cpp")
        target_compile_features(petari_pipeline_${_kind}_tests PRIVATE cxx_std_20)
        target_include_directories(petari_pipeline_${_kind}_tests PRIVATE "${CMAKE_SOURCE_DIR}/native/include")
        target_link_libraries(petari_pipeline_${_kind}_tests PRIVATE Threads::Threads)
    endforeach()
    target_link_libraries(petari_pipeline_stage_tests PRIVATE SQLite::SQLite3)
    target_link_libraries(petari_pipeline_seed_sort_tests PRIVATE SQLite::SQLite3)

    add_test(NAME native_pipeline_worker COMMAND "${CMAKE_COMMAND}" -E env
        --unset=PETARI_PIPELINE_POLICY --unset=PETARI_PIPELINE_GLOBAL_PRECOMPILE
        $<TARGET_FILE:petari_pipeline_worker_tests>)
    add_test(NAME native_pipeline_worker_async COMMAND "${CMAKE_COMMAND}" -E env
        --unset=PETARI_PIPELINE_GLOBAL_PRECOMPILE PETARI_PIPELINE_POLICY=async
        $<TARGET_FILE:petari_pipeline_worker_tests> async)
    add_test(NAME native_pipeline_stage COMMAND petari_pipeline_stage_tests)
    add_test(NAME native_pipeline_seed COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/native/tools/pipeline_seed_tests.py" --aurora "${aurora_SOURCE_DIR}")
    add_test(NAME native_pipeline_replay COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/native/tools/pipeline_replay_tests.py")
    add_test(NAME native_pipeline_seed_sort COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/native/tools/pipeline_seed_sort_tests.py"
        $<TARGET_FILE:petari_pipeline_seed_sort_tests>)
    set_tests_properties(native_pipeline_worker native_pipeline_worker_async native_pipeline_stage
        native_pipeline_seed native_pipeline_replay native_pipeline_seed_sort PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 60)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/pipeline_replay.cmake")

# Frame telemetry: first-use GX texture hashing and static texture creation.
foreach(_relative lib/gx/texture.cpp lib/gfx/texture.cpp)
    set(_source "${aurora_SOURCE_DIR}/${_relative}")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/texture-timing/${_relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_texture_timing.py" "${_source}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_texture_timing.py"
        "${_source}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources aurora_gx SOURCES)
    list(REMOVE_ITEM _sources "${_relative}" "${_source}")
    list(APPEND _sources "${_output}")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    get_filename_component(_original_dir "${_source}" DIRECTORY)
    set_source_files_properties("${_output}" TARGET_DIRECTORY aurora_gx PROPERTIES
        COMPILE_OPTIONS "-iquote${_original_dir};-iquote${aurora_SOURCE_DIR}/lib")
endforeach()

if(_petari_build_testing)
    include("${CMAKE_SOURCE_DIR}/native/tests/asset_reference_sweep.cmake")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/pipeline_owners.cmake")
