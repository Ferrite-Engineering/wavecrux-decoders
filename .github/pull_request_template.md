## What and why

<!-- What changed, and the problem it solves. Link the issue if there is one. -->

## How it was verified

- [ ] `ctest --preset dev` and `ctest --preset asan` pass, with no skips
- [ ] `format-check`, `tidy` and `cppcheck` targets are clean
- [ ] New behaviour has tests whose expected values come from the specification or an independent source, not from the decoder's own output
- [ ] Fixtures regenerated with `generate_fixtures.py` if the generator changed, and the diff reviewed line by line

## Documentation changed with it

<!-- README, CHANGELOG (Unreleased), SPEC, standards: which, or "none needed" and why. -->
