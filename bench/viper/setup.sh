#!/bin/sh
# Prepare the Viper toolchain pieces this harness needs, without touching the system.
#   1) MLIR 21 tools (mlir-opt, mlir-translate) unpacked from Ubuntu debs into /tmp/mlirroot
#   2) a link wrapper that drops the Windows import libs ViperC emits on Linux
# Usage: sh bench/viper/setup.sh   then export the two variables it prints.
set -e
VIPER_HOME=${VIPER_HOME:-/tmp/viper}
MLIR_HOME=${MLIR_HOME:-/tmp/mlirroot}
VER=21.1.8-6ubuntu1
BASE=https://archive.ubuntu.com/ubuntu/pool/universe/l/llvm-toolchain-21

mkdir -p "$MLIR_HOME" "$VIPER_HOME"
if [ ! -x "$MLIR_HOME/usr/lib/llvm-21/bin/mlir-opt" ]; then
  cd "$VIPER_HOME"
  for pkg in libmlir-21 mlir-21-tools; do
    deb="${pkg}_${VER}_amd64.deb"
    [ -s "$deb" ] || curl -fL -C - -o "$deb" "$BASE/$deb"
    dpkg -x "$deb" "$MLIR_HOME"
  done
fi

cat > "$VIPER_HOME/linkwrap" <<EOF
#!/bin/sh
# ViperC hard-codes Windows import libs in its link line; drop them and link with clang.
args=""
for a in "$@"; do
  case "$a" in
    -lmsvcrt|-lucrt|-lmingwex|-lkernel32) ;;
    *) args="$args $a" ;;
  esac
done
exec clang $args -lm
EOF
chmod +x "$VIPER_HOME/linkwrap"

echo "export PATH=$MLIR_HOME/usr/lib/llvm-21/bin:\$PATH"
echo "export LD_LIBRARY_PATH=$MLIR_HOME/usr/lib/x86_64-linux-gnu:$MLIR_HOME/usr/lib/llvm-21/lib:\$LD_LIBRARY_PATH"
