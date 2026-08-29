#!/bin/sh
#
# Assemble the Indispensable Labs apps repository from the .hpkg files in
# packaging/out/, into packaging/out-repo/.
#
# MUST RUN ON HAIKU -- it needs `package_repo`.
#
# ---------------------------------------------------------------------------
# NO SIGNING EXISTS
# ---------------------------------------------------------------------------
#
# Haiku has no package signing. The only integrity is a checksum chain that
# never leaves the origin: repo.sha256 -> repo -> per-package checksum -> .hpkg.
# That detects a corrupted download; it cannot detect a substituted one,
# because whoever can replace a .hpkg can replace the index that names its
# checksum. So whoever can write to the bucket controls what these users run.
#
# This repository carries ordinary userland apps rather than kernel drivers, so
# the blast radius is smaller than Media-OS's -- but it is not zero, and it is
# the reason SHA256SUMS is emitted below for publication somewhere structurally
# independent of the bucket (the git repo / a GitHub release). That is the only
# out-of-band check a suspicious user can make.

set -e

PKGDIR=$(cd "$(dirname "$0")" && pwd)
INDIR="$PKGDIR/out"
OUTDIR="$PKGDIR/out-repo"

# --- configuration ---------------------------------------------------------
#
# The base URL is load-bearing and hard to change later: `pkgman add-repo`
# stores the base URL the user typed into their local repo config and keeps it.
# Moving hosts later means every existing user must drop-repo and add-repo.
#
# NO HAIKU RELEASE IN THIS PATH -- compatibility is per package, via
# `requires { haiku >= ... }`. Must stay in step with URL_PATH in
# publish-repo.sh; that script fails the publish if the two ever drift.
BASEURL_ROOT="https://hpkg.indispensablelabs.com/apps"

if [ "$(uname -s)" != "Haiku" ]; then
	echo "ERROR: this must run on Haiku -- it needs package_repo." >&2
	exit 1
fi
command -v package_repo >/dev/null 2>&1 || {
	echo "ERROR: package_repo not found." >&2; exit 1; }

case "$(uname -m)" in
	x86_64)	ARCH=x86_64 ;;
	BePC|x86)
		if [ -d /boot/system/develop/headers/x86_gcc2 ] \
			|| gcc -dumpversion 2>/dev/null | grep -q '^2\.'; then
			ARCH=x86_gcc2
		else
			ARCH=x86
		fi
		;;
	*)	ARCH=$(uname -m) ;;
esac
BASEURL="$BASEURL_ROOT/$ARCH/current"

if [ ! -d "$INDIR" ] || [ -z "$(ls "$INDIR"/*.hpkg 2>/dev/null)" ]; then
	echo "ERROR: no .hpkg files in $INDIR -- run build-package.sh first." >&2
	exit 1
fi

echo "Indispensable Labs apps repository build"
echo "  arch:    $ARCH"
echo "  baseurl: $BASEURL"
echo "  output:  $OUTDIR"
echo

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/packages"

# --- select packages -------------------------------------------------------
#
# out/ accumulates across versions and across experiments. Selecting by name
# alone is ambiguous the moment a second version of the same package exists,
# and `ls | head -1` picks ALPHABETICALLY -- which on the Media-OS repo once
# silently assembled an entire release from a stale build while every check
# passed. So: group by package name, and refuse outright if any name has more
# than one file present.
COUNT=0
NAMES=$(ls "$INDIR"/*.hpkg | xargs -n1 basename | sed 's/-[0-9].*//' | sort -u)
for name in $NAMES; do
	matches=$(ls "$INDIR/$name-"*.hpkg 2>/dev/null || true)
	nmatch=$(printf '%s\n' "$matches" | grep -c . || true)
	if [ "$nmatch" -gt 1 ]; then
		echo "ERROR: '$name' is ambiguous -- $nmatch files match:" >&2
		printf '  %s\n' $matches >&2
		echo "Remove the ones you are not publishing and re-run." >&2
		exit 1
	fi

	p=$matches
	base=$(basename "$p")

	# Every package must match the repository architecture or be `any`;
	# package_repo enforces this too (RepositoryWriterImpl.cpp:415-423) but
	# with a worse message.
	declared=$(package list -i "$p" 2>/dev/null \
		| sed -n 's/^[[:space:]]*architecture:[[:space:]]*//p' | head -1)
	case "$declared" in
		"$ARCH"|any) ;;
		"")	echo "ERROR: $base -- could not read its architecture" >&2; exit 1 ;;
		*)	echo "ERROR: $base declares arch '$declared', not $ARCH or any" >&2
			exit 1 ;;
	esac
	case "$base" in
		*-"$declared".hpkg) ;;
		*)	echo "ERROR: $base -- filename arch disagrees with declared '$declared'" >&2
			exit 1 ;;
	esac

	cp "$p" "$OUTDIR/packages/$base"
	COUNT=$((COUNT + 1))
	printf '  %-52s %s\n' "$base" "$declared"
done
echo "  $COUNT package(s)"
echo

# --- index -----------------------------------------------------------------
sed -e "s|@BASEURL@|$BASEURL|g" \
    -e "s|@ARCH@|$ARCH|g" \
    "$PKGDIR/repo-info.in" > "$OUTDIR/repo.info"

( cd "$OUTDIR" && package_repo create -q repo.info packages/*.hpkg )
[ -f "$OUTDIR/repo" ] || { echo "ERROR: package_repo produced no 'repo'" >&2; exit 1; }

( cd "$OUTDIR" && sha256sum repo | awk '{print $1}' > repo.sha256 )
( cd "$OUTDIR/packages" && sha256sum *.hpkg > ../SHA256SUMS )

echo "=== verifying the index reads back ==="
package_repo list "$OUTDIR/repo" | head -20
echo
echo "=== result ==="
find "$OUTDIR" -type f | sort | while read -r f; do
	printf '  %8s  %s\n' "$(stat -c %s "$f")" "${f#$OUTDIR/}"
done
echo
echo "Next: packaging/publish-repo.sh   (do NOT just rsync the directory --"
echo "      the upload ORDER matters; see that script's header)"
