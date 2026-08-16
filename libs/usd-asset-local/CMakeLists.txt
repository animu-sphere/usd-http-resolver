# SPDX-License-Identifier: Apache-2.0
#
# usdAssetLocal -- the local-file backend, and the correctness oracle every
# other backend is compared against.
#
# It links usdasset::io and nothing else. A backend never depends on another
# backend, never depends on the cache, and never sees OpenUSD
# (WORKSPACE.md §2).

cmake_minimum_required(VERSION 3.23)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" _usd_asset_local_version
         LIMIT_COUNT 1)
    string(STRIP "${_usd_asset_local_version}" _usd_asset_local_version)
else()
    set(_usd_asset_local_version "0.1.0")
endif()

project(usdAssetLocal
    VERSION ${_usd_asset_local_version}
    DESCRIPTION "Local-file random-access backend"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED USDASSETLOCAL_BUILD_TESTS)
    if(DEFINED USD_HTTP_RESOLVER_BUILD_TESTS)
        set(_usd_asset_local_tests_default "${USD_HTTP_RESOLVER_BUILD_TESTS}")
    else()
        set(_usd_asset_local_tests_default "${PROJECT_IS_TOP_LEVEL}")
    endif()
    option(USDASSETLOCAL_BUILD_TESTS "Build usdAssetLocal tests"
           ${_usd_asset_local_tests_default})
endif()

# Standalone builds (`ost library build libs/usd-asset-local`) resolve the
# dependency; in-tree builds already have the target from the root's ordered
# library list.
if(NOT TARGET usdasset::io)
    find_package(usdAssetIo CONFIG REQUIRED)
endif()

add_library(usdAssetLocal STATIC
    src/LocalAssetReader.cpp
    src/PlatformFile.cpp)
add_library(usdasset::local ALIAS usdAssetLocal)
set_target_properties(usdAssetLocal PROPERTIES
    EXPORT_NAME local
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(usdAssetLocal PUBLIC cxx_std_17)
target_include_directories(usdAssetLocal
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usdAssetLocal PUBLIC usdasset::io)

if(MSVC)
    target_compile_options(usdAssetLocal PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetLocal PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS usdAssetLocal
    EXPORT usdAssetLocalTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/usdAssetLocalConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetLocalConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetLocal")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetLocalConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT usdAssetLocalTargets
    FILE usdAssetLocalTargets.cmake
    NAMESPACE usdasset::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetLocal")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetLocalConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetLocalConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetLocal")

if(USDASSETLOCAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
