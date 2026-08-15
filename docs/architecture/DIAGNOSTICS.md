# Diagnostics contract

This document defines the typed error vocabulary shared by every module, and
its projection onto the stable `HTTPxxx` codes the plugin bundle emits.

Status: planned for `v0.1.0`. Nothing here is implemented.

## 1. Principle

Transport detail is neither leaked nor erased.

Erasing it produces "failed to read asset", which tells a consumer nothing and
sends a user to the wrong system. Leaking it produces "curl error 28", which
couples every consumer to this repository's HTTP client. The vocabulary below
is the middle: it names conditions a caller can act on, in terms that survive a
change of transport.

The test of a code is whether a caller would do something different. `Timeout`
and `NetworkError` are separate because one is worth retrying at a higher level
and the other is not. `403` and `404` are separate because one is a
configuration problem and the other is a scene problem. Two HTTP status codes
that lead to identical caller behavior share one code here.

## 2. Vocabulary

```cpp
enum class StatusCode {
    Ok,

    NotFound,             // the asset does not exist
    AccessDenied,         // it exists, or may, but this caller may not read it
    RangeNotSupported,    // the server will not serve partial content
    InvalidResponse,      // malformed framing, wrong range, truncated below EOF
    NetworkError,         // connection failure, DNS, TLS, reset
    Timeout,              // a deadline elapsed
    AssetChanged,         // the validator changed while the asset was open
    Cancelled,            // the caller cancelled
    InvalidArgument,      // an overflowing or malformed request from the caller
    Unsupported,          // a legal operation this backend does not implement
};

struct Status {
    StatusCode                   code = StatusCode::Ok;
    Severity                     severity = Severity::Error;
    std::string                  message;        // for humans
    std::optional<std::uint64_t> byteOffset;     // where, when known
    std::optional<std::size_t>   byteLength;
    std::optional<int>           transportStatus;// e.g. HTTP 503, when known
};
```

`transportStatus` is diagnostic sugar for a human reading a log. No caller
branches on it, and no consumer sees it.

## 3. Requirements

- **Codes are stable.** A code is a compatibility commitment, like a schema
  field. Adding one is a minor change; changing what one means is a breaking
  change.
- **Messages are for humans.** Callers branch on `code`, never on message text.
- **No secrets in messages.** A message never contains an `Authorization`
  value, a token, a cookie, or a signed-URL query string. A URL that appears in
  a message has its query string elided.
- **Range and offset are attached where available.** "Read failed at offset
  1258291, length 65536" is actionable; "read failed" is not.
- **Retries are visible.** A retried request appears in
  [metrics](METRICS.md), not only in a debug log. A silent retry that succeeds
  still cost latency, and an operator investigating slowness must be able to
  see it.
- **Warnings are distinguished from failures.** A backend that degrades — for
  example, one that had to re-request a block — warns; it does not fail.

## 4. Distinctions that matter

### 4.1 Truncation at EOF is not an error

A read that returns fewer bytes because the asset ended is `Ok`. A read that
returns fewer bytes below EOF is `InvalidResponse`. Conflating them converts a
network fault into apparent file corruption, and the format plugin above
reports a malformed asset that is in fact intact. This is the single most
important distinction in this document.

### 4.2 `RangeNotSupported` is not `InvalidResponse`

A server that ignores `Range` and returns `200` with a full body is not
malformed — it is a server that does not do what this project requires. It gets
its own code so that a caller can present a comprehensible message and so that
the fallback policy has something to key on. See
[ADR-0002](../adr/0002-range-unsupported-policy.md), which is open.

### 4.3 `AssetChanged` is not a read error

The asset was fine and the read was fine; the world moved. It is reported
distinctly so that a consumer can invalidate its own generated cache rather
than concluding that the data is corrupt. It is never repaired silently by
re-opening: silently rebinding to new content mid-composition is how two
revisions end up in one stage.

### 4.4 `NotFound` is not `NetworkError`

An unreachable server is not an absent asset. `Resolve` returns an empty path
only for genuine absence; a transport failure is a diagnostic.

## 5. Plugin projection

The bundle projects the vocabulary onto stable `HTTPxxx` codes and OpenUSD
diagnostics. The mapping is one-way and total:

| Code | `HTTPxxx` | OpenUSD | Typical cause |
| --- | --- | --- | --- |
| `NotFound` | `HTTP001` | error | `404`, or a resolved path that does not exist |
| `AccessDenied` | `HTTP002` | error | `401`, `403` |
| `RangeNotSupported` | `HTTP003` | error | no `Accept-Ranges`, or `200` in response to `Range` |
| `InvalidResponse` | `HTTP004` | error | wrong `Content-Range`, truncated body, bad framing |
| `NetworkError` | `HTTP005` | error | connection reset, DNS, TLS failure |
| `Timeout` | `HTTP006` | error | connect, read, or total deadline elapsed |
| `AssetChanged` | `HTTP007` | error | validator changed mid-read |
| `Cancelled` | `HTTP008` | warning | caller cancellation; not a fault |
| `InvalidArgument` | `HTTP009` | coding error | overflowing offset and size |
| `Unsupported` | `HTTP010` | error | write, or an unimplemented backend operation |
| retry occurred | `HTTP101` | warning | a request succeeded after N retries |
| range unsupported, full-download fallback | `HTTP102` | warning | reserved for ADR-0002 |

Codes are allocated in this table. A new code is added here before it is
emitted.

## 6. Message form

```text
HTTP004: partial response did not cover the requested range
         (requested bytes 1258291-1323826, server returned 1258291-1290000)
         https://example.org/data/survey.copc
```

Three properties: the code first, the actual and expected framing second, and
the identifier with its query string elided last.
