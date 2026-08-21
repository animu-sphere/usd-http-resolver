# SPDX-License-Identifier: Apache-2.0
#
# The block-policy measurement: a sweep of block size against coalescing gap
# over the access patterns METRICS.md section 6 names.
#
# This is the fifth legal reverse edge in WORKSPACE.md section 2 -- a test
# outside libs/ that links both a backend and the fixture server -- and it is
# here for a reason the other four share and one they do not. The shared reason:
# a module's tests must not depend on anything outside libs/, or
# `ost library build libs/usd-asset-cache`, which builds the module alone, stops
# working. The reason of its own: the constants this measures are about round
# trips, and a sweep over a local file would be a sweep over a cost that does
# not exist there.
#
# It links the cache and the HTTP backend at once, which nothing else does. That
# is not the cache learning what a transport is -- it holds an AssetReader and
# nothing else -- it is a measurement of a stack, made where the stack is.

add_executable(usdAssetCache_tuning tuning_main.cpp)

target_link_libraries(usdAssetCache_tuning
    PRIVATE usdasset::cache usdasset::http usdasset::fixtureserver)
target_include_directories(usdAssetCache_tuning PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(usdAssetCache_tuning PRIVATE /utf-8 /W4)
    target_compile_definitions(usdAssetCache_tuning PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(usdAssetCache_tuning PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(COMMAND usd_http_resolver_stage_runtime_dependencies)
    usd_http_resolver_stage_runtime_dependencies(usdAssetCache_tuning)
endif()

# Registered as a test rather than kept as a tool, for the reason the baseline
# is: what it asserts is that every configuration in the sweep returns the right
# bytes, which is the cache running over a real socket at five block sizes and
# four gaps. The table it prints is the measurement, and it is recorded in
# docs/reference/BLOCK_POLICY.md.
add_test(NAME usdAssetCache_block_policy COMMAND usdAssetCache_tuning)
set_tests_properties(usdAssetCache_block_policy PROPERTIES TIMEOUT 900)

# A sanitizer lane runs the sweep for its assertions and not for its numbers,
# and 128 MiB of instrumented memcpy over loopback forty times is a lane that
# times out rather than a lane that measures.
if(USD_HTTP_RESOLVER_SANITIZER)
    set_tests_properties(usdAssetCache_block_policy PROPERTIES
        ENVIRONMENT "USD_ASSET_TUNING_ASSET_BYTES=8388608")
endif()
