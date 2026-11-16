/* ---------------------------------------------------------------- debug printing
 *
 * Recursively printing every field of a struct is something only the compiler
 * can do, the equivalent of Rust's `#[derive(Debug)]`: extC has no reflection,
 * so "walk all fields" cannot be written in the language at all.
 *
 * This is not the same as putting the standard library inside the compiler. What
 * the compiler emits is data plus one generic printer, not one printer per type:
 * see genStructDesc above and the `extc_print` at the top of the generated file.
 * The boundary is the rule that whatever can be expressed in extC belongs in
 * extC, not in the compiler.
 */

