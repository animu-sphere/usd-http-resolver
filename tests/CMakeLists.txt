# SPDX-License-Identifier: Apache-2.0
#
# Cross-module tests. A module's own tests live with the module; what lives here
# is what belongs to no single module: the shared boundary suite, and the
# hostile-server fixture corpus the HTTP backend is written against.
#
# The fixture server is added first because it exists first, and deliberately
# so: design policy §17 requires it to stand up before the backend, so that the
# backend is written against a passing oracle rather than debugged against a
# server.

add_subdirectory(fixture-server)
add_subdirectory(boundary)

# The corpus projection, added last because it is the only thing here that needs
# both of the above to exist: it is where the hostile server and the backend
# written against it finally meet. Guarded on the backend, so the tree still
# configures before a transport lands and after one is removed.
if(TARGET usdasset::http AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/CMakeLists.txt")
    add_subdirectory(corpus)
endif()
