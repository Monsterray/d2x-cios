#!/bin/sh
# Build d2x in the pinned container (docker/Dockerfile).
#
#     tools/build.sh                     build the release in VERSION: build/d2x-v11.1.0/*.app
#     tools/build.sh MAJOR [MINOR]       an upstream-style build/d2x-vMAJOR-MINOR (experiments)
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

# SOURCE_DATE_EPOCH, when set, fixes the build date stamped into the modules,
# so two builds of different commits can be compared byte for byte.
# EXTRA_CFLAGS reaches every module's CFLAGS (CI sets -Werror).
run() {
    docker run --rm -v "$mount:/src" --user "$ids" -e HOME=/tmp \
        ${SOURCE_DATE_EPOCH:+-e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH"} \
        ${EXTRA_CFLAGS:+-e EXTRA_CFLAGS="$EXTRA_CFLAGS"} \
        ${D2XNAME:+-e D2XNAME="$D2XNAME"} "$@"
}

case "${1:-}" in
shell)
    run -it "$tag" bash ;;
clean)
    run "$tag" sh -c './maked2x.sh clean; make -C tests/host clean' ;;
test)
    run "$tag" sh -c 'make -C tests/host && python3 -m unittest discover -s tests -p "test_*.py"' ;;
*)
    # Objects depend on their sources and Makefile, not on EXTRA_CFLAGS: when
    # the flags change (say -DEHCI_DEBUG_LOG on or off), build from clean.
    stamp=build/.extra_cflags
    if [ "$(cat "$stamp" 2>/dev/null)" != "${EXTRA_CFLAGS:-}" ]; then
        echo "EXTRA_CFLAGS changed ('${EXTRA_CFLAGS:-}'): building from clean"
        run "$tag" sh -c './maked2x.sh clean' >/dev/null
        mkdir -p build
        printf '%s' "${EXTRA_CFLAGS:-}" > "$stamp"
    fi
    # With no version given, build the release in VERSION (major.minor.patch
    # or major.minor.patch-pre): named d2x-v11.1.0; inside the cIOS the major
    # stays a number (11) and the rest is the version string "-1-0", since
    # ModMii allows no dots there (data/d2x-beta.bat). Loaders show
    # "d2x v11-1-0". Explicit arguments (11 beta3-mr10) build an
    # upstream-style name, for experiments.
    if [ $# -eq 0 ]; then
        v=$(tr -d ' \r\n' < VERSION)
        D2XNAME="d2x-v$v"
        set -- "${v%%.*}" "-$(printf '%s' "${v#*.}" | tr . -)"
        echo "Release $D2XNAME (from VERSION): version $1, version string $2"
    fi
    run "$tag" sh -c 'g++ -O2 -Wall -Wextra $EXTRA_CFLAGS stripios_src/main.cpp -o stripios && ./maked2x.sh "$@"' \
        build "$@" ;;
esac
