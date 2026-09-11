# Configuration

This document defines the configuration surface. The five transport bounds are
implemented as of `v0.2.0`, the four cache variables as of `v0.3.0`, the two
persistence variables as of `v0.4.0`, and the destination policy as of `v0.7.0`;
all twelve are read by `plugins/http-resolver`, once, when the resolver is
constructed. Eight of them can also be set per stage, through an
`ArResolverContext`, as of `v0.7.0` (§4).

## 1. Two mechanisms, in order

```text
environment variables       process-wide, v0.x bootstrap
ArResolverContext           per stage, v0.7.0
```

Environment variables are a bootstrap, not the destination. A host that opens
two stages against two servers with two credentials cannot be served by a
process-global, and the moment authentication is real, the context form is the
only correct one. Both coexist: environment values are the defaults a bound
context overrides.

## 2. Variables

All variables are prefixed `USD_HTTP_RESOLVER_`. Every one has a working
default; a deployment that must set several of these to function is a signal
that the defaults are wrong.

| Variable | Default | Meaning |
| --- | --- | --- |
| `USD_HTTP_RESOLVER_BLOCK_SIZE` | `65536` | Cache block size in bytes; rounded down to a power of two, and the rounding is reported |
| `USD_HTTP_RESOLVER_CACHE_BUDGET` | `134217728` | Process-wide cache budget in bytes, shared across assets |
| `USD_HTTP_RESOLVER_COALESCE_GAP` | `1` | Maximum gap, in blocks, merged into one request |
| `USD_HTTP_RESOLVER_MAX_REQUEST_BYTES` | `8388608` | Upper bound on a single merged request |
| `USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR` | unset | Directory for the on-disk block cache. Unset means no persistence; there is no default location |
| `USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET` | `1073741824` | Ceiling on the cache directory in bytes, enforced by a sweep |
| `USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS` | `10000` | Connection deadline |
| `USD_HTTP_RESOLVER_READ_TIMEOUT_MS` | `30000` | Deadline from connection established to status line received |
| `USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS` | `300000` | Total per-request deadline, headers and body |
| `USD_HTTP_RESOLVER_MAX_RETRIES` | `2` | Retry ceiling for retryable failures; `0` disables retry |
| `USD_HTTP_RESOLVER_MAX_REDIRECTS` | `5` | Redirect chain ceiling; `0` refuses to follow any |
| `USD_HTTP_RESOLVER_DESTINATIONS` | `public,private,loopback` | Address classes a connection may reach, as a comma-separated set of `public`, `private`, `loopback`, and `link-local`; see §2.1 |
| `USD_HTTP_RESOLVER_METRICS_DUMP` | unset | When set, dumps the metrics aggregate at process exit |

An unparseable value is a diagnostic at first use, not a silent fallback to the
default. A configuration typo that silently does nothing is worse than one that
fails.

The three deadlines are the ones the backend separates so that `Timeout`
(`HTTP006`) can name which one elapsed, which DIAGNOSTICS.md requires of it. The
value read is the one the resolver was constructed with, unless the stage's
context sets another (§4).

The four cache defaults are measured constants and the measurement that chose
them is [BLOCK_POLICY.md](BLOCK_POLICY.md). Two of them are labelled there as
bounds rather than tuned values, which is a distinction this table cannot carry
and that record can.

Three rules that follow from the code and are worth stating rather than
discovering:

- **`0` is rejected for the three deadlines and accepted for the two counters.**
  To most transports a zero deadline means *no* deadline, which is the one value
  §10 of the [design policy](../design/DESIGN_POLICY.md) exists to forbid. For
  the counters it means "do not retry" and "do not follow", which are both
  legitimate things to ask for.
- **One bad value does not discard the others.** Each variable is parsed
  independently, so a configuration that is mostly right stays mostly in force,
  and the warning names the variable, its value, and what was wrong with it.
- **An adjustment is reported, not only a rejection.** A block size that is not
  a power of two is rounded *down* — rounding up would silently double the bytes
  every miss moves — and a coalescing gap too wide to fit under
  `USD_HTTP_RESOLVER_MAX_REQUEST_BYTES` is capped. Both are diagnostics. An
  operator who set 100000 and got 65536 should learn it from a log rather than
  from a byte count. Values outside the block-size or budget bounds are refused
  rather than clamped: a budget below one block means the caller wanted no
  cache, and there is no variable for that.

### 2.1 The destination policy

§10.2 of the [design policy](../design/DESIGN_POLICY.md): an identifier can
arrive from a layer the user did not author, so the reach it has is bounded by
declared policy rather than by whatever the host's network happens to allow.
`USD_HTTP_RESOLVER_DESTINATIONS` is the declaration.

| Class | Addresses |
| --- | --- |
| `loopback` | `127.0.0.0/8` and `::1`; and `0.0.0.0/8` and `::`, because a connect to them reaches this host |
| `link-local` | `169.254.0.0/16` and `fe80::/10`, which is where cloud instance-metadata services live |
| `private` | `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, the shared address space `100.64.0.0/10`, `fc00::/7`, and the deprecated `fec0::/10` |
| `public` | everything else |

An IPv6 address that carries an IPv4 one — mapped, compatible, or behind the
NAT64 well-known prefix — is the class of the IPv4 address, because that is
where the connection ends up. `[::ffff:169.254.169.254]` is link-local.

**The default is `public,private,loopback`, and it is a middle rather than
either end.** Loopback and private networks stay reachable because `http` is
registered for local fixture servers and intranet hosts
([RESOLVER.md](../architecture/RESOLVER.md) §1), and a default that broke the
uses the scheme exists for would be a default every deployment overrode.
Link-local is refused because nothing legitimate serves USD from it and the one
thing reliably found there is the credential endpoint of a cloud instance. A
deployment that wants less reach says so: `public` alone for a render farm that
must never reach its own network from a layer it did not author, `private` alone
for one that must never leave it — for the whole process in the environment, or
for one stage in its context (§4).

The policy is judged twice, and neither judgement is redundant:

- **At connect time**, against the numeric address the transport is about to
  connect to, after the name was resolved and before a socket exists. This is
  the check that makes the policy hold at all: for a name that resolves to a
  refused address, for a name whose answer changed between lookups, and for
  every legacy spelling of an address a system resolver accepts. A name with
  several addresses is refused only when every one of them is; a refused IPv6
  address followed by a permitted IPv4 one that did not answer is a network
  failure, not a refusal.
- **Before any request**, against a literal address in the URL, at every
  redirect hop. This is the check that holds through a proxy, where the address
  this process connects to is the proxy's and the destination is the proxy's to
  resolve. It reads canonical spellings only; a non-canonical one is left to the
  first check, which sees the address it actually becomes.

A refusal is `AccessDenied` (`HTTP002`), naming the class, and no request is
sent. The code is the one a `403` gets because a caller does the same thing
about both — nothing, and tell whoever owns the configuration — and the class is
named so that the person told goes to their own policy rather than to the
origin's permissions.

A value is a set, not a level, because the classes are not an order. Whitespace
around a name is tolerated; an unknown name refuses the whole value and keeps
the default, because a policy that silently dropped the word it did not
recognize is a different policy from the one written.

One interaction is named rather than solved. Through a proxy, a *name* is
resolved by the proxy, so the policy cannot see where it leads, and the
connect-time check judges the proxy's own address instead: a proxy on loopback
needs `loopback` in the set. A literal in the URL is still judged, which is why
a redirect to `http://169.254.169.254/` is refused through a proxy as well as
without one.

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
- **The cache bypass threshold.** A read at least this large skips the cache
  entirely, and it is a constant rather than a variable in `v0.3.0`. It exists
  to stop a streaming pass evicting the whole working set, which is a
  correctness-of-policy rule and not a tuning knob; a deployment that could set
  it to zero could turn the full sequential read into the regression
  [BASELINE.md](BASELINE.md) gates against.
- **The location of the persistent cache.** There is no default directory, and
  the absence is the policy: a resolver that wrote to a disk nobody named would
  be a surprise, and choosing a home for it — `%LOCALAPPDATA%`, `$XDG_CACHE_HOME`,
  `/var/tmp` — is a deployment's decision about a disk this project cannot see.
  Naming one turns persistence on; that is the only switch, because a separate
  enable flag would be a second way to say the same thing and a second thing to
  get wrong.
- **Which validators may persist.** Never a variable. `Strong` only, per
  [CACHE.md](../architecture/CACHE.md) §8. A switch that admitted a weak
  validator to disk is a switch for writing a guess down and reading it back
  after a restart, which is precisely what the rule exists to prevent.
- **Credentials.** Never read from a variable in this list. When authentication
  arrives it arrives through a credential provider resolved from the
  environment or the context, and no credential is ever named in a variable
  this resolver defines, printed, or persisted.

## 4. Precedence, and the context form

```text
ArResolverContext  >  environment variable  >  built-in default
```

The environment is read once, when the resolver is constructed, and kept. A
context is resolved against that snapshot rather than against `getenv`, so a
host that mutates its environment mid-session does not change what a stage it
opened earlier is configured by — and a reader keeps the options it was opened
with, so a stage's configuration is bound when its assets are opened, not per
request.

A context is created from a string, through OpenUSD, with either scheme name —
one resolver type serves both:

```python
ctx = Ar.GetResolver().CreateContextFromString(
    "https",
    "USD_HTTP_RESOLVER_DESTINATIONS=public; USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS=60000")
stage = Usd.Stage.Open("https://example.org/scenes/main.usda", ctx)
```

The string is `NAME=value` entries separated by `;`. The names are the
environment's, spelled exactly as §2 spells them, and each value goes through
the parser the environment's would: refused or adjusted for the same reasons,
and reported the same way. Whitespace around an entry, a name, or a value is
tolerated, and so is an empty entry — a trailing `;` is what concatenation
leaves behind. A name set twice keeps the last value, as an environment
assignment would, and says so.

Eight variables may be set in a context, and four may not:

| May be set per stage | Environment only |
| --- | --- |
| `CONNECT_TIMEOUT_MS`, `READ_TIMEOUT_MS`, `TOTAL_TIMEOUT_MS` | `BLOCK_SIZE` |
| `MAX_RETRIES`, `MAX_REDIRECTS` | `CACHE_BUDGET` |
| `DESTINATIONS` | `PERSISTENT_CACHE_DIR` |
| `COALESCE_GAP`, `MAX_REQUEST_BYTES` | `PERSISTENT_CACHE_BUDGET` |

The left column is what binds a reader or its cache wrap, and is therefore a
property of whoever opened the asset. The right column is what the process
shares: one block store with one budget (CACHE.md §7), one persistent
directory, and a store whose stripes are sized for one block size — eight blocks
to a stripe, so a stage that asked for blocks larger than a stripe would fetch
each one and watch it evicted on arrival. A context that names one of those is
told, when it is created, that the value is read from the environment.

Problems are reported once, when the context is created, and never at bind
time: a context is created once and bound on every thread that composes the
stage, and a warning per bind would be one typo rendered once per prim. What a
context carries is what it admitted, so two contexts that say the same thing
compare and hash equal however they were spelled, and a context whose every
entry was refused configures a stage exactly as no context does.

In Python, a context reads back as its canonical string —
`Ar.ResolverContext('USD_HTTP_RESOLVER_DESTINATIONS=public')` — which is what
`Usd.Stage.__repr__` prints for a stage opened with one.

Two properties follow for the destination policy in particular, and both are
asserted in `httpResolver_stage`. A stage whose context refuses a destination
cannot reach it through a layer another stage has already loaded: every
identifier this resolver owns is context-dependent, which is what makes
OpenUSD's layer registry look the layer up by the path `Resolve` returned
rather than by its name ([RESOLVER.md](../architecture/RESOLVER.md) §6). And it
cannot reach it through a reader another stage's resolve left behind: a
retained open is handed only to a caller that would have opened it under the
same transport options.
