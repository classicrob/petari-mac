add_test(NAME native_asset_reference_sweep
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/native/tests/asset_reference_sweep_tests.py")
set_tests_properties(native_asset_reference_sweep PROPERTIES TIMEOUT 60)

add_test(NAME native_layout_asset_guards
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/native/tests/layout_asset_guard_tests.py")
set_tests_properties(native_layout_asset_guards PROPERTIES TIMEOUT 120)
