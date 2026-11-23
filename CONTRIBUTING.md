# Contributing to extC

Thanks for helping. This repository holds the compiler, its standard library, the tests and the
benchmarks. Today's documentation is a **separate project** — read
[`CONTRIBUTING#the-two-repositories`](#the-two-repositories) before opening a docs change.

## The two repositories

| Repository | Holds |
|---|---|
| this one (`~` relative: `../extC_Compiler`) | `src/` the compiler, `stdlib/`, `tests/`, `bench/`, `examples/`, and the five pages under `docs/` that change with the code |
| [`../extc-docs`](../extc-docs/) | `guide/`, `reference/`, `design/`, `plans/`, `devlog/` (changelog and analysis), `archive/` |

They are checked out as **siblings**, not nested. Gates that read the documentation project resolve
it as `EXTC_DOCS`, defaulting to `$HOME/extc-docs`; the documentation project's own gates resolve
this repository as `EXTC_SRC`, defaulting to `../extC_Compiler`. Without the sibling checkout the
cross-repository doc gates print `  --   跳过：文档项目 ~/extc-docs 不存在（文档独立维护）` and exit 0 —
they never fail for a missing project.

Rule of thumb: *would this sentence be wrong if the code changed?* Then it belongs in this
repository's `docs/` (build facts, generated pages, comment rules). Everything about usage,
mechanisms, plans and history belongs in the documentation project.

## Two minutes

```sh
make -j"$(nproc)"                            # needs only cc
./build/extc --run examples/hello-world.extc  # generate C, compile, run
./tests/run.sh                               # regressions only (fast)
./check.sh quick                             # regressions + gates + subsystem suites
```

| Task | Needs |
|---|---|
| `make` | `cc` (gcc 11+ or clang 14+), `make` |
| `./tests/run.sh` | `python3` (3.8+) |
| `./check.sh quick` / `ci` | `cc`, `python3` |
| `./check.sh` (full) | also `g++`, `rustc` (the corpus runs under ASan/UBSan) |
| `bench/bigmatrix/` | also `go`, `java`; `bench/fortran/` needs `gfortran` |

## Before you change something

- Read the mechanism the change touches:
  [`../extc-docs/design/CORE.md`](../extc-docs/design/CORE.md),
  [`../extc-docs/design/TYPE-MODEL.md`](../extc-docs/design/TYPE-MODEL.md),
  [`../extc-docs/design/MEMORY-MODEL.md`](../extc-docs/design/MEMORY-MODEL.md).
- Check whether it is already on the list: [`../extc-docs/plans/TODO.md`](../extc-docs/plans/TODO.md).
- Find the regression directory that already guards the area (`tests/<area>/`); a change without a
  test is a change nobody can keep correct. Criteria are organised as described in
  [`tests/README.md`](tests/README.md).
- Generated pages are never edited by hand: `docs/flags.md` comes from `tools/gen_flags.py`,
  `docs/surface.md` from `tools/manual_surface.py`, `stdlib/INDEX` from `tools/gen_index.py`.

## Steps to land a change

1. `make -j"$(nproc)"` — a warning is a bug; fix it rather than hiding it.
2. Run the regression directories you touched, then `./tests/run.sh` — the whole suite must stay
   green (positive cases run right, compile-error cases are caught, run-time cases trap with a
   source position).
3. `./check.sh quick` — gates, meta-gates and the subsystem suites.
4. Update the documentation where it lives: a code-coupled fact goes into this repo's `docs/`;
   a usage/mechanism/plan change goes into `../extc-docs/` (follow
   [`../extc-docs/STYLE.md`](../extc-docs/STYLE.md) — status block, no line-number anchors, no
   history).
5. Record the change in the development history: add an entry to
   `../extc-docs/devlog/changelog/YYYY-MM.md` and let
   `python3 tools/check_changelog.py --write` regenerate the month table in that directory's
   `README.md`. `../extc-docs/archive/` is frozen — do not edit it.

## Code conventions

| Rule | Where it is enforced |
|---|---|
| Comments state facts; no history, no "this used to be" | `tools/check_no_history.py` |
| Comment format and example rules | [`docs/COMMENT-STYLE.md`](docs/COMMENT-STYLE.md) |
| Any diagnostic needs a source position | tests that assert the trap text |
| New driver switch must appear in `--help` | `tools/check_manual.py`, `tools/gen_flags.py --check` |
| New public API must be documented | `tools/manual_surface.py --check`, the manual coverage ratchet |
| No new build dependency | `make` needs `cc` only; helper scripts need `python3` |
| C is C11, warnings are errors | `make` |

## Gates

```sh
./tests/run.sh          # 377 cases: behaviour only
./check.sh quick        # + static gates and subsystem suites
./check.sh ci           # CI tier: slim correctness subset (no performance criteria)
./check.sh docs         # documentation gates (also the doc-project-* gates when the docs project is checked out)
./check.sh              # full acceptance (ASan/UBSan corpus + benchmarks)
```

The invariants (size, escape soundness, differential, instance wiring, attack library) are not
samples: each gate compares against a baseline, and ratchet baselines may only shrink. A red gate
blocks the change; if a gate is wrong, fix the gate in its own change first.

## Releasing

See [`RELEASE.md`](RELEASE.md). Version comes from the root `VERSION` file; a release tags this
repository and the documentation project separately (each has its own history).
