# SPDX-License-Identifier: Apache-2.0
#
# usdAssetIo -- the transport-independent core.
#
# This module names no transport, no cache, and no OpenUSD, and the build graph
# is where that is enforced rather than reviewed: there is no find_package here
# at all, so a dependency acquired by accident is a configure failure on every
# machine that runs the core path (WORKSPACE.md §2.1).

cmake_minimum_required(VERSION 3.23)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" _usd_asset_io_version
         LIMIT_COUNT 1)
    string(STRIP "${_usd_asset_io_version}" _usd_asset_io_version)
else()
    set(_usd_asset_io_version "0.1.0")
endif()

project(usdAssetIo
    VERSION ${_usd_asset_io_version}
    DESCRIPTION "Random-access read contract, diagnostics, and metrics"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED USDASSETIO_BUILD_TESTS)
    if(DEFINED USD_HTTP_RESOLVER_BUILD_TESTS)
        set(_usd_asset_io_tests_default "${USD_HTTP_RESOLVER_BUILD_TESTS}")
    else()
        set(_usd_asset_io_tests_default "${PROJECT_IS_TOP_LEVEL}")
    endif()
    option(USDASSETIO_BUILD_TESTS "Build usdAssetIo tests"
           ${_usd_asset_io_tests_default})
endif()

add_library(usdAssetIo STATIC
    src/Diagnostics.cpp
    src/Metrics.cpp
    src/RangeMath.cpp
    src/Validator.cpp)
add_library(usdasset::io ALIAS usdAssetIo)
set_target_properties(usdAssetIo PROPERTIES
    EXPORT_NAME io
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(usdAssetIo PUBLIC cxx_std_17)
target_include_directories(usdAssetIo
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

find_package(Threads REQUIRED)
target_link_libraries(usdAssetIo PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(usdAssetIo PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetIo PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS usdAssetIo
    EXPORT usdAssetIoTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/usdAssetIoConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetIoConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetIo")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetIoConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT usdAssetIoTargets
    FILE usdAssetIoTargets.cmake
    NAMESPACE usdasset::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetIo")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetIoConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetIoConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetIo")

if(USDASSETIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
