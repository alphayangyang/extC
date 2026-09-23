# COMMENT-STYLE.md — comment standard for the compiler source (`src/`)

> Applies to every `.c` / `.h` file under `src/`. It is a **rule**, not a taste:
> a comment that does not follow it will be rewritten in review.
> Design documents (`*.md`) and the language corpus (`stdlib/`, `examples/`, `tests/`)
> are out of scope and stay in Chinese.

## 1. Language

**English only, ASCII only.** No Chinese, no full-width punctuation, no emoji-like
symbols (`⭐`, `⚠️`, `✅`, `✗`, `✓`, `⇒`, `·`). Use plain ASCII: `*`, `!`, `->`, `=>`.

The rule covers **everything that belongs to the code**: source comments, user-visible
diagnostics, the generated C (its comments and its runtime preamble), and debug traces
behind the `EXTC_DBG_*` switches. Code and diagnostics should read as one artifact.

> Correction (2026-09-23): an earlier version of this file claimed the compiler's output
> "was always English". That was wrong -- a number of emitted-C banners, the emitted
> runtime preamble, and several `EXTC_DBG_*` traces were still Chinese. Converting those
> string literals is tracked as its own change, because it moves the generated C and the
> golden manifest; see `PLAN.md` section 0.4.1.

## 2. No project-private vocabulary

Comments must be understandable to a C programmer who has never read this project's
documentation. Do not write internal codenames, ruling numbers, plan-item numbers,
or arena-memory appendix references. Describe the mechanism, not the project's
nickname for it.

| Do not write | Write instead |
|---|---|
| "ruling 63", "PLAN #38", "decision 68" | a sentence saying what the rule is |
| "A3 promotion", "layer 2", "step B2" | "escape promotion", "the level solver", "allocator symmetry" |
| "the home arena" alone | "the home arena (the arena chosen by the caller)" |
| "arena pockets", "soak", "sink" | "block arenas", "escape", "promote" |
| "the sentinel state", "pre-null" | "not yet decided", "the provisional pass" |

Names that are real identifiers in the code (`ARENA_HOME`, `minAt`, `arenaLevel`,
`refDepth`, `homeDepth`, `arenaArg`) are fine — they are the subject matter.

## 3. Function comment block

Every function definition carries a comment block directly above it, in this shape:

```c
/* <One-line summary: what it does, in the imperative.>
 *
 * <Why it exists / what invariant it maintains. Include this only when the reason
 *  is not obvious from the summary; it is usually the most valuable part.>
 *
 * Params:
 *   c     - <meaning, or "unused" >
 *   val   - <meaning>
 *   at    - <meaning and units: "arena level the value must reach; 0 = beyond this frame">
 *
 * Returns:
 *   <what the return value means. Omit for void.>
 *
 * Notes:
 *   - <A non-obvious constraint, a caller obligation, or a trap that already caused a bug.>
 */
```

Rules for the block:

- Any of `Params:` / `Returns:` / `Notes:` may be omitted **when empty**. A one-line
  summary is enough for a trivial helper; a subtle analysis function normally needs
  `Params:` + `Returns:` + `Notes:`.
- Document **semantics and units**, not types that the signature already shows.
- State the direction of any approximation: "over-approximates, so it can only cause
  a false rejection" is worth more than restating the code.
- If the function has a precondition the compiler does not enforce, put it in `Notes:`.
- Keep lines within 100 columns.

## 4. Inline comments

- Explain *why*, not *what*. If the code needs a comment to say what it does, rename
  things instead.
- One concern per comment. Do not stack five historical notes on one line.
- Record a past bug as a constraint, not as a story:
  `/* Must run before the escape check: the check reads the depth this call updates. */`

## 5. File header

Each file starts with a short block (2-6 lines) stating the file's responsibility and
where it sits in the pipeline. No changelog, no history.

## 6. Checking a change

`tools/comment_neutral.py <base-revision> [file ...]` extracts the code from both
revisions (comments removed, literals preserved) and compares it byte for byte. Use it
to prove a comment rewrite changed nothing:

```sh
python3 tools/comment_neutral.py HEAD src/check_escape.c
```

It must print `ok ... (comments only)` for every file. A comment rewrite is only
acceptable together with the normal test gates (`./tests/run.sh`, arena corpora).
