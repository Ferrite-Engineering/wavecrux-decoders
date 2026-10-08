# Security policy

## Reporting a vulnerability

**Please do not open a public issue.** Email
[support@ferriteengineering.com](mailto:support@ferriteengineering.com) with
`Security` in the subject line.

Useful things to include, as far as you have them: the decoder and its
version, the platform, what an attacker can do, and the smallest waveform file
(or `config_json`) that shows it. A file that reproduces the problem is worth
more than a description of one.

## What to expect

A human acknowledgement within five business days, then our assessment of the
impact and what we intend to do. If we do not think it is a vulnerability we
will say so and why. We credit reporters by name in the release notes if they
wish, and we will not involve lawyers over a good-faith report.

## Why decoders are in scope

A decoder plugin is native code running inside WaveCrux with the user's full
rights, fed a waveform the user may have received from someone else. So any
input that makes a decoder in this repository crash, hang, leak without bound,
or read or write out of bounds is a vulnerability, and the highest-value
report this repository can receive. We fix the latest release of each decoder.

## Not in scope here

- Flaws in WaveCrux itself, including how it loads plugins: report those to
  WaveCrux under the same address.
- Decoders hosted in other repositories and listed in the catalog: report to
  their maintainer, and tell us too so we can delist a decoder that is not
  fixed.
