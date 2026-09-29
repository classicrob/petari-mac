find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(PETARI_LEGACY_ROOT "${CMAKE_BINARY_DIR}/legacy-sjis")
set(_petari_encoding_script "${CMAKE_SOURCE_DIR}/native/tools/encode_legacy_sources.py")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_petari_encoding_script}"
        --root "${CMAKE_SOURCE_DIR}" --output "${PETARI_LEGACY_ROOT}"
    RESULT_VARIABLE _petari_encoding_result)
if(NOT _petari_encoding_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare Shift-JIS legacy sources")
endif()
file(GLOB_RECURSE _petari_legacy_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.c" "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.h" "${CMAKE_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/*.h" "${CMAKE_SOURCE_DIR}/include/*.hpp" "${CMAKE_SOURCE_DIR}/include/*.inc"
    "${CMAKE_SOURCE_DIR}/libs/*.c" "${CMAKE_SOURCE_DIR}/libs/*.cpp" "${CMAKE_SOURCE_DIR}/libs/*.h" "${CMAKE_SOURCE_DIR}/libs/*.hpp"
    "${CMAKE_SOURCE_DIR}/libs/*.inc")
set(_petari_encoding_stamp "${PETARI_LEGACY_ROOT}/.prepared")
file(TOUCH "${_petari_encoding_stamp}")
add_custom_command(OUTPUT "${_petari_encoding_stamp}"
    COMMAND "${Python3_EXECUTABLE}" "${_petari_encoding_script}"
        --root "${CMAKE_SOURCE_DIR}" --output "${PETARI_LEGACY_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_petari_encoding_stamp}"
    DEPENDS "${_petari_encoding_script}" ${_petari_legacy_inputs}
    COMMENT "Updating Shift-JIS literals in generated native source inputs")
add_custom_target(petari_legacy_sources DEPENDS "${_petari_encoding_stamp}")

function(petari_encode_target_sources target)
    get_target_property(_sources "${target}" SOURCES)
    set(_encoded)
    foreach(_source IN LISTS _sources)
        if(IS_ABSOLUTE "${_source}")
            file(RELATIVE_PATH _relative "${CMAKE_SOURCE_DIR}" "${_source}")
        else()
            set(_relative "${_source}")
        endif()
        if(_relative MATCHES "^src/")
            list(APPEND _encoded "${PETARI_LEGACY_ROOT}/${_relative}")
        else()
            list(APPEND _encoded "${_source}")
        endif()
    endforeach()
    set_property(TARGET "${target}" PROPERTY SOURCES "${_encoded}")
    add_dependencies("${target}" petari_legacy_sources)
endfunction()
