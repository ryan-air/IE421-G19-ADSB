#!/usr/bin/env bash
# Builds GenMC (https://github.com/MPI-SWS/genmc) into spec/.tools/ (ignored by
# git) on Ubuntu 24.04, for spec/memory-model/run.sh. Needs LLVM 19; installs
# the packages with apt (sudo if not root). About 10 minutes the first time.
#
#   bash spec/memory-model/build-genmc.sh   # prints the path of the binary
set -euo pipefail
VERSION=v0.20.0
here=$(cd "$(dirname "$0")" && pwd)
dest="$here/../.tools/genmc-$VERSION"
bin="$dest/build/bin/genmc"
if [[ -x $bin ]]; then
  echo "$bin"
  exit 0
fi
sudo=""
[[ $(id -u) -ne 0 ]] && sudo=sudo
$sudo apt-get update -qq >&2
$sudo apt-get install -y -qq git cmake ninja-build g++-14 clang-19 llvm-19 llvm-19-dev \
  libffi-dev zlib1g-dev libedit-dev libzstd-dev >&2
rm -rf "$dest"
git clone -q --depth 1 --branch "$VERSION" https://github.com/MPI-SWS/genmc.git "$dest" >&2
CC=gcc-14 CXX=g++-14 cmake -S "$dest" -B "$dest/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/usr/lib/llvm-19/cmake >&2
cmake --build "$dest/build" >&2
echo "$bin"
