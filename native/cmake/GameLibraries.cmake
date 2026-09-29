# Source libraries used by the full game. Hardware services come from native/platform.
file(GLOB_RECURSE PETARI_RUNTIME_LIBRARY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/JSystem/*.cpp" "${CMAKE_SOURCE_DIR}/src/nw4r/*.cpp")
list(FILTER PETARI_RUNTIME_LIBRARY_SOURCES EXCLUDE REGEX "/JSystem/(JKernel|JMath|J3DGraphBase|J3DGraphAnimator|J3DGraphLoader)/")
list(FILTER PETARI_RUNTIME_LIBRARY_SOURCES EXCLUDE REGEX "/JASSeqReader\\.cpp$")
list(FILTER PETARI_RUNTIME_LIBRARY_SOURCES EXCLUDE REGEX "/JSU(List|InputStream|FileStream)\\.cpp$")
list(FILTER PETARI_RUNTIME_LIBRARY_SOURCES EXCLUDE REGEX "/JUT(NameTab|NativeResource|Assert)\\.cpp$")
add_library(petari_game_libraries STATIC EXCLUDE_FROM_ALL ${PETARI_RUNTIME_LIBRARY_SOURCES}
    "${CMAKE_SOURCE_DIR}/native/gx/particle_overrides.cpp")
petari_encode_target_sources(petari_game_libraries)
target_compile_options(petari_game_libraries PRIVATE -fno-rtti)
target_link_libraries(petari_game_libraries PUBLIC petari_j3d petari_input petari_platform)

# Force the real game entry to remain reachable even though this diagnostic does not run it.
add_executable(petari_link_check EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/native/src/link_check.cpp"
    "${CMAKE_SOURCE_DIR}/native/gx/legacy_commands.cpp"
    "${CMAKE_SOURCE_DIR}/native/gx/texture_cache.cpp")
target_link_libraries(petari_link_check PRIVATE petari_game_objects petari_game_libraries petari_audio_sdl)
target_link_options(petari_link_check PRIVATE -Wl,-dead_strip -Wl,-u,_petari_game_main)

add_library(petari_sdk_helpers STATIC
    "${CMAKE_SOURCE_DIR}/src/RVL_SDK/wenc/wenc.c"
    "${CMAKE_SOURCE_DIR}/src/Game/Screen/THPDraw.c")
petari_encode_target_sources(petari_sdk_helpers)
set_source_files_properties(
    "${PETARI_LEGACY_ROOT}/src/RVL_SDK/wenc/wenc.c"
    "${PETARI_LEGACY_ROOT}/src/Game/Screen/THPDraw.c"
    PROPERTIES LANGUAGE CXX)
target_link_libraries(petari_sdk_helpers PUBLIC petari_native_config aurora::thp)
target_link_libraries(petari_link_check PRIVATE petari_sdk_helpers)

if(_petari_build_testing)
    add_executable(petari_jaudio_resource_tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/native/tests/jaudio_resource_tests.cpp"
        "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp")
    petari_encode_target_sources(petari_jaudio_resource_tests)
    target_compile_options(petari_jaudio_resource_tests PRIVATE -fno-rtti)
    target_link_libraries(petari_jaudio_resource_tests PRIVATE petari_game_libraries petari_resources)
    target_link_options(petari_jaudio_resource_tests PRIVATE -Wl,-dead_strip)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
        add_test(NAME native_jaudio_resources_assets COMMAND petari_jaudio_resource_tests
            --assets "${CMAKE_SOURCE_DIR}/build/game-data/RMGE01/files")
    endif()
endif()
