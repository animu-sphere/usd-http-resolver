# SPDX-License-Identifier: Apache-2.0
#
# These suites report through `CHECK`, which is deliberate rather than a
# framework that was not chosen: `CHECK` reports every failing row instead of
# aborting on the first, and a boundary table is only readable when it does.

set(_usd_asset_io_tests
    diagnostics
    metrics
    range_math
    validator)

foreach(_test ${_usd_asset_io_tests})
    add_executable(usdAssetIo_test_${_test} test_${_test}.cpp)
    target_link_libraries(usdAssetIo_test_${_test} PRIVATE usdasset::io)
    target_include_directories(usdAssetIo_test_${_test}
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(usdAssetIo_test_${_test} PRIVATE /utf-8)
    endif()
    add_test(NAME usdAssetIo_${_test} COMMAND usdAssetIo_test_${_test})
endforeach()
