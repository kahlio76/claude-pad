#!/bin/sh
# Builds Claude Pad and copies the binaries to ~/.claude-pad/bin, a stable
# path with no spaces for Claude Code's hooks to call.
set -e
cd "$(dirname "$0")"
swift build -c release
mkdir -p "$HOME/.claude-pad/bin"
chmod 700 "$HOME/.claude-pad"
for b in padd fakepad pad-hook; do cp ".build/release/$b" "$HOME/.claude-pad/bin/"; done
echo "Installed to $HOME/.claude-pad/bin"
echo "Next: merge hooks/settings-hooks.json into ~/.claude/settings.json"
