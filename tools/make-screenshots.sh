#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Regenerates docs/screenshots/*.png from a small demo repository with fictional authors.
# Needs a built ./build/aslgc (or pass the binary as $1) and ImageMagick 7.
# Runs headless and never touches your own git or aslgc configuration.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${1:-$root/build/aslgc}"
[[ -x "$app" ]] || { echo "aslgc binary not found: $app (build first)" >&2; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
demo="$work/weather-cli"
mkdir -p "$demo"
cd "$demo"
git init -q -b main

as() { export GIT_AUTHOR_NAME="$1" GIT_AUTHOR_EMAIL="$2" GIT_COMMITTER_NAME="$1" GIT_COMMITTER_EMAIL="$2"; }
at() { export GIT_AUTHOR_DATE="$1 +0200" GIT_COMMITTER_DATE="$1 +0200"; }
commit() { git add -A && git commit -qm "$1"; }
alex() { as "Alex Rivera" alex@example.com; }
sam() { as "Sam Chen" sam@example.com; }
jordan() { as "Jordan Lee" jordan@example.com; }

alex; at "2026-11-02T09:10"
printf '# weather-cli\n\nShow the forecast in your terminal.\n' > README.md
printf 'import sys\n\n\ndef main():\n    print("weather-cli")\n\n\nif __name__ == "__main__":\n    main()\n' > weather.py
commit "Initial project layout"
sam; at "2026-11-02T11:42"
printf 'def fetch(city):\n    return {"city": city, "temp": 21}\n' > api.py
commit "Add forecast API client"
alex; at "2026-11-03T10:05"
git checkout -qb feature/units
printf 'def to_fahrenheit(c):\n    return c * 9 / 5 + 32\n' > units.py
commit "Add unit conversion helpers"
at "2026-11-03T15:30"
printf 'def to_fahrenheit(c):\n    return c * 9 / 5 + 32\n\n\ndef to_kelvin(c):\n    return c + 273.15\n' > units.py
commit "Support Kelvin output"
jordan; at "2026-11-04T09:20"
git checkout -q main
printf 'requests>=2.31\n' > requirements.txt
commit "Pin dependencies"
alex; at "2026-11-04T14:00"
git merge -q --no-ff --no-edit feature/units
git tag -a v0.2.0 -m "Release 0.2.0"
sam; at "2026-11-05T10:15"
git checkout -qb fix/timeout
printf 'def fetch(city, timeout=10):\n    return {"city": city, "temp": 21}\n' > api.py
commit "Add request timeout"
jordan; at "2026-11-06T16:45"
git checkout -q main
printf '# weather-cli\n\nShow the forecast in your terminal.\n\n    python weather.py <city>\n' > README.md
commit "Document usage"
git clone -q --bare . "$work/remote.git"
git remote add origin "$work/remote.git"
git fetch -q origin
git branch -q -u origin/main
alex; at "2026-11-07T09:00"
printf 'import sys\n\nfrom api import fetch\n\n\ndef main():\n    city = sys.argv[1] if len(sys.argv) > 1 else "Cape Town"\n    print(fetch(city))\n\n\nif __name__ == "__main__":\n    main()\n' > weather.py
commit "Read the city from the command line"
printf 'TODO: caching\n' > notes.txt
git stash push -q -u -m "caching experiment"
# Uncommitted work for the Local Changes screenshot.
cat > weather.py <<'EOF'
import sys

from api import fetch
from units import to_fahrenheit


def main():
    city = sys.argv[1] if len(sys.argv) > 1 else "Cape Town"
    data = fetch(city)
    print(f"{data['city']}: {data['temp']} C / {to_fahrenheit(data['temp']):.0f} F")


if __name__ == "__main__":
    main()
EOF
printf 'requests>=2.32\n' > requirements.txt
git add requirements.txt
printf 'cache/\n' > .gitignore

cd "$root"
shoot() {  # <name> <ASLGC_VIEW> [ASLGC_FILE]
    rm -rf "$work/config" "$work/state"
    XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" QT_QPA_PLATFORM=offscreen \
        ASLGC_SCREENSHOT="$root/docs/screenshots/$1.png" ASLGC_VIEW="$2" ASLGC_FILE="${3:-}" \
        timeout 60 "$app" "$demo" >/dev/null 2>&1
    test -s "$root/docs/screenshots/$1.png"
    echo "captured docs/screenshots/$1.png"
}
shoot history history
shoot changes changes
shoot blame blame weather.py
"$root/tools/optimize-images.sh" --palette "$root"/docs/screenshots/*.png
