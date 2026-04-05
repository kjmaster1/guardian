#!/usr/bin/env bash
# tools/record_demo.sh — scripted guardian demo for GIF generation
#
# This script plays through the core guardian features in a way that looks
# natural when recorded with asciinema. Each step has a deliberate pause so
# the GIF reader has time to see the output before the next command runs.
#
# Usage:
#   asciinema rec demo.cast -c "bash tools/record_demo.sh"
#   agg demo.cast demo.gif --speed 1.5
#   # Then add demo.gif to the repo and reference it in README.md
#
# Requirements (Linux / WSL):
#   sudo apt install asciinema
#   curl -sL https://github.com/asciinema/agg/releases/download/v1.4.2/agg-x86_64-unknown-linux-gnu \
#        -o /usr/local/bin/agg && chmod +x /usr/local/bin/agg

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

GUARDIAN="./guardian"
CONFIG="guardian.ini.demo"
SOCK="/tmp/guardian-demo.sock"

# ── helpers ──────────────────────────────────────────────────────────────────

# Print a prompt + command in green, pause, then run it.
run() {
    echo -e "\033[1;32m\$\033[0m \033[1m$*\033[0m"
    sleep 0.6
    eval "$@"
}

pause() { sleep "${1:-1.5}"; }

# Clean up any leftover state from a previous run.
cleanup() {
    "$GUARDIAN" stop "$CONFIG" 2>/dev/null || true
    rm -f "$SOCK" /tmp/guardian-demo*.log
    sleep 0.5
}

# ── demo ─────────────────────────────────────────────────────────────────────

trap cleanup EXIT

cleanup

echo ""
echo "  guardian — process supervisor demo"
echo "  ────────────────────────────────────"
echo ""
pause 1

# Step 1: version
run "$GUARDIAN version"
pause 1.2

# Step 2: start supervisor in background
run "$GUARDIAN start $CONFIG &"
GUARDIAN_PID=$!
pause 2   # give it time to start both services

# Step 3: show status — both services should be RUNNING
run "$GUARDIAN status $CONFIG"
pause 2

# Step 4: kill one managed process to trigger crash detection
echo ""
echo -e "\033[1;33m# Kill the 'counter' service — watch guardian restart it\033[0m"
pause 1

SLEEP_PID=$(pgrep -n sleep 2>/dev/null || true)
if [ -n "$SLEEP_PID" ]; then
    run "kill -9 $SLEEP_PID"
fi
pause 0.5

# Step 5: status immediately — should show BACKOFF
run "$GUARDIAN status $CONFIG"
pause 2

# Step 6: status again — should show RUNNING (restarted)
run "$GUARDIAN status $CONFIG"
pause 1.5

# Step 7: stop everything cleanly
echo ""
run "$GUARDIAN stop $CONFIG"
wait "$GUARDIAN_PID" 2>/dev/null || true
pause 0.8

echo ""
echo "  ✓ All services stopped cleanly."
echo ""
