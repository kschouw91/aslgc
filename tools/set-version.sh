#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Sets the version for a release, on its release/hotfix branch before finishing it:
#   - CMakeLists.txt: project(aslgc VERSION ...)
#   - CHANGELOG.md:   "## [Unreleased]" or "## [<version>] — unreleased" gets today's date
#   - metainfo:       a <release> entry for the version (date updated if it exists)
# Usage: tools/set-version.sh 1.2.3 [YYYY-MM-DD]
# With --check instead of a date it only verifies that all three agree on the version and that
# the CHANGELOG entry is dated (the release workflow runs this for the pushed tag).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${1:?usage: $0 <version> [date | --check]}"
version="${version#v}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a version: $version (expected X.Y.Z)" >&2; exit 1; }

cmake="$root/CMakeLists.txt"
changelog="$root/CHANGELOG.md"
app_id="$(sed -n 's/^set(ASLGC_APP_ID "\([^"]*\)").*/\1/p' "$cmake")"
metainfo="$root/packaging/$app_id.metainfo.xml"
escaped="${version//./\\.}"

if [[ "${2:-}" == "--check" ]]; then
    errors=0
    cmake_version="$(sed -n 's/^project(aslgc VERSION \([0-9.]*\).*/\1/p' "$cmake")"
    if [[ "$cmake_version" != "$version" ]]; then
        echo "CMakeLists.txt has version $cmake_version, not $version" >&2; errors=1
    fi
    if ! grep -qE "^## \[$escaped\] — [0-9]{4}-[0-9]{2}-[0-9]{2}$" "$changelog"; then
        echo "CHANGELOG.md has no dated '## [$version] — YYYY-MM-DD' heading" >&2; errors=1
    fi
    if ! grep -qE "<release version=\"$escaped\" date=\"[0-9-]+\"" "$metainfo"; then
        echo "$(basename "$metainfo") has no <release version=\"$version\">" >&2; errors=1
    fi
    [[ $errors -eq 0 ]] && echo "Version $version is consistent"
    exit $errors
fi

date="${2:-$(date +%F)}"
[[ "$date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || { echo "Not a date: $date (expected YYYY-MM-DD)" >&2; exit 1; }

sed -i "s/^project(aslgc VERSION [0-9.]*/project(aslgc VERSION $version/" "$cmake"

if grep -qE "^## \[$escaped\]" "$changelog"; then
    sed -i -E "s/^## \[$escaped\].*/## [$version] — $date/" "$changelog"
elif grep -qE "^## \[Unreleased\]" "$changelog"; then
    sed -i -E "s/^## \[Unreleased\].*/## [$version] — $date/" "$changelog"
else
    echo "CHANGELOG.md has neither '## [Unreleased]' nor '## [$version]'; add the release notes first" >&2
    exit 1
fi

if grep -q "<release version=\"$version\"" "$metainfo"; then
    sed -i -E "s|<release version=\"$escaped\" date=\"[0-9-]+\"|<release version=\"$version\" date=\"$date\"|" "$metainfo"
else
    sed -i "s|^  <releases>$|  <releases>\n    <release version=\"$version\" date=\"$date\">\n      <url type=\"details\">https://github.com/kschouw91/aslgc/releases/tag/$version</url>\n    </release>|" "$metainfo"
fi

echo "Version set to $version ($date) in CMakeLists.txt, CHANGELOG.md and $(basename "$metainfo")"
