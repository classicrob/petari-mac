# Wii SDK MEM heap library (expanded heap, heap lists, allocator), compiled from
# the SDK's own sources with native pointer-width support.
#
# Root integration: add_subdirectory(native/memory/sdk_mem) after native/platform.
# Uses petari_native_config and petari_platform_os (OSMutex), which the root
# build defines; target names resolve at generate time.
get_filename_component(PETARI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(PETARI_SDK_MEM_SOURCES
    ${PETARI_REPO_ROOT}/src/RVL_SDK/mem/mem_heapCommon.c
    ${PETARI_REPO_ROOT}/src/RVL_SDK/mem/mem_list.c
    ${PETARI_REPO_ROOT}/src/RVL_SDK/mem/mem_expHeap.c
    ${PETARI_REPO_ROOT}/src/RVL_SDK/mem/mem_allocator.c)
add_library(petari_sdk_mem STATIC ${PETARI_SDK_MEM_SOURCES})
set_source_files_properties(${PETARI_SDK_MEM_SOURCES} PROPERTIES LANGUAGE CXX)
target_link_libraries(petari_sdk_mem PUBLIC petari_native_config petari_platform_os)
target_compile_features(petari_sdk_mem PUBLIC cxx_std_17)

if(BUILD_TESTING)
    add_executable(petari_sdk_mem_tests ${PETARI_REPO_ROOT}/native/tests/sdk_mem_tests.cpp)
    target_link_libraries(petari_sdk_mem_tests PRIVATE petari_sdk_mem petari_boot petari_platform_sc petari_platform_nand)
    add_test(NAME native_sdk_mem COMMAND petari_sdk_mem_tests)
endif()
