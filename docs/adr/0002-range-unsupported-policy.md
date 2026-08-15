# ADR-0002: Behavior when a server does not support range requests

## Status

Open. Must be resolved before the `v0.2.0` HTTP backend ships.

Until it is resolved, the implemented behavior is the conservative one: report
`RangeNotSupported` (`HTTP003`) and read nothing.

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

Not yet made.

The leading candidate is **C**, with the threshold defaulting conservatively
(order of tens of megabytes), an explicit `HTTP102` warning whenever the
fallback engages, the fallback bytes counted in full, and a refusal when
`Content-Length` is unknown. **D** is the likely end state once configuration
lands in `v0.6.0`; adding it before then would mean designing the configuration
surface around this one question.

Resolve this before `v0.2.0` ships, not after a user finds it.

## Consequences to record on resolution

- Which policy is the default, and what the threshold is.
- Whether the fallback body is held in memory, spilled to disk, or bounded by
  the cache budget.
- The diagnostic and metric emitted when the fallback engages.
- What happens when `Content-Length` is absent.
- Whether the policy is per process, per stage, or per asset.
