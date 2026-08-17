// SPDX-License-Identifier: Apache-2.0
//
// The transport injection point, for this module's own tests.
//
// It is in `src/` rather than in `include/` deliberately. The seam is internal
// (ADR-0003), and a testing hook in an installed header would make it part of
// the module's surface -- which is how an internal interface becomes one that
// cannot be replaced without a deprecation.
//
// The shared boundary suite does not use this. Its misbehaving-transport row is
// a real hostile server (`tests/fixture-server`), because a fake transport
// asserting a short read would be the suite checking its own mock. What this
// hook is for is the protocol rules that no server can produce on demand: a
// scheme downgrade over a plaintext fixture, an unbounded redirect chain
// counted exactly, and a client that fails before it connects.

#ifndef USDASSETHTTP_TESTSUPPORT_H
#define USDASSETHTTP_TESTSUPPORT_H

#include <functional>
#include <memory>
#include <string>

#include "usdAssetHttp/HttpAssetReader.h"
#include "Transport.h"

namespace usdasset {
namespace http {
namespace testing {

using MakeTransport = std::function<std::unique_ptr<Transport>()>;

/// Opens `url` over a transport the caller supplies. A null factory means the
/// real one, which is what `Open` does.
HttpOpenResult OpenWithTransport(const std::string& url,
                                 const HttpOptions& options,
                                 MakeTransport factory);

}  // namespace testing
}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_TESTSUPPORT_H
