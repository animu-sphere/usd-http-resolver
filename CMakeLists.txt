# Libs-first root for the usd-http-resolver workspace.
#
# The build graph mirrors the dependency boundary fixed in
# docs/architecture/WORKSPACE.md: everything under libs/ is transport- and
# OpenUSD-agnostic and builds with plain CMake alone; only the OpenStrata
# plugin bundles under plugins/ need an OpenUSD runtime. OpenUSD is therefore
# resolved *inside* the plugin branch, never at the top of this file.
#
#   # core only -- no OpenUSD installation required
#   cmake -S . -B build-core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
#   cmake --build build-core
#   ctest --test-dir build-core
#
#   # everything, including the ArResolver bundle
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<your-openusd-install>
#   cmake --build build
#
# `ost` builds each bundle individually (`ost plugin build <bundle>`),
# composing the resolved runtime's environment and toolchain. Each bundle
# guards its own `find_package(pxr)` with `if(NOT pxr_FOUND)`, so it builds
# either standalone (via `ost`) or as a subdirectory of this root.
cmake_minimum_required(VERSION 3.23)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _usd_http_resolver_version
     LIMIT_COUNT 1)
string(STRIP "${_usd_http_resolver_version}" _usd_http_resolver_version)

project(usd-http-resolver
        VERSION ${_usd_http_resolver_version}
        LANGUAGES CXX)

option(USD_HTTP_RESOLVER_BUILD_PLUGIN
       "Build the OpenUSD ArResolver plugin bundles under plugins/" ON)
option(USD_HTTP_RESOLVER_BUILD_TESTS
       "Build the module tests and the shared boundary suite" ON)

enable_testing()

# --- sanitizers --------------------------------------------------------------
#
# Sanitizer builds are part of the boundary-suite contract, not an optional lane
# (docs/contributing/BOUNDARY_SUITE.md §5). The concurrency properties in §7 of
# the design policy are only actually verified under ThreadSanitizer; asserting
# them in prose asserts nothing.
#
# The flags are set here rather than per module so that a sanitizer build covers
# the libraries and the suite that drives them with one switch:
#
#   cmake -S . -B build/core-asan -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF \
#         -DCMAKE_BUILD_TYPE=Debug -DUSD_HTTP_RESOLVER_SANITIZER=address,undefined
#
# This is also the practical argument for the libs-first graph: these builds
# need no USD runtime, so they run on every platform in CI rather than being
# skipped on the ones without one.
set(USD_HTTP_RESOLVER_SANITIZER "" CACHE STRING
    "Sanitizers to build libs/ and tests/ with: address, undefined, thread, or a comma-separated combination")

if(USD_HTTP_RESOLVER_SANITIZER)
    if(MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        # cl.exe implements /fsanitize=address and nothing else. UBSan and TSan
        # are a clang or GCC lane; saying so is better than emitting flags that
        # are silently ignored and a green build that checked nothing.
        if(NOT USD_HTTP_RESOLVER_SANITIZER STREQUAL "address")
            message(FATAL_ERROR
                    "USD_HTTP_RESOLVER_SANITIZER='${USD_HTTP_RESOLVER_SANITIZER}': "
                    "MSVC supports only 'address'. Use clang or GCC for "
                    "undefined and thread.")
        endif()
        # Incremental linking and ASan are incompatible on MSVC, and the
        # failure is not a link error: the binary builds, then dies at startup
        # with STATUS_DLL_INIT_FAILED before main. CMake's Debug defaults turn
        # incremental linking on, so it has to be turned back off here.
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
        # The ASan runtime is a DLL in the MSVC toolchain's bin directory,
        # which is on PATH inside a developer prompt and nowhere else.
        #
        # This lane is unverified. On MSVC 19.34 (VS 17.4) the instrumented
        # binaries build and then exit with STATUS_DLL_INIT_FAILED before main,
        # with the runtime both on PATH and beside the executable, under Debug
        # and RelWithDebInfo alike. The sanitizer evidence this project relies
        # on comes from the clang and GCC lanes, which also carry UBSan and
        # TSan; this branch exists so that a newer MSVC can be tried without
        # editing the build.
        message(WARNING
                "usd-http-resolver: the MSVC AddressSanitizer lane is unverified; "
                "run sanitizers under clang or GCC")
    else()
        # -fno-sanitize-recover is not a hardening extra: without it the
        # UndefinedBehaviorSanitizer lane is decorative. UBSan's default is to
        # print the violation to stderr and *continue*, so a signed-overflow in
        # the offset arithmetic this project exists to get right leaves the
        # process exiting 0, CTest reporting a pass, and the report scrolling
        # past in the log. Measured: an int overflow under
        # `-fsanitize=address,undefined` exits 0; the same binary with this flag
        # exits 1. ASan already aborts and TSan already sets a non-zero exit
        # code when it has reported, so `=all` changes only the lane that needed
        # it.
        add_compile_options(-fsanitize=${USD_HTTP_RESOLVER_SANITIZER}
                            -fno-sanitize-recover=all
                            -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${USD_HTTP_RESOLVER_SANITIZER})
    endif()
    message(STATUS
            "usd-http-resolver: sanitizers '${USD_HTTP_RESOLVER_SANITIZER}'")
endif()

# --- libs/ -------------------------------------------------------------------
#
# Always built, never dependent on OpenUSD. The list is the landing order from
# the roadmap, not a directory listing: a module is added only once it exists,
# because a directory is created when its first tested capability exists
# (WORKSPACE.md invariant 11). The glob below adds nothing -- it exists so that
# `CONFIGURE_DEPENDS` re-runs configuration when a new module lands.
file(GLOB _usd_http_resolver_lib_stamps CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/libs/*/CMakeLists.txt")

set(_usd_http_resolver_libs
    usd-asset-io
    usd-asset-local
    usd-asset-http
    usd-asset-cache)

foreach(_lib ${_usd_http_resolver_libs})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_lib}/CMakeLists.txt")
        message(STATUS "usd-http-resolver: adding library 'libs/${_lib}'")
        add_subdirectory("libs/${_lib}")
    endif()
endforeach()

# --- tests/ ------------------------------------------------------------------
#
# The shared boundary suite lives here rather than under a module because it
# belongs to no single backend: it is the executable form of the read contract,
# and every backend is admitted by it unchanged
# (docs/contributing/BOUNDARY_SUITE.md). Added before the plugin branch below,
# which returns early.
if(USD_HTTP_RESOLVER_BUILD_TESTS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# --- plugins/ ----------------------------------------------------------------
#
# The only branch that requires OpenUSD. A bundle is any directory containing
# both an openstrata.plugin.yaml and a CMakeLists.txt, either an immediate
# subdirectory of this root or one under plugins/. `CONFIGURE_DEPENDS` makes the
# build re-glob when a bundle is added or removed, so a later `cmake --build`
# picks up new `ost plugin new` bundles without a manual reconfigure.
if(NOT USD_HTTP_RESOLVER_BUILD_PLUGIN)
    message(STATUS
            "usd-http-resolver: plugin bundles disabled; OpenUSD is not required")
    return()
endif()

if(NOT pxr_FOUND)
    find_package(pxr CONFIG REQUIRED)
endif()

set(_ost_bundle_roots "${CMAKE_CURRENT_SOURCE_DIR}")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
    list(APPEND _ost_bundle_roots "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
endif()

foreach(_root ${_ost_bundle_roots})
    file(GLOB _entries LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE
         "${_root}" "${_root}/*")
    foreach(_entry ${_entries})
        set(_bundle "${_root}/${_entry}")
        if(IS_DIRECTORY "${_bundle}"
           AND EXISTS "${_bundle}/openstrata.plugin.yaml"
           AND EXISTS "${_bundle}/CMakeLists.txt")
            file(RELATIVE_PATH _rel_bundle "${CMAKE_CURRENT_SOURCE_DIR}" "${_bundle}")
            message(STATUS "OpenStrata: adding plugin bundle '${_rel_bundle}'")
            add_subdirectory("${_rel_bundle}")
        endif()
    endforeach()
endforeach()
