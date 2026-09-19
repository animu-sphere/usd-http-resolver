# Security Policy

## Supported versions

Security fixes are made against the `main` branch and the latest supported
release line. Older release lines may not receive fixes; upgrade before
reporting a problem that is already fixed on `main`.

| Version | Security support |
| --- | --- |
| `main` | Supported |
| Latest release | Supported |
| Older releases | Not supported |

## Reporting a vulnerability

Please do not open a public issue, pull request, or discussion for a suspected
vulnerability. Report it privately through
[GitHub Private Vulnerability Reporting](https://github.com/animu-sphere/usd-http-resolver/security/advisories/new).
If that channel is unavailable, contact a repository maintainer privately
through GitHub and request a secure reporting route.

Reports are most useful when they include:

- the affected version, commit, component, and build configuration;
- the security impact and an assessment of exploitability;
- exact reproduction steps or a minimal proof of concept;
- the expected and observed behavior;
- relevant platform, compiler, and dependency versions; and
- whether the report is known to be exploited or publicly disclosed.

Remove credentials, access tokens, private asset URLs, customer data, and other
secrets from the report. If a proof of concept needs a private fixture, explain
the required shape instead of attaching sensitive data.

## What to expect

Maintainers will acknowledge a report when they can, reproduce and assess the
impact, and coordinate a fix or mitigation with the reporter. Do not disclose
the issue publicly until a fix or disclosure date has been agreed with the
maintainers. The project may credit reporters in a release note only with their
permission.

## Security-sensitive areas

Reports involving these areas are especially important:

- request-forgery or redirect-policy bypasses, including access to private
  networks or disallowed schemes;
- credential exposure through resolved identifiers, logs, diagnostics, or
  cache paths;
- unbounded allocation, response handling, retries, redirects, or decompression;
- cache identity or validator bugs that return bytes from the wrong asset
  revision; and
- memory safety, data races, or incorrect bounds handling in the C++ libraries.

The project's security and network assumptions are documented in
[DESIGN_POLICY.md](docs/design/DESIGN_POLICY.md#10-security-and-trust). A
documented limitation is not automatically a vulnerability, but a report that
shows the implementation violates the stated policy should use the private
channel above.
