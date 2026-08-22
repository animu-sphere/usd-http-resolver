# SPDX-License-Identifier: Apache-2.0
#
# Module tests only.
#
# The read semantics live in the shared boundary suite under tests/boundary,
# where the cache is entered as a row over the local backend and runs the same
# cases unchanged. What is here is what the suite cannot ask: how many requests
# a pattern cost, how many bytes were moved that nobody asked for, and whether
# eight threads missing one block issue one request or eight.
#
# test_plan reaches into src/. The block arithmetic is internal -- putting it in
# an installed header would make it part of the module's surface -- and it is
# where the off-by-one lives, so it is tested directly rather than through a
# reader.

set(_usd_asset_cache_tests
    plan
    cache
    singleflight
    persistence)

foreach(_test ${_usd_asset_cache_tests})
    add_executable(usdAssetCache_test_${_test} test_${_test}.cpp)
    target_link_libraries(usdAssetCache_test_${_test} PRIVATE usdasset::cache)
    target_include_directories(usdAssetCache_test_${_test}
                               PRIVATE
                                   "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    if(MSVC)
        target_compile_options(usdAssetCache_test_${_test} PRIVATE /utf-8)
    endif()
    add_test(NAME usdAssetCache_${_test} COMMAND usdAssetCache_test_${_test})
endforeach()
