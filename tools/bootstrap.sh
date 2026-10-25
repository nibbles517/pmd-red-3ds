#!/bin/sh
# Fetches pret/pmd-red at the pinned commit into build/upstream and applies the
# port's patches. Then put your own ROM at build/upstream/baserom.gba and run
# `make upstream` to extract the game data from it.
set -e
cd "$(dirname "$0")/.."
COMMIT=$(sed -n 's/^commit = "\(.*\)"/\1/p' upstream.lock)
REPO=$(sed -n 's/^repository = "\(.*\)"/\1/p' upstream.lock)

if [ -d build/upstream/.git ]; then
    echo "build/upstream already exists; delete it to bootstrap again."
    exit 1
fi
mkdir -p build
git clone "$REPO" build/upstream
git -C build/upstream checkout --quiet "$COMMIT"
git -C build/upstream apply ../../patches/pmd-red.patch
echo "Done. Copy your ROM to build/upstream/baserom.gba, then run: make upstream && make"
