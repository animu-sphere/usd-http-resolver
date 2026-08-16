# ADR-0002: Behavior when a server does not support range requests

## Status

Accepted, 2026-08-16, for `v0.2.0`: **option A, hard error**. A server that
will not serve partial content produces `RangeNotSupported` (`HTTP003`) and no
bytes. There is no whole-asset fallback in `v0.2.0`.

Bounded fallback is not rejected — it is deferred to a later release as a
separate feature with its own residency model. Reopening it means a new ADR,
not an edit to this one.

## Context

Range support is a server capability, not a guarantee. A server may:

- omit `Accept-Ranges`, or send `Accept-Ranges: none`;
- advertise ranges and then answer a `Range` request with `200` and a full
  body;
- honor ranges for some paths and not others, for instance behind a
  transforming proxy or a compressing CDN edge;
- honor ranges but ignore `If-Range`.

The second case is the dangerous one. The request looks successful. The
response body is correct. It is simply the entire asset, and the reader has to
decide what to do with a 10 GB body it did not ask for while a caller waits for
64 KB.

The project's whole premise is that reading a fraction of a remote asset is
cheap. A server without range support removes the premise. What remains is a
policy question about what the user experiences: a clear failure, a slow
success, or something in between.

## Options

### A. Hard error

Report `RangeNotSupported` and refuse.

- Never surprises anyone with a multi-gigabyte transfer.
- The diagnostic is actionable and names the real problem: the server.
- Fails on assets that would have worked. A 2 MB `.usda` layer on a server
  without range support is entirely readable, and refusing it is obstinate.

### B. Full-download fallback, always

Fetch the whole asset once, serve reads from it.

- Everything works, everywhere. Maximum compatibility.
- Silently turns a bounded query into an unbounded transfer. On a large asset
  this looks like a hang, then like memory exhaustion. This is the specific
  failure §4.2 of the [design policy](../design/DESIGN_POLICY.md) names.
- Needs a spill-to-disk story, which drags in cache-directory ownership,
  atomicity, and cleanup for a case the project does not want to encourage.

### C. Size-bounded fallback

Fall back to a full download when `Content-Length` is below a configured
threshold; otherwise report `RangeNotSupported`.

- Small layers and small textures work. Large assets fail loudly instead of
  hanging.
- The threshold is arbitrary and will be wrong for somebody. It also becomes a
  configuration surface that must be per stage, not per process.
- `Content-Length` may be absent under chunked encoding, so the policy needs a
  defined answer for "size unknown" — which should be to refuse.

### D. Policy selected by configuration, with C as the default

The caller chooses `error`, `fallback`, or `bounded`, and `bounded` is the
default.

- Honest: the correct answer genuinely depends on the deployment.
- Three code paths to test, and a default that still needs choosing.

## Considerations

- **Detection cost.** Determining support reliably means testing an actual
  range request, since `Accept-Ranges` is advisory. That can be folded into the
  first real read rather than paid as a separate probe at open.
- **Cache interaction.** A downloaded whole asset is not a block cache entry;
  it is a different residency model with a different budget. Option B or C adds
  a second storage path to the [cache contract](../architecture/CACHE.md).
- **Metrics honesty.** A fallback download must be visible in
  `bytesTransferred` and in `selectivity`, and it must warn. A silent fallback
  would make the amplification numbers meaningless — the project's headline
  claim would be reported against a path that did not do what it says.
- **Per-asset, not per-server.** Range support varies by path behind a CDN, so
  the decision belongs to the asset, not to a host-level capability cache.
- **Consumer expectations.** A consumer that asked for 64 KB and blocked on
  10 GB will report this repository as broken, correctly. Whatever is chosen,
  the wait must be attributable.

## Decision

**A, for `v0.2.0`.**

```text
Range unsupported
    -> RangeNotSupported (HTTP003)
    -> no whole-asset fallback
```

The choice is about what `v0.2.0` is allowed to be, more than about which
policy is ultimately right. C remains the better long-term answer for small
assets, and D remains the likely end state once configuration lands in
`v0.6.0`. Neither belongs in the release that first has to prove HTTP range
reads are byte-equivalent to a local file.

Implementing bounded fallback now would pull all of this into that release:

```text
whole-asset residency          a second storage model beside the block cache
memory and disk spill          allocation bounds, spill paths, cleanup
threshold selection            a number that is wrong for somebody
warning policy                 when HTTP102 fires, and how loudly
configuration                  per process or per stage, decided early
separate metrics               fallback bytes counted apart from range bytes
```

Every line of that is a correctness surface of its own, verified by tests that
have nothing to do with range reads. `v0.2.0`'s scope is one sentence — *HTTP
random access is byte-equivalent to the local backend* — and the fallback
neither supports that sentence nor is testable against the oracle that proves
it.

The cost is real and accepted: a 2 MB `.usda` on a server without range support
is readable and will be refused. The diagnostic names the server as the cause,
which is actionable, and the failure is loud rather than a multi-gigabyte
transfer nobody asked for. That is the correct direction to err in the release
that establishes trust.

### Recorded consequences

- **Default policy:** error. There is no other policy in `v0.2.0`.
- **`USD_HTTP_RESOLVER_RANGE_POLICY`** is not introduced. A configuration
  variable with one legal value is a promise about a feature that does not
  exist; it arrives with the fallback.
- **Detection:** from `Accept-Ranges` and from the actual response status. A
  `200` in answer to a `Range` request is `RangeNotSupported`, not
  `InvalidResponse` — the server is coherent, it just does not do what this
  project requires.
- **Per asset, not per server.** Range support varies by path behind a CDN, so
  no host-level capability cache decides it.
- **Unknown `Content-Length`** (chunked, no length) is refused, for the same
  reason: an unbounded body is what the policy exists to prevent.
- **`HTTP102`** stays allocated in
  [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md) and unemitted, reserved for
  the fallback when it lands.

## When bounded fallback returns

The trigger is a demonstrated need — a real deployment where small assets sit
behind a server that will not serve ranges — not a hypothetical one. When it
does return, it is admitted as its own feature and records:

- whether the body is held in memory, spilled to disk, or bounded by the cache
  budget — but never as a block cache entry, because it is a different
  residency model with a different budget;
- the threshold, and what happens when `Content-Length` is unknown (refuse);
- the diagnostic and the metric emitted when the fallback engages, with the
  fallback bytes counted in full so `selectivity` stays honest;
- whether the policy is per process, per stage, or per asset.
