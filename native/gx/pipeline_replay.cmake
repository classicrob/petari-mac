set(_replay_model "${CMAKE_CURRENT_BINARY_DIR}/pipeline-replay-model.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_pipeline_replay_model.py" "${CMAKE_SOURCE_DIR}/src/Game/Util/ModelUtil.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_pipeline_replay_model.py"
    "${CMAKE_SOURCE_DIR}/src/Game/Util/ModelUtil.cpp" "${_replay_model}" COMMAND_ERROR_IS_FATAL ANY)
set(_replay_init "${CMAKE_CURRENT_BINARY_DIR}/pipeline-replay-init.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_pipeline_replay_init.py"
    "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXManage.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_pipeline_replay_init.py"
    "${aurora_SOURCE_DIR}/lib/dolphin/gx/GXManage.cpp" "${_replay_init}" COMMAND_ERROR_IS_FATAL ANY)
# Offline config replay: no window, GPU initialization or live game launch.
add_executable(petari_pipeline_replay EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/native/tools/pipeline_replay.cpp"
    "${CMAKE_SOURCE_DIR}/native/tools/pipeline_replay_backend.cpp"
    "${CMAKE_SOURCE_DIR}/native/tools/pipeline_replay_layout.cpp"
    "${CMAKE_SOURCE_DIR}/native/tests/heap_diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/legacy_commands.cpp")
target_compile_features(petari_pipeline_replay PRIVATE cxx_std_20)
target_include_directories(petari_pipeline_replay PRIVATE "${aurora_SOURCE_DIR}/lib")
target_link_libraries(petari_pipeline_replay PRIVATE petari_game_libraries petari_sdk_mem SQLite::SQLite3)
target_link_options(petari_pipeline_replay PRIVATE -Wl,-dead_strip)

target_compile_options(petari_pipeline_replay PRIVATE -fno-rtti)

target_include_directories(petari_pipeline_replay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
