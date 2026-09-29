# THP movie validation (petari_movie) and its component tests.
#
# Included from the root build: defines petari_movie, and petari_movie_tests when
# testing is enabled and aurora::thp exists.
# Standalone (cmake -S native/movie): builds the same library and tests against
# Aurora's THP decoder sources from PETARI_AURORA_DIR, with sanitizers, without
# the GX backend.
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PetariMovie LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
    set(PETARI_MOVIE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    set(PETARI_AURORA_DIR "${PETARI_MOVIE_ROOT}/../aurora-reference" CACHE PATH "Aurora source tree")
    find_path(PETARI_FMT_INCLUDE fmt/format.h HINTS /opt/homebrew/opt/fmt/include REQUIRED)
    option(PETARI_MOVIE_SANITIZERS "Build with address and undefined behavior sanitizers" ON)
    if(PETARI_MOVIE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    # Aurora's THP decoder only needs Aurora headers and header-only fmt.
    add_library(aurora_thp_standalone STATIC
        "${PETARI_AURORA_DIR}/lib/dolphin/thp/THPAudio.cpp"
        "${PETARI_AURORA_DIR}/lib/dolphin/thp/THPDec.cpp")
    set_target_properties(aurora_thp_standalone PROPERTIES CXX_STANDARD 20)
    target_compile_definitions(aurora_thp_standalone PRIVATE AURORA TARGET_PC FMT_HEADER_ONLY)
    target_include_directories(aurora_thp_standalone PRIVATE
        "${PETARI_AURORA_DIR}/include" "${PETARI_AURORA_DIR}/lib" "${PETARI_FMT_INCLUDE}")
    add_library(aurora::thp ALIAS aurora_thp_standalone)
    enable_testing()
    set(BUILD_TESTING ON)

    # Native platform services for the wrapper integration test, as the root
    # build defines them (raw headers; the wrapper needs no legacy transformation).
    project(PetariMoviePlatform LANGUAGES C CXX)
    add_library(petari_native_config INTERFACE)
    target_compile_definitions(petari_native_config INTERFACE PETARI_NATIVE=1)
    target_include_directories(petari_native_config INTERFACE
        "${PETARI_MOVIE_ROOT}/native/include" "${PETARI_MOVIE_ROOT}/include"
        "${PETARI_MOVIE_ROOT}/libs/RVL_SDK/include" "${PETARI_MOVIE_ROOT}/libs/JSystem/include"
        "${PETARI_MOVIE_ROOT}/libs/nw4r/include")
    target_compile_options(petari_native_config INTERFACE
        -fno-strict-aliasing -ffp-contract=off -fcheck-new -Werror=return-type -Wno-register)
    add_library(petari_host_runtime STATIC "${PETARI_MOVIE_ROOT}/native/src/host_allocation.cpp")
    target_link_libraries(petari_host_runtime PUBLIC petari_native_config)
    add_library(petari_memory STATIC "${PETARI_MOVIE_ROOT}/native/memory/arena.cpp"
        "${PETARI_MOVIE_ROOT}/src/RVL_SDK/os/OSAlloc.c")
    set_source_files_properties("${PETARI_MOVIE_ROOT}/src/RVL_SDK/os/OSAlloc.c" PROPERTIES LANGUAGE CXX)
    target_link_libraries(petari_memory PUBLIC petari_native_config petari_host_runtime)
    # native/platform's root-integration-only targets (not built here) call the
    # root's legacy-source helper; their sources need no transformation here.
    if(NOT COMMAND petari_encode_target_sources)
        function(petari_encode_target_sources target)
        endfunction()
    endif()
    add_subdirectory("${PETARI_MOVIE_ROOT}/native/platform" platform EXCLUDE_FROM_ALL)

    add_executable(petari_movie_wrapper_tests wrapper_tests.cpp
        "${PETARI_MOVIE_ROOT}/src/Game/Screen/THPSimplePlayerWrapper.cpp"
        "${PETARI_MOVIE_ROOT}/src/Game/LiveActor/Nerve.cpp"
        "${PETARI_MOVIE_ROOT}/src/Game/LiveActor/Spine.cpp"
        "${PETARI_MOVIE_ROOT}/src/Game/LiveActor/ActorStateKeeper.cpp"
        "${PETARI_MOVIE_ROOT}/src/Game/System/NerveExecutor.cpp"
        "${PETARI_MOVIE_ROOT}/src/Game/Util/NerveUtil.cpp")
    target_link_libraries(petari_movie_wrapper_tests PRIVATE
        petari_native_config petari_movie aurora::thp petari_platform_dvd petari_platform_os)
    target_link_options(petari_movie_wrapper_tests PRIVATE -Wl,-dead_strip)
    add_test(NAME native_movie_wrapper COMMAND petari_movie_wrapper_tests)
    set_tests_properties(native_movie_wrapper PROPERTIES ENVIRONMENT "PETARI_SOURCE_DIR=${PETARI_MOVIE_ROOT}")
else()
    set(PETARI_MOVIE_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_library(petari_movie STATIC thp_stream.cpp thp_video_check.cpp)
target_include_directories(petari_movie PUBLIC "${PETARI_MOVIE_ROOT}/native/include")

if(BUILD_TESTING AND TARGET aurora::thp)
    add_executable(petari_movie_tests "${PETARI_MOVIE_ROOT}/native/tests/movie_tests.cpp")
    target_link_libraries(petari_movie_tests PRIVATE petari_movie aurora::thp)
    # CTest: synthetic checks, then the first 600 frames of every movie validated
    # and their audio decoded, with a bounded video sample. Without --frame-limit
    # every frame is validated; --all-video also decodes every video frame.
    add_test(NAME native_movie COMMAND petari_movie_tests --frame-limit 600)
    set_tests_properties(native_movie PROPERTIES ENVIRONMENT "PETARI_SOURCE_DIR=${PETARI_MOVIE_ROOT}")
endif()
