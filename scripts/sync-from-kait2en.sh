#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Sync Apple T2 BCE kernel modules from KaiT2en-Fedora into t2bce
# while preserving commit history, author attribution, dates, full commit messages,
# and upstream commit URLs.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYNC_FILE="$REPO_ROOT/.upstream_kait2en_commit"
DEFAULT_UPSTREAM_URL="https://github.com/kaiT2en/KaiT2en-Fedora.git"
UPSTREAM_URL="${UPSTREAM_URL:-$DEFAULT_UPSTREAM_URL}"
UPSTREAM_DIR="${1:-/tmp/KaiT2en-Fedora}"

# Modules tracked and mapped to t2bce root
TARGET_PATHS=(
  "modules/t2bce_audio-alsa-ucm-conf"
  "modules/t2bce_audio"
  "modules/t2bce_core"
  "modules/t2bce_dma"
  "modules/t2bce_vhci"
  "t2-services/t2-ave/kernel/t2bce_ave"
)

# Ensure committer identity is configured
if ! git config user.name >/dev/null 2>&1; then
  git config user.name "github-actions[bot]"
fi
if ! git config user.email >/dev/null 2>&1; then
  git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
fi

# 1. Clone or update upstream repository
if [ ! -d "$UPSTREAM_DIR/.git" ]; then
  echo "Cloning upstream repository from $UPSTREAM_URL to $UPSTREAM_DIR..."
  git clone "$UPSTREAM_URL" "$UPSTREAM_DIR"
else
  echo "Fetching latest changes from upstream in $UPSTREAM_DIR..."
  git -C "$UPSTREAM_DIR" fetch origin main
fi

# 2. Determine synchronization baseline
# Default baseline: commit f1b26d295dbc718df8714c0364ef81aa3a0ff426 (matching 45db242 in t2bce)
if [ ! -f "$SYNC_FILE" ]; then
  echo "No sync state file found. Initializing baseline to f1b26d295dbc718df8714c0364ef81aa3a0ff426..."
  echo "f1b26d295dbc718df8714c0364ef81aa3a0ff426" > "$SYNC_FILE"
fi

LAST_SYNC=$(tr -d '[:space:]' < "$SYNC_FILE")
HEAD_SYNC=$(git -C "$UPSTREAM_DIR" rev-parse origin/main)

echo "Last synced upstream commit: $LAST_SYNC"
echo "Current upstream HEAD:       $HEAD_SYNC"

if [ "$LAST_SYNC" = "$HEAD_SYNC" ]; then
  echo "Already up to date. No new upstream commits."
  exit 0
fi

# 3. Retrieve list of relevant commits in chronological order
COMMITS=$(git -C "$UPSTREAM_DIR" log --reverse --format="%H" "$LAST_SYNC..$HEAD_SYNC" -- "${TARGET_PATHS[@]}")

if [ -z "$COMMITS" ]; then
  echo "No upstream commits modified the tracked module paths between $LAST_SYNC and $HEAD_SYNC."
  echo "$HEAD_SYNC" > "$SYNC_FILE"
  exit 0
fi

PATCH_TMP=$(mktemp -d)
trap 'rm -rf "$PATCH_TMP"' EXIT

TOTAL=$(echo "$COMMITS" | wc -w)
COUNT=0
echo "Found $TOTAL commits to import from upstream."

# 4. Process and apply each commit
for commit in $COMMITS; do
  COUNT=$((COUNT + 1))
  SUBJECT=$(git -C "$UPSTREAM_DIR" log -1 --format="%s" "$commit")
  AUTHOR=$(git -C "$UPSTREAM_DIR" log -1 --format="%an <%ae>" "$commit")
  echo "[$COUNT/$TOTAL] Processing $commit: '$SUBJECT' by $AUTHOR..."

  # Generate patch containing only target paths
  git -C "$UPSTREAM_DIR" format-patch -1 "$commit" --binary --stdout -- "${TARGET_PATHS[@]}" > "$PATCH_TMP/patch.raw"

  if [ ! -s "$PATCH_TMP/patch.raw" ]; then
    echo "  Skipping: patch is empty for target paths."
    continue
  fi

  # Rewrite diff paths and inject upstream commit link trailer
  python3 - << 'PY_EOF' "$PATCH_TMP/patch.raw" "$PATCH_TMP/patch.clean" "$commit" "$UPSTREAM_URL"
import sys
import re

src = sys.argv[1]
dst = sys.argv[2]
commit_sha = sys.argv[3]
upstream_url = sys.argv[4]

clean_url = upstream_url[:-4] if upstream_url.endswith('.git') else upstream_url
upstream_link = f"{clean_url}/commit/{commit_sha}"

with open(src, 'r', encoding='utf-8', errors='surrogateescape') as f:
    lines = f.readlines()

out = []
in_header = True
trailer_added = False

for line in lines:
    if in_header:
        if line.strip() == '---':
            if not trailer_added:
                out.append(f"\nUpstream: {upstream_link}\n")
                trailer_added = True
            in_header = False
            out.append(line)
            continue
        out.append(line)
    else:
        # Rewrite diff headers to point to t2bce root
        line = re.sub(r'([ \t][ab]/)modules/', r'\1', line)
        line = re.sub(r'([ \t][ab]/)t2-services/t2-ave/kernel/', r'\1', line)
        line = re.sub(r'^(rename (?:from|to) )modules/', r'\1', line)
        line = re.sub(r'^(rename (?:from|to) )t2-services/t2-ave/kernel/', r'\1', line)
        line = re.sub(r'^ (?:modules/|t2-services/t2-ave/kernel/)', ' ', line)
        out.append(line)

with open(dst, 'w', encoding='utf-8', errors='surrogateescape') as f:
    f.writelines(out)
PY_EOF

  # Apply patch preserving author, date, and commit message
  if ! git -C "$REPO_ROOT" am --3way --committer-date-is-author-date "$PATCH_TMP/patch.clean"; then
    echo "ERROR: Conflict occurred while applying upstream commit $commit ($SUBJECT)"
    echo "Resolve conflict and run 'git am --resolved', or abort with 'git am --abort'."
    exit 1
  fi

  echo "$commit" > "$SYNC_FILE"
done

echo "$HEAD_SYNC" > "$SYNC_FILE"
echo "Successfully synchronized all $TOTAL upstream commits up to $HEAD_SYNC."
