#!/bin/sh
# latest-release.sh <version>: the tag of the newest AIGW OS release older than
# <version> (the upgrade tests start from it). GH_TOKEN, if set, avoids GitHub's
# rate limit for anonymous API calls.
set -eu
want=${1:?usage: latest-release.sh <version>}
API=${AIGW_RELEASES_API:-https://api.github.com/repos/sudeepksingh/aigw/releases?per_page=20}
curl -fsSL --retry 3 ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} "$API" |
	python3 -c '
import json, sys
want = tuple(int(x) for x in sys.argv[1].split("."))
tags = [r["tag_name"] for r in json.load(sys.stdin) if not r.get("draft") and not r.get("prerelease")]
older = [t for t in tags if t.startswith("v") and tuple(int(x) for x in t[1:].split(".")) < want]
older.sort(key=lambda t: tuple(int(x) for x in t[1:].split(".")))
if not older:
    sys.exit("no release older than " + sys.argv[1])
print(older[-1])' "$want"
