# Release, platforms and support

## Where the version lives

The single source of truth is the root [`VERSION`](VERSION) file (`0.0.1-alpha` today);
`make`, `extc --version` and the CI badge all read it. Do not write a version literal anywhere else.

## Version policy

`MAJOR.MINOR.PATCH[-alpha]`. While the suffix `-alpha` is present:

- **No interface stability promise.** The language surface, the standard library, the driver switches
  and the generated C may change between minor versions; every break is recorded in the
  [development history](../extc-docs/devlog/changelog/).
- A `MINOR` bump means a feature set, a `PATCH` bump a fix-only release. Nothing in the alpha series
  promises a smooth upgrade path.
- Once `-alpha` is dropped, semver applies to the language and the standard library; the generated C
  remains an implementation detail.

## What `-alpha` promises

What the compiler proves stays proved: a check it can discharge becomes a compile error, and a
check it cannot discharge becomes a trap with a source position — never a silent wrong result. That
is the promise this project is built around, and it holds in every alpha.

Within that, an alpha release promises:

- it builds from a clean checkout with `make` on the supported platforms below;
- `./tests/run.sh` is green, and the `ci` workflow is green on both supported baselines
  (that workflow is the slim correctness subset: it judges "builds and answers correctly", not speed);
- the generated C compiles with a supported `cc` and links only against libc for a minimal program;
- every gate in `check.sh` is honest: a red gate is a real failure, not a warning.

It does **not** promise: ABI or C-API stability, a frozen standard-library surface, performance
numbers (they are snapshots), cross-compilation, or that every design document describes shipped
behaviour — `../extc-docs/plans/` is the list of what is unfinished, and
[`../extc-docs/design/PARALLEL.md`](../extc-docs/design/PARALLEL.md) is explicit about the
multi-threaded reclamation work that is not done.

## Supported platforms

| Platform | Status |
|---|---|
| Linux x86_64, glibc ≥ 2.35 | supported; CI runs ubuntu-22.04 (glibc 2.35, gcc 11) and ubuntu-24.04 (glibc 2.39, gcc 13) |
| WSL2 (x86_64 Linux) | supported |
| aarch64 Linux | builds, but untested: no CI, and the coroutine runtime assumes epoll with the x86_64 calling convention |
| macOS, Windows | not supported (no epoll, and the driver installs POSIX paths) |

The coroutine runtime is bound to epoll; a platform without epoll cannot run the scheduler. Cross
compilation is not supported — build on the target.

## Dependencies

| For | Needs |
|---|---|
| building | `cc` (gcc 11+ or clang 14+), `make` |
| the test suite and gates | `python3` 3.8+ (standard library; on 3.10 and older the `extpkg` suite also needs `tomli` — CI installs `python3-tomli`) |
| full acceptance | additionally `g++`, `rustc` |
| comparison benchmarks | `go`, `java`, `gfortran` (optional; each report says what it used) |

No network access is required at build time or for the gates.

## Installing

```sh
make -j"$(nproc)"
make install PREFIX="$HOME/.local"     # installs extc, the runtime headers and the standard library
```

`EXTC_STD` overrides where the installed compiler looks for the standard library.

## Release checklist

1. Bump [`VERSION`](VERSION) in one commit; `make` rebuilds `build/extc` without warnings.
2. `make -j"$(nproc)" && ./tests/run.sh` — expect `通过 380，失败 0`.
3. `./check.sh nobench` — all gates green, including the documentation gates for this repository.
   (CI runs only the slim `ci` tier; a release must also judge the gates whose lines are
   calibrated on a lab machine, which is what `nobench` adds. Neither tier judges benchmarks:
   those need a quiet machine, so `full` is a separate, non-CI run.)
   If this release touched codegen, first run `./check.sh ratchet` and confirm every diff in the
   frozen artifacts is the intended one (then re-freeze deliberately, and record it below).
4. With the documentation project checked out next to it: `./check.sh docs` — the five
   `doc-project-*` items run against `~/extc-docs` and must be green; also run that project's own
   `check.sh`. A missing documentation project is a skip, not a pass, so check it out for a release.
5. Confirm the `../extc-docs/devlog/changelog/` pages cover the release and its month table is
   current: `python3 tools/check_changelog.py --gate`.
6. Tag this repository and the documentation project **separately** (`v0.0.1-alpha` for the code,
   its own tag for the docs project) — they have independent histories.
7. Announce with: version, supported platforms, the alpha promise above, and the link to
   [`../extc-docs/devlog/changelog/`](../extc-docs/devlog/changelog/).

## Reporting a problem

Include `./build/extc --version`, the smallest `.extc` file that shows the problem, what you
expected, and what happened. Add `--no-line-map` if the problem is in the generated C, and the
generated C itself if you suspect the compiler emitted the wrong code. Security-relevant reports
(a program that silently violates a check) are the highest priority: state that it is one in the
report title.
