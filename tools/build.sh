#!/bin/sh
# Build d2x in the pinned container (docker/Dockerfile).
#
#     tools/build.sh                     build the release in VERSION: build/d2x-v11.1.0/*.app
#     tools/build.sh MAJOR [MINOR]       an upstream-style build/d2x-vMAJOR-MINOR (experiments)
#     tools/build.sh test                host tests (tests/host) and Python tests
#     tools/build.sh version             print the name and version string, no build
#     tools/build.sh clean               remove build/ and object files
#
# Variants: D2X_VARIANT=sdhs tools/build.sh builds 11.1.0-rc1+sdhs, version
# string "-1-0-rc1+sdhs". A build with EXTRA_CFLAGS (other than -Werror) must
# name a variant, so it can never look like the release. See CONTRIBUTING.md.
#     tools/build.sh shell               a shell in the build container
#
# Needs Docker. On Windows, run it from Git Bash; if docker is not on PATH it
# uses Docker inside WSL. The image is tagged with a hash of the Dockerfile,
# so editing the Dockerfile rebuilds it.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# Name and version string (see CONTRIBUTING.md, "Versions"). Checked before
# anything else, so a bad variant fails in a second, not after a build.
die() { echo "build.sh: $*" >&2; exit 1; }
case "${1:-}" in
shell|clean|test) ;;
*)
    variant=${D2X_VARIANT:-}
    if [ -n "$variant" ] && ! printf '%s' "$variant" | grep -Eq '^[a-z0-9]+(-[a-z0-9]+)*$'; then
        die "D2X_VARIANT '$variant' must be lower-case letters and digits, words joined by '-' (sdhs, usbs, dbg, trace-dbg)"
    fi
    flags=${EXTRA_CFLAGS:-}
    if [ -n "$flags" ] && [ -z "$variant" ] && [ "$(printf '%s' "$flags" | tr -d ' ')" != "-Werror" ]; then
        die "EXTRA_CFLAGS='$flags' needs D2X_VARIANT (say D2X_VARIANT=dbg): a flagged build must not look like the release"
    fi
    suffix=${variant:++$variant}
    if [ "${1:-}" = "version" ] || [ $# -eq 0 ]; then
        v=$(tr -d ' \r\n' < VERSION)
        D2XNAME="d2x-v$v$suffix"
        major=${v%%.*}
        vstring="-$(printf '%s' "${v#*.}" | tr . -)$suffix"
    else
        # upstream-style experiment: build.sh 11 beta4-test
        D2XNAME=
        major=$1
        vstring="${2:-unknown}$suffix"
        [ "$suffix" ] && D2XNAME="d2x-v$1-$vstring"
    fi
    if [ ${#vstring} -gt 15 ]; then
        die "version string '$vstring' is ${#vstring} characters; the cIOS holds 15. Shorten the variant '$variant' (the string is the VERSION after the major, dots as '-', then +variant)"
    fi
    commit=$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
    [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ] && commit="$commit-dirty"
    # A debug build (-DD2X_DEBUG) answers this through /dev/d2xdbg
    D2X_IDENT="$vstring@$commit"
    D2XINFO="${D2XNAME:-d2x-v$major-$vstring}: version $major, version string $vstring, commit $commit, variant ${variant:-none}, flags ${flags:-none}"
    if [ "${1:-}" = "version" ]; then
        echo "name=${D2XNAME:-d2x-v$major-$vstring}"
        echo "major=$major"
        echo "string=$vstring"
        echo "commit=$commit"
        echo "info=$D2XINFO"
        exit 0
    fi ;;
esac

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
# so two builds of different commits can be compared byte for byte. Unset, it
# is the last commit's time, read here: in a git worktree the container can't
# see the repository, and maked2x.sh then fell back to the wall clock.
# EXTRA_CFLAGS reaches every module's CFLAGS (CI sets -Werror).
[ -n "${SOURCE_DATE_EPOCH:-}" ] || SOURCE_DATE_EPOCH=$(git log -1 --pretty=%ct 2>/dev/null) || true
run() {
    docker run --rm -v "$mount:/src" --user "$ids" -e HOME=/tmp \
        ${SOURCE_DATE_EPOCH:+-e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH"} \
        ${EXTRA_CFLAGS:+-e EXTRA_CFLAGS="$EXTRA_CFLAGS"} \
        ${D2XNAME:+-e D2XNAME="$D2XNAME"} ${D2XINFO:+-e D2XINFO="$D2XINFO"} \
        ${D2X_IDENT:+-e D2X_IDENT="$D2X_IDENT"} "$@"
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
    # With no arguments, build the release in VERSION as computed above: named
    # d2x-v11.1.0-rc1 (+variant); inside the cIOS the major stays a number
    # (11) and the rest is the version string "-1-0-rc1", since ModMii allows
    # no dots there (data/d2x-beta.bat). Loaders show "d2x v11-1-0-rc1".
    # Explicit arguments (11 beta3-mr10) build an upstream-style name.
    if [ $# -eq 0 ]; then
        set -- "$major" "$vstring"
        echo "Release $D2XNAME: version $1, version string $2"
    else
        set -- "$major" "$vstring"
    fi
    run "$tag" sh -c 'g++ -O2 -Wall -Wextra $EXTRA_CFLAGS stripios_src/main.cpp -o stripios && ./maked2x.sh "$@"' \
        build "$@"
    printf '%s\n' "$D2XINFO" > build/BUILD.txt ;;
esac
