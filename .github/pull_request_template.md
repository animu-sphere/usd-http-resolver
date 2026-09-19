## Summary

<!-- What changed, and why? Link the issue or design record when one exists. -->

## Scope and risk

<!-- Note affected modules, public contracts, compatibility, security, and performance implications. -->

## Validation

- [ ] I ran the narrowest relevant test while iterating.
- [ ] I ran the complete core suite, or explained why it was not possible.
- [ ] I ran sanitizer tests when the change affects memory, arithmetic, or concurrency.
- [ ] I recorded the exact commands and any platform limitations below.

```text
# Commands and relevant results
```

## Documentation and tests

- [ ] Tests cover the changed behavior, including a fixed regression case where appropriate.
- [ ] The owning documentation is updated, or no documentation change is needed.
- [ ] Measured I/O behavior and its baseline are updated when the change affects requests or bytes transferred.

## Review checklist

- [ ] This change preserves the core boundary and does not add format knowledge to the resolver.
- [ ] No credentials, private URLs, customer data, or generated build output are included.
- [ ] I have read the [contribution guide](../CONTRIBUTING.md) and [Code of Conduct](../CODE_OF_CONDUCT.md).