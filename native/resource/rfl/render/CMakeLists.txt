# RVLFaceLib rendered through the native GX backend (Aurora/Metal), checked by
# GPU readback of the EFB.
#
# Root integration, after native/gx (needs Aurora, petari_vi_bridge, and the
# EFB snapshot patches) and native/resource/rfl:
#     add_subdirectory(native/resource/rfl/render)
# Run from the repository root in a graphical session:
#     petari_rfl_render_tests --assets build/game-data/RMGE01/files
include(FetchContent)
FetchContent_GetProperties(aurora)
if(NOT aurora_SOURCE_DIR)
    message(FATAL_ERROR "native/resource/rfl/render needs Aurora: add it after native/gx")
endif()
get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

set(_petari_rfl_render_aurora "${PETARI_REPO_ROOT}/native/tests/rfl_render_aurora.cpp")
add_executable(petari_rfl_render_tests
    "${PETARI_REPO_ROOT}/native/tests/rfl_render_tests.cpp"
    "${_petari_rfl_render_aurora}"
    "${PETARI_REPO_ROOT}/native/tests/heap_diagnostics.cpp")
target_compile_features(petari_rfl_render_tests PRIVATE cxx_std_20)
# Aurora's private headers are visible to the Aurora half only; the SDK half
# includes the RVL headers.
set_source_files_properties("${_petari_rfl_render_aurora}" PROPERTIES
    INCLUDE_DIRECTORIES "${aurora_SOURCE_DIR}/lib;${PETARI_REPO_ROOT}/native/gx")
target_link_libraries(petari_rfl_render_tests PRIVATE
    petari_rfl petari_resources petari_heaps petari_memory petari_vi_bridge petari_platform_nand
    aurora::core aurora::gx aurora::main)
# RFL references two functions the decompilation does not define
# (RFLiFormatAsync, RFLiIsSameFaceCore), on paths SMG never reaches.
target_link_options(petari_rfl_render_tests PRIVATE -Wl,-dead_strip)

if(BUILD_TESTING AND EXISTS "${PETARI_REPO_ROOT}/build/game-data/RMGE01/files/ObjectData/MiiFaceDatabase.arc")
    add_test(NAME native_rfl_render
        COMMAND petari_rfl_render_tests --assets "${PETARI_REPO_ROOT}/build/game-data/RMGE01/files"
        WORKING_DIRECTORY "${PETARI_REPO_ROOT}")
    # A window and a Metal device are required.
    set_tests_properties(native_rfl_render PROPERTIES LABELS gpu)
endif()
