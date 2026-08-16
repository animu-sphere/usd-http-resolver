# SPDX-License-Identifier: Apache-2.0
#
# The corpus checked against itself, before any backend exists to point at it.
#
# This target reaches into the server's private src/ for the socket layer, and
# only for that. `send` is not the thing under test; HTTP framing is, and the
# client here parses it with its own code so that the two disagree when one is
# wrong -- the same separation the boundary suite keeps between its oracle and
# usdAssetLocal.

add_executable(usdAssetFixture_test_corpus
    RawClient.cpp
    test_corpus.cpp)

target_link_libraries(usdAssetFixture_test_corpus PRIVATE usdasset::fixtureserver)
target_include_directories(usdAssetFixture_test_corpus PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

if(MSVC)
    target_compile_options(usdAssetFixture_test_corpus PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetFixture_test_corpus PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME usdAssetFixture_corpus COMMAND usdAssetFixture_test_corpus)

# Two deliberate stalls of half a second each, plus a redirect chain and a
# concurrency sweep. The default 1500s CTest timeout is far more than this
# needs; the point of naming one is that a *hung* fixture fails the lane rather
# than holding it, which is the failure mode a server full of sleeps has.
set_tests_properties(usdAssetFixture_corpus PROPERTIES TIMEOUT 120)
