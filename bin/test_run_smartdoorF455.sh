#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

TMPDIR=$(mktemp -d /tmp/run_smartdoorF455-test.XXXXXX)
cleanup() {
    if [ -n "${LAUNCHED_PID:-}" ]; then
        kill "$LAUNCHED_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

SCRIPT_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/run_smartdoorF455.sh"
if [ ! -x "$SCRIPT_SRC" ]; then
    echo "run_smartdoorF455.sh is not executable" >&2
    exit 1
fi

cp "$SCRIPT_SRC" "$TMPDIR/run_smartdoorF455.sh"
chmod +x "$TMPDIR/run_smartdoorF455.sh"

cat > "$TMPDIR/smartdoorF455" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > started.args
while :; do sleep 1; done
EOF
chmod +x "$TMPDIR/smartdoorF455"

cat > "$TMPDIR/custom-config.toml" <<'EOF'
[dummy]
value = "ok"
EOF

cd "$TMPDIR"

# Sanity: status should say not running
STATUS_OUTPUT=$(bash ./run_smartdoorF455.sh status 2>&1 || true)
echo "$STATUS_OUTPUT" | grep -q "not running"

echo "OK: status before start"

# Start with custom config and no taskset (keep logs inside the test sandbox)
bash ./run_smartdoorF455.sh start --config "$TMPDIR/custom-config.toml" --logdir "$TMPDIR/log" --no-taskset
sleep 1

if [ ! -f "$TMPDIR/log/smartdoorF455.pid" ]; then
    echo "PID file not created" >&2
    exit 1
fi
LAUNCHED_PID=$(<"$TMPDIR/log/smartdoorF455.pid")
if ! kill -0 "$LAUNCHED_PID" >/dev/null 2>&1; then
    echo "Process did not start" >&2
    exit 1
fi

echo "OK: process started with PID $LAUNCHED_PID"

# Verify the fake program received the config path
if [ ! -f "$TMPDIR/started.args" ]; then
    echo "started.args file not created" >&2
    exit 1
fi
read -r RECEIVED_CONFIG < "$TMPDIR/started.args"
if [ "$RECEIVED_CONFIG" != "$TMPDIR/custom-config.toml" ]; then
    echo "Config path not passed correctly: got '$RECEIVED_CONFIG'" >&2
    exit 1
fi

echo "OK: config path passed correctly"

# Status should now report running
STATUS_OUTPUT=$(bash ./run_smartdoorF455.sh status)
echo "$STATUS_OUTPUT" | grep -q "running with PID"

echo "OK: status while running"

# Stop the service
bash ./run_smartdoorF455.sh stop
sleep 1

if kill -0 "$LAUNCHED_PID" >/dev/null 2>&1; then
    echo "Process still running after stop" >&2
    exit 1
fi

echo "OK: process stopped"

# Final status should report not running
STATUS_OUTPUT=$(bash ./run_smartdoorF455.sh status || true)
echo "$STATUS_OUTPUT" | grep -q "not running"

echo "OK: status after stop"

echo "Regression test passed"
