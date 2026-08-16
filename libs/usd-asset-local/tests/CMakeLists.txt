# SPDX-License-Identifier: Apache-2.0
#
# Module tests only. The read semantics live in the shared boundary suite under
# tests/boundary, because they belong to the contract rather than to this
# backend.

set(_usd_asset_local_tests
    metrics
    open)

foreach(_test ${_usd_asset_local_tests})
    add_executable(usdAssetLocal_test_${_test} test_${_test}.cpp)
    target_link_libraries(usdAssetLocal_test_${_test} PRIVATE usdasset::local)
    target_include_directories(usdAssetLocal_test_${_test}
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(usdAssetLocal_test_${_test} PRIVATE /utf-8)
        target_compile_definitions(usdAssetLocal_test_${_test}
                                   PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME usdAssetLocal_${_test} COMMAND usdAssetLocal_test_${_test})
endforeach()
