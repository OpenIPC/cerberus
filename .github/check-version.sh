#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# check-version.sh — fail a pull request that changes the code or the build
# but not CERBERUS_VERSION in src/cerberus.c.  A new version must be
# MAJOR.MINOR.PATCH without leading zeros, greater than the base one, and
# have its section in CHANGELOG.md ("## <version> — YYYY-MM-DD") that the
# base branch does not have yet.  src/, the installers, the Makefile and ci/
# count as code or build (a new version is also what publishes a release,
# #32); docs, tests/ and the workflows in .github/ need no new version.
#
# Usage: .github/check-version.sh [BASE]   (default origin/master; CI passes
# HEAD^1, the base side of the pull request's merge commit)
#

set -eu

base=${1:-origin/master}
header=src/cerberus.c
code="src install.sh uninstall.sh Makefile ci"

# The one CERBERUS_VERSION definition (comments and other text do not count).
ver() {
	v=$(sed -n 's/^#define CERBERUS_VERSION "\([^"]*\)"[[:space:]]*$/\1/p')
	if [ "$(printf '%s\n' "$v" | grep -c .)" -ne 1 ]; then
		echo "::error::$1:$header: expected one CERBERUS_VERSION definition," \
			"found: $(printf '%s' "$v" | tr '\n' ' ')" >&2
		exit 1
	fi
	printf '%s\n' "$v"
}

new=$(ver HEAD < "$header")
old=$(git show "$base:$header" | ver "$base")

# shellcheck disable=SC2086 # $code is a list of paths
changed=$(git diff --name-only "$base"...HEAD -- $code ':(exclude,glob)**/*.md')

if [ "$new" = "$old" ]; then
	if [ -n "$changed" ]; then
		echo "::error::code or build changed, CERBERUS_VERSION is still $old:" \
			"bump PATCH (fix) or MINOR (feature) and add its CHANGELOG.md section"
		echo "$changed"
		exit 1
	fi
	echo "version $new unchanged, no code or build changes"
	exit 0
fi

if ! echo "$new" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
	echo "::error::CERBERUS_VERSION \"$new\" is not MAJOR.MINOR.PATCH"
	exit 1
fi

top=$(printf '%s\n%s\n' "$old" "$new" | sort -V | tail -n 1)
if [ "$top" != "$new" ]; then
	echo "::error::CERBERUS_VERSION $new is not greater than $old"
	exit 1
fi

# $new is digits and dots only (checked above): escape the dots, require a date.
re=$(printf '%s' "$new" | sed 's/\./\\./g')
if ! grep -Eq "^## $re — [0-9]{4}-[0-9]{2}-[0-9]{2}\$" CHANGELOG.md; then
	echo "::error::CHANGELOG.md has no section \"## $new — YYYY-MM-DD\""
	exit 1
fi

# The section must come with this pull request, not be one the base
# branch already has (a version taken from there brings no notes).
if git show "$base:CHANGELOG.md" 2>/dev/null | grep -Eq "^## $re — "; then
	echo "::error::the section \"## $new\" is already on $base: take a new version"
	exit 1
fi

echo "version $old -> $new, CHANGELOG.md section found"
