#!/bin/bash
#
# apply_overlay.sh - copy the modified common-repository files from overlay/
#                    into a synced openvela working tree.
#
# Usage:
#   ./tools/apply_overlay.sh /path/to/openvela
#
# Why this exists:
#   Most of this submission lives in common repositories (nuttx, packages,
#   apps) that a team repository cannot push to directly. overlay/ carries the
#   final version of every such file under its openvela working-tree path, so
#   reviewers can read the code directly and a synced tree can be brought to
#   the exact submitted state with one command.
#
#   patches/ carries the same changes as unified diffs for the files where a
#   diff is meaningful. Use whichever form you prefer; they describe the same
#   tree state.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
OVERLAY="$REPO/overlay"

if [ $# -ne 1 ]; then
  echo "usage: $0 <openvela-working-tree>" >&2
  exit 2
fi

TREE=$1

if [ ! -d "$TREE" ]; then
  echo "error: no such directory: $TREE" >&2
  exit 2
fi

if [ ! -d "$OVERLAY" ]; then
  echo "error: overlay/ not found next to tools/" >&2
  exit 2
fi

echo "openvela tree : $TREE"
echo "overlay source: $OVERLAY"
echo

count=0
cd "$OVERLAY" || exit 1

find . -type f -print | sed 's|^\./||' | sort | while IFS= read -r rel; do
  dest="$TREE/$rel"
  mkdir -p "$(dirname "$dest")"
  if [ -f "$dest" ]; then
    cp -a "$rel" "$dest"
    echo "updated: $rel"
  else
    cp -a "$rel" "$dest"
    echo "created: $rel"
  fi
done

echo
echo "done. files applied from overlay/."
echo
echo "note: app/chatui and board/contest_board are linked into the working tree"
echo "      by contest2026_412_SuperBen.xml (<linkfile>) after 'repo sync'."
