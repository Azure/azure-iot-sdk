<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Code coverage — C SDK

How coverage of the C SDK is measured and reported.

Scope: first-party C code in `c/src/` and `c/adapters/`. Fetched dependencies (azure-sdk-for-c,
cmocka, Paho), test code and samples are outside the denominator.

---

## Status

- Coverage is measured and published on every pull request that touches `c/`.
- **No coverage number fails a build yet.** Per-component floors and patch coverage are reported
  only. The test-count sentinel below does fail the job.
- Linux (gcc + gcov) only. MSVC builds are not instrumented.

---

## Instrumentation

[`cmake/az_iot_coverage.cmake`](../../cmake/az_iot_coverage.cmake), enabled with
`-DAZ_IOT_ENABLE_COVERAGE=ON` (the `linux-gcc-coverage` preset sets it).

- Flags are applied **per target**, to shipped library targets only. Setting `--coverage`
  globally would instrument the fetched dependencies and pollute the denominator.
- Test executables and test-support libraries are not instrumented.
- The module is a no-op on MSVC.
- A `coverage` custom target runs gcovr, when gcovr is installed.

### Local run

```sh
cmake --preset linux-gcc-coverage
cmake --build --preset linux-gcc-coverage
ctest --preset linux-gcc-coverage --output-on-failure
cmake --build --preset linux-gcc-coverage --target coverage
# -> build/linux-gcc-coverage/coverage/html/index.html
```

The coverage preset uses its own build tree, so it never mixes with a normal debug build.

---

## CI

### Per pull request: `coverage (linux-gcc)` in [ci-c.yml](../../../.github/workflows/ci-c.yml)

Runs unit and conformance suites (against a local MQTT broker, with a SoftHSM2 PKCS#11 token)
under the coverage preset, then:

1. **Test-count sentinel.** `ctest` must discover exactly `EXPECTED_TESTS` suites, or the job
   fails. A misconfigured build that runs zero tests would otherwise report a coverage number
   computed over nothing. Adding or removing a suite means updating `EXPECTED_TESTS`.
2. **Report.** gcovr produces HTML, Cobertura and a JSON summary, uploaded as the `coverage-c`
   artifact.
3. **Per-component summary.** [`tests/coverage_report.py`](../../tests/coverage_report.py) checks
   each component in [`tests/coverage-components.json`](../../tests/coverage-components.json)
   against its line, branch and function floors and writes a table to the job summary. Report-only.
4. **Patch coverage.** `diff-cover` against the merge base reports coverage of the changed lines.
   Report-only.
5. **Tracefile.** [`eng/collect-coverage.sh`](../../eng/collect-coverage.sh) writes a gcovr JSON
   tracefile (`coverage-trace-unit`) for the combined report.

`coverage-gate` is a single, stable check name for branch protection. It passes when the coverage
job did not need to run and fails when it failed.

### Nightly: combined coverage

[ci-c-coverage-combined.yml](../../../.github/workflows/ci-c-coverage-combined.yml) merges the
unit tracefile with those of the nightly e2e workflows (`ci-c-e2e`, `ci-c-e2e-csr`,
`ci-c-e2e-adu`) using [`eng/combine-coverage.sh`](../../eng/combine-coverage.sh).

- It accepts only runs whose `head_sha` is the target commit. Tracefiles from different revisions
  would give a plausible but wrong percentage, and `combine-coverage.sh` does not detect that
  itself.
- It writes per-suite and combined reports (summary, Markdown, Cobertura, lcov).
- `collect-coverage.sh` is the only place the gcovr filters are defined. Tracefiles collected with
  different filters cannot be merged meaningfully.
- A leg that produced no `.gcda` files fails collection, rather than reporting 0%.

---

## Conventions

- **The denominator must not shrink silently.** An adapter dropping out of the build would raise
  the percentage. `coverage_report.py` lists every source file on disk that no report measured,
  and a component prefix that matches nothing is an error.
- **Coverage never runs against provisioned cloud resources on pull requests.** E2E coverage comes
  only from the nightly legs.
- **Tool versions are pinned** (`gcovr`, `diff-cover`) in the workflow, because report formats
  change between versions.
