# Configuration

This document defines the configuration surface. The five transport bounds are
implemented as of `v0.2.0` and are read by `plugins/http-resolver`; the cache
variables are planned alongside the cache they control, and the
`ArResolverContext` form arrives in `v0.6.0`.

## 1. Two mechanisms, in order

```text
environment variables       process-wide, v0.x bootstrap
ArResolverContext           per stage, v0.6.0
```

Environment variables are a bootstrap, not the destination. A host that opens
two stages against two servers with two credentials cannot be served by a
process-global, and the moment authentication is real, the context form is the
only correct one. Both will coexist: environment values become the defaults
that a bound context overrides.

## 2. Variables

All variables are prefixed `USD_HTTP_RESOLVER_`. Every one has a working
default; a deployment that must set several of these to function is a signal
that the defaults are wrong.

| Variable | Default | Meaning |
| --- | --- | --- |
| `USD_HTTP_RESOLVER_BLOCK_SIZE` | measured in `v0.3.0` | Cache block size in bytes; rounded to a power of two |
| `USD_HTTP_RESOLVER_CACHE_BUDGET` | measured in `v0.3.0` | Process-wide cache budget in bytes |
| `USD_HTTP_RESOLVER_COALESCE_GAP` | measured in `v0.3.0` | Maximum gap, in blocks, merged into one request |
| `USD_HTTP_RESOLVER_MAX_REQUEST_BYTES` | measured in `v0.3.0` | Upper bound on a single merged request |
| `USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS` | `10000` | Connection deadline |
| `USD_HTTP_RESOLVER_READ_TIMEOUT_MS` | `30000` | Deadline from connection established to status line received |
| `USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS` | `300000` | Total per-request deadline, headers and body |
| `USD_HTTP_RESOLVER_MAX_RETRIES` | `2` | Retry ceiling for retryable failures; `0` disables retry |
| `USD_HTTP_RESOLVER_MAX_REDIRECTS` | `5` | Redirect chain ceiling; `0` refuses to follow any |
| `USD_HTTP_RESOLVER_METRICS_DUMP` | unset | When set, dumps the metrics aggregate at process exit |

An unparseable value is a diagnostic at first use, not a silent fallback to the
default. A configuration typo that silently does nothing is worse than one that
fails.

The three deadlines are the ones the backend separates so that `Timeout`
(`HTTP006`) can name which one elapsed, which DIAGNOSTICS.md requires of it. The
value read is the one the resolver was constructed with: these are process-wide,
and per-stage values are what `ArResolverContext` is for in `v0.6.0`.

Two rules that follow from the code and are worth stating rather than
discovering:

- **`0` is rejected for the three deadlines and accepted for the two counters.**
  To most transports a zero deadline means *no* deadline, which is the one value
  §10 of the [design policy](../design/DESIGN_POLICY.md) exists to forbid. For
  the counters it means "do not retry" and "do not follow", which are both
  legitimate things to ask for.
- **One bad value does not discard the other four.** Each variable is parsed
  independently, so a configuration that is mostly right stays mostly in force,
  and the warning names the variable, its value, and what was wrong with it.

## 3. What is not configurable

Some things are deliberately absent, because making them configurable would
turn a correctness property into a deployment mistake:

- **Range-unsupported behavior.** Not a variable in `v0.2.0`. The policy is a
  hard error, decided in [ADR-0002](../adr/0002-range-unsupported-policy.md);
  `USD_HTTP_RESOLVER_RANGE_POLICY` is deliberately absent rather than present
  with one legal value, and arrives with the bounded fallback it would select.
- **Validator checking.** Never disabled. A "skip `If-Range`" switch is a
  switch for serving corrupt data, and it is not made safer by being off by
  default.
- **TLS verification.** Never disabled. A test server uses plain `http`, which
  is why `http` is a registered scheme at all.
- **`https` to `http` redirect following.** Always refused.
- **Response framing validation.** Always on. A `206` that does not cover the
  requested range is always `InvalidResponse`.
- **Credentials.** Never read from a variable in this list. When authentication
  arrives it arrives through a credential provider resolved from the
  environment or the context, and no credential is ever named in a variable
  this resolver defines, printed, or persisted.

## 4. Precedence

```text
ArResolverContext  >  environment variable  >  built-in default
```

Resolved at bind time, not per request. A per-request read of a global is both
slow and unpredictable when a host mutates the environment mid-session.
