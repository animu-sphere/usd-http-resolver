# SPDX-License-Identifier: Apache-2.0
#
# The hostile-server fixture corpus.
#
# It links nothing. Not usdasset::io, not a backend, and above all not the HTTP
# client: this is the *server* side, and acquiring a second third-party
# dependency to test the one ADR-0003 chose on stated criteria would be a
# dependency admitted without an argument ever being made. Sockets and the
# standard library are the whole of it.
#
# It is also why this lives under tests/ rather than libs/: it is not a module
# of the product, it has no consumer, and it ships in nothing.

add_library(usdAssetFixtureServer STATIC
    src/Corpus.cpp
    src/Http.cpp
    src/Server.cpp
    src/Socket.cpp)
add_library(usdasset::fixtureserver ALIAS usdAssetFixtureServer)

target_compile_features(usdAssetFixtureServer PUBLIC cxx_std_17)
target_include_directories(usdAssetFixtureServer
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)
target_link_libraries(usdAssetFixtureServer PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(usdAssetFixtureServer PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(usdAssetFixtureServer PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetFixtureServer PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(tests)
