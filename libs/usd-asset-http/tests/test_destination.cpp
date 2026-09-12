// SPDX-License-Identifier: Apache-2.0
//
// Address classification for the destination policy, as a table.
//
// The policy is only as good as the arithmetic under it, and the arithmetic is
// where request-forgery bypasses live: an address that means loopback spelled
// in a form the classifier did not expect. So every row below is either a
// boundary of one of the five classes or a spelling of an address that a
// careless classifier would put in the wrong one -- and the whole file needs no
// socket, because the transport hands the classifier bytes and the protocol
// layer hands it text.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Check.h"
#include "Destination.h"

namespace {

using usdasset::http::AddressClass;
using usdasset::http::AddressClassName;
using usdasset::http::ClassifyHostLiteral;
using usdasset::http::ClassifyIPv4;
using usdasset::http::ClassifyIPv6;
using usdasset::http::DestinationPolicy;
using usdasset::http::ParseIPv4;
using usdasset::http::ParseIPv6;

void Fail(const char* what, const std::string& input, const char* detail) {
    std::fprintf(stderr, "FAIL [%s] %s: %s\n", what, input.c_str(), detail);
    ++usdassettest::FailureCount();
}

struct LiteralCase {
    const char* host;
    AddressClass expected;
};

void TestHostLiterals() {
    // The class a URI host names when it is a literal. Every row is one the
    // pre-flight check has to get right, because through a proxy it is the
    // only check that sees the destination.
    const LiteralCase cases[] = {
        // Loopback, and the addresses a connect treats as this host.
        {"127.0.0.1", AddressClass::Loopback},
        {"127.255.255.254", AddressClass::Loopback},
        {"0.0.0.0", AddressClass::Loopback},
        {"0.1.2.3", AddressClass::Loopback},
        {"[::1]", AddressClass::Loopback},
        {"[::]", AddressClass::Loopback},
        {"[0:0:0:0:0:0:0:1]", AddressClass::Loopback},

        // The instance-metadata endpoints, by value, whichever range they sit
        // in -- two of them are inside ranges permitted by default.
        {"169.254.169.254", AddressClass::Metadata},
        {"169.254.170.2", AddressClass::Metadata},
        {"169.254.170.23", AddressClass::Metadata},
        {"169.254.0.23", AddressClass::Metadata},
        {"100.100.100.200", AddressClass::Metadata},
        {"168.63.129.16", AddressClass::Metadata},
        {"[fd00:ec2::254]", AddressClass::Metadata},
        {"[fd00:ec2::23]", AddressClass::Metadata},
        {"[FD00:0EC2:0:0:0:0:0:254]", AddressClass::Metadata},
        // And their neighbours, which are only what their ranges say.
        {"169.254.169.253", AddressClass::LinkLocal},
        {"100.100.100.201", AddressClass::Private},
        {"168.63.129.17", AddressClass::Public},
        {"[fd00:ec2::255]", AddressClass::Private},
        {"[fd00:ec3::254]", AddressClass::Private},

        // Link-local, the rest of it.
        {"169.254.0.0", AddressClass::LinkLocal},
        {"169.254.255.255", AddressClass::LinkLocal},
        {"[fe80::1]", AddressClass::LinkLocal},
        {"[febf:ffff::1]", AddressClass::LinkLocal},
        {"[fe80::1%25en0]", AddressClass::LinkLocal},
        {"[FE80::A9FE:A9FE]", AddressClass::LinkLocal},

        // Private, at both edges of each range.
        {"10.0.0.0", AddressClass::Private},
        {"10.255.255.255", AddressClass::Private},
        {"172.16.0.0", AddressClass::Private},
        {"172.31.255.255", AddressClass::Private},
        {"192.168.0.1", AddressClass::Private},
        {"100.64.0.0", AddressClass::Private},
        {"100.127.255.255", AddressClass::Private},
        {"[fc00::1]", AddressClass::Private},
        {"[fdff:ffff::1]", AddressClass::Private},
        {"[fec0::1]", AddressClass::Private},

        // Public, just outside each range -- the rows that catch a mask that
        // is one bit too wide.
        {"172.15.255.255", AddressClass::Public},
        {"172.32.0.0", AddressClass::Public},
        {"169.253.255.255", AddressClass::Public},
        {"169.255.0.0", AddressClass::Public},
        {"100.63.255.255", AddressClass::Public},
        {"100.128.0.0", AddressClass::Public},
        {"126.255.255.255", AddressClass::Public},
        {"128.0.0.0", AddressClass::Public},
        {"8.8.8.8", AddressClass::Public},
        {"[2001:4860:4860::8888]", AddressClass::Public},
        {"[fbff::1]", AddressClass::Public},
        {"[fe00::1]", AddressClass::Public},

        // An IPv6 address carrying an IPv4 one is the IPv4 one's class. These
        // are the classic bypasses: each spells a refused address in a family
        // a naive classifier files as public.
        {"[::ffff:127.0.0.1]", AddressClass::Loopback},
        {"[::ffff:7f00:1]", AddressClass::Loopback},
        {"[::ffff:169.254.169.254]", AddressClass::Metadata},
        {"[::ffff:a9fe:a9fe]", AddressClass::Metadata},
        {"[::ffff:169.254.1.1]", AddressClass::LinkLocal},
        {"[::ffff:10.1.2.3]", AddressClass::Private},
        {"[::ffff:8.8.8.8]", AddressClass::Public},
        {"[::127.0.0.1]", AddressClass::Loopback},
        {"[::169.254.169.254]", AddressClass::Metadata},
        {"[64:ff9b::169.254.169.254]", AddressClass::Metadata},
        {"[64:ff9b::100.100.100.200]", AddressClass::Metadata},
        {"[64:ff9b::7f00:1]", AddressClass::Loopback},
        {"[64:ff9b::8.8.8.8]", AddressClass::Public},

        // The one trailing dot of a fully qualified name is the same address.
        {"169.254.169.254.", AddressClass::Metadata},
        {"127.0.0.1.", AddressClass::Loopback},
    };
    for (const LiteralCase& row : cases) {
        AddressClass actual = AddressClass::Public;
        if (!ClassifyHostLiteral(row.host, &actual)) {
            Fail("literal", row.host, "not recognized as a literal address");
            continue;
        }
        if (actual != row.expected) {
            std::fprintf(stderr, "FAIL [literal] %s: %s, expected %s\n", row.host,
                         AddressClassName(actual), AddressClassName(row.expected));
            ++usdassettest::FailureCount();
        }
    }
}

void TestNamesAreNotLiterals() {
    // A name is judged at connect time, by what it resolves to. A spelling of
    // an address that is not canonical is not read here either: the transport
    // judges it as its client normalizes it, and reading `010.0.0.1` as
    // 10.0.0.1 here would judge an address a client that honours the leading
    // zero never connects to.
    const char* const names[] = {
        "example.org",
        "localhost",
        "127.1",
        "0x7f.0.0.1",
        "017700000001",
        "2130706433",
        "010.0.0.1",
        "127.0.0.01",
        "256.0.0.1",
        "1.2.3",
        "1.2.3.4.5",
        "1.2.3.4..",
        ".1.2.3.4",
        "1..2.3",
        "[]",
        "[::1",
        "::1",
        "[1.2.3.4]",
        "[v1.fe80::1]",
        "",
    };
    for (const char* name : names) {
        AddressClass ignored = AddressClass::Public;
        if (ClassifyHostLiteral(name, &ignored)) {
            Fail("name", name, "classified as a literal address");
        }
    }
}

void TestIPv6Grammar() {
    // RFC 4291 §2.2, and the malformations adjacent to it. A parser that
    // accepted any of the second list would be classifying a string that no
    // resolver turns into the address it was classified as.
    const char* const valid[] = {
        "::",
        "::1",
        "1::",
        "1:2:3:4:5:6:7:8",
        "1:2:3:4:5:6:7::",
        "::2:3:4:5:6:7:8",
        "1:2:3:4:5:6:1.2.3.4",
        "::ffff:1.2.3.4",
        "fe80::abcd:1234",
        "ABCD:ef01::",
    };
    for (const char* text : valid) {
        std::array<std::uint8_t, 16> out{};
        if (!ParseIPv6(text, &out)) Fail("ipv6", text, "refused a valid address");
    }

    const char* const invalid[] = {
        "",
        ":",
        ":::",
        "1:::2",
        "1::2::3",
        ":1::2",
        "1::2:",
        "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7:8::",
        "::1:2:3:4:5:6:7:8",
        "12345::",
        "g::",
        "1.2.3.4",
        "1.2.3.4::",
        "::1.2.3.4:5",
        "1:2:3:4:5:6:7:1.2.3.4",
        "::256.0.0.1",
    };
    for (const char* text : invalid) {
        std::array<std::uint8_t, 16> out{};
        if (ParseIPv6(text, &out)) Fail("ipv6", text, "accepted a malformed address");
    }

    // And the bytes, for the one form with arithmetic in it.
    std::array<std::uint8_t, 16> mapped{};
    CHECK(ParseIPv6("::ffff:169.254.1.2", &mapped));
    const std::array<std::uint8_t, 16> expected = {0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0xff, 0xff, 169, 254, 1, 2};
    CHECK(mapped == expected);
}

void TestIPv4Bytes() {
    std::array<std::uint8_t, 4> out{};
    CHECK(ParseIPv4("192.0.2.255", &out));
    CHECK(out[0] == 192 && out[1] == 0 && out[2] == 2 && out[3] == 255);
    CHECK(ClassifyIPv4({169, 254, 169, 254}) == AddressClass::Metadata);
    CHECK(ClassifyIPv4({169, 254, 1, 1}) == AddressClass::LinkLocal);

    std::array<std::uint8_t, 16> loopback{};
    loopback[15] = 1;
    CHECK(ClassifyIPv6(loopback) == AddressClass::Loopback);
}

void TestDefaultPolicy() {
    // The documented default, asserted so that changing it is a change to a
    // test and not only to a comment: CONFIGURATION.md states it, and a
    // default nobody can see move is a default nobody chose.
    const DestinationPolicy policy;
    CHECK(policy.Permits(AddressClass::Public));
    CHECK(policy.Permits(AddressClass::Private));
    CHECK(policy.Permits(AddressClass::Loopback));
    CHECK(!policy.Permits(AddressClass::LinkLocal));
    CHECK(!policy.Permits(AddressClass::Metadata));

    // Permitting link-local is not permitting the metadata endpoints in it.
    DestinationPolicy linkLocal;
    linkLocal.linkLocal = true;
    CHECK(!linkLocal.Permits(AddressClass::Metadata));

    // Covers: every destination reachable under the narrower policy is
    // reachable under the wider one, and not the other way round.
    CHECK(linkLocal.Covers(policy));
    CHECK(!policy.Covers(linkLocal));
    CHECK(policy.Covers(policy));

    DestinationPolicy publicOnly;
    publicOnly.privateNetworks = false;
    publicOnly.loopback = false;
    CHECK(publicOnly.Permits(AddressClass::Public));
    CHECK(!publicOnly.Permits(AddressClass::Private));
    CHECK(!publicOnly.Permits(AddressClass::Loopback));
    CHECK(publicOnly != policy);

    // The spellings the configuration takes, pinned: they are a compatibility
    // surface as soon as one deployment has written one down.
    CHECK_EQ(std::string(AddressClassName(AddressClass::Public)), std::string("public"));
    CHECK_EQ(std::string(AddressClassName(AddressClass::Private)), std::string("private"));
    CHECK_EQ(std::string(AddressClassName(AddressClass::Loopback)),
             std::string("loopback"));
    CHECK_EQ(std::string(AddressClassName(AddressClass::LinkLocal)),
             std::string("link-local"));
    CHECK_EQ(std::string(AddressClassName(AddressClass::Metadata)),
             std::string("metadata"));
}

}  // namespace

int main() {
    TestHostLiterals();
    TestNamesAreNotLiterals();
    TestIPv6Grammar();
    TestIPv4Bytes();
    TestDefaultPolicy();
    return usdassettest::Report("usdAssetHttp/destination");
}
