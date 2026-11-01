#!/bin/sh
# bench/realsvc/go/build.sh — build the Go arm of the realsvc benchmark.
#
# Output: ../build/realsvc_go (same directory the other arms build into).
#
# The arm is a single std-only `package main`, and this tree has no go.mod, so
# the build asks Go for GOPATH mode explicitly (GO111MODULE=off). That is the
# same setting that makes `go run .` work in this directory; without it module
# mode refuses to run a package outside a module. `go run main.go ...` and
# `go build main.go` work either way.
set -eu
cd "$(dirname "$0")"
mkdir -p ../build
GO111MODULE=off go build -trimpath -o ../build/realsvc_go .
echo "built $(pwd)/../build/realsvc_go"
