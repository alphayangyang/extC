#!/usr/bin/env bash
# Build the Rust arm of the realsvc benchmark.
#   ./rust/build.sh   ->   ../build/realsvc_rust
# Plain rustc, no cargo, no crates.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ../build
rustc -O --edition 2021 -o ../build/realsvc_rust main.rs
