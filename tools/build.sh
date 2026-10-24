#!/bin/sh
# Build d2x in the pinned container (docker/Dockerfile).
#
#     tools/build.sh [MAJOR [MINOR]]     build build/d2x-vMAJOR-MINOR/*.app
#     tools/build.sh test                host tests (tests/host) and Python tests
#     tools/build.sh clean               remove build/ and object files
#     tools/build.sh shell               a shell in the build container
#
# Needs Docker. On Windows, run it from Git Bash; if docker is not on PATH it
# uses Docker inside WSL. The image is tagged with a hash of the Dockerfile,
# so editing the Dockerfile rebuilds it.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if command -v docker >/dev/null 2>&1; then
    docker() { command docker "$@"; }
    mount=$root
    ids="$(id -u):$(id -g)"
elif command -v wsl >/dev/null 2>&1; then
    # Git Bash: /c/x -> C:/x for wslpath. MSYS_NO_PATHCONV keeps Git Bash
    # from rewriting the container paths below.
    export MSYS_NO_PATHCONV=1
    docker() { wsl -e docker "$@"; }
    mount=$(wsl -e wslpath -a "$(cygpath -m "$root")")
    ids=$(wsl -e sh -c 'echo $(id -u):$(id -g)')
else
    echo "build.sh: needs docker (or WSL with docker on Windows)" >&2
    exit 1
fi

tag="d2x-build:$(git hash-object docker/Dockerfile | cut -c1-12)"
if ! docker image inspect "$tag" >/dev/null 2>&1; then
    echo "Building image $tag"
    docker build -t "$tag" - < docker/Dockerfile
fi

run() {
    docker run --rm -v "$mount:/src" --user "$ids" -e HOME=/tmp "$@"
}

case "${1:-}" in
shell)
    run -it "$tag" bash ;;
clean)
    run "$tag" sh -c './maked2x.sh clean; make -C tests/host clean' ;;
test)
    run "$tag" sh -c 'make -C tests/host && python3 -m unittest discover -s tests -p "test_*.py"' ;;
*)
    run "$tag" sh -c 'g++ -O2 stripios_src/main.cpp -o stripios && ./maked2x.sh "$@"' \
        build "$@" ;;
esac
