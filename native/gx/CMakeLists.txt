include(FetchContent)
set(CMAKE_FIND_FRAMEWORK LAST)
find_package(SQLite3 3.37 REQUIRED GLOBAL)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
FetchContent_Declare(aurora
    GIT_REPOSITORY https://github.com/encounter/aurora.git
    GIT_TAG 08122911e8621acb7ded6563813b264bec1494b5)
FetchContent_MakeAvailable(aurora)

add_executable(petari_gx_probe probe.cpp game_math.cpp)
target_compile_features(petari_gx_probe PRIVATE cxx_std_20)
target_link_libraries(petari_gx_probe PRIVATE petari_core aurora::core aurora::gx aurora::main aurora::vi)
