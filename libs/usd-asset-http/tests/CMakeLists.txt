# SPDX-License-Identifier: Apache-2.0
#
# Module tests only.
#
# The read semantics live in the shared boundary suite under tests/boundary,
# because they belong to the contract rather than to this backend, and the
# hostile server behaviors live in tests/corpus, because they need a server.
# What is here is what needs neither: the URI arithmetic a redirect depends on,
# the framing rules as pure functions of a header table, and the protocol
# policies over a scripted transport.
#
# These tests reach into src/. The transport seam is internal (ADR-0003), and a
# testing hook in an installed header would make it part of the module's
# surface -- which is how an internal interface becomes one that cannot be
# replaced without a deprecation.

set(_usd_asset_http_tests
    framing
    protocol
    uri)

foreach(_test ${_usd_asset_http_tests})
    add_executable(usdAssetHttp_test_${_test} test_${_test}.cpp)
    target_link_libraries(usdAssetHttp_test_${_test} PRIVATE usdasset::http)
    target_include_directories(usdAssetHttp_test_${_test}
                               PRIVATE
                                   "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    if(MSVC)
        target_compile_options(usdAssetHttp_test_${_test} PRIVATE /utf-8)
    endif()
    if(COMMAND usd_http_resolver_stage_runtime_dependencies)
        usd_http_resolver_stage_runtime_dependencies(usdAssetHttp_test_${_test})
    endif()
    add_test(NAME usdAssetHttp_${_test} COMMAND usdAssetHttp_test_${_test})
endforeach()
