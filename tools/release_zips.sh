#!/bin/bash
# Downloads the .zip assets of every published GitHub release of this
# repository into <dir>/<tag>/. Needs gh with GH_TOKEN (or a login).
#
#   tools/release_zips.sh <dir>
set -euo pipefail

dir=${1:?usage: release_zips.sh <dir>}
mkdir -p "$dir"
gh release list --limit 1000 --exclude-drafts --json tagName --jq '.[].tagName' |
  while read -r tag; do
    gh release download "$tag" --pattern '*.zip' --dir "$dir/$tag"
    printf '%s:' "$tag"; printf ' %s' "$dir/$tag"/*.zip; echo
  done
