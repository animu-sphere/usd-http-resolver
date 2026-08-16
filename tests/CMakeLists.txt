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
