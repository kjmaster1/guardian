#!/usr/bin/env python3
"""
integration_test.py — End-to-end integration tests for guardian.

These tests exercise the REAL guardian binary against REAL child processes.
Unlike unit tests (which call individual C functions), integration tests
observe guardian's external behaviour:

  - Does a service actually reach RUNNING state?
  - Does a crash trigger BACKOFF, then a restart?
  - Does max_retries eventually lead to FAILED?
  - Does 'guardian stop' shut everything down cleanly?

Usage (called by 'make integration-test'):
  python3 tests/integration_test.py <guardian-binary> <helper-binary>

Arguments:
  guardian-binary   Path to the compiled guardian executable
  helper-binary     Path to tests/helper — the controllable test service

The test framework mirrors the C test_framework.h style so both suites
produce consistent output. Pass/fail counts are printed at the end.
Exit code 0 = all passed, 1 = any failed.
"""

import subprocess
import sys
import os
import time
import tempfile
import shutil
import platform

# ─── Test framework ──────────────────────────────────────────────────────────
#
# Mirrors the C test_framework.h macros so output looks the same as
# 'make test'. Keeps all state in module-level variables.

_passed       = 0
_failed       = 0
_current_test = "(no test active)"


def TEST(name):
    global _current_test
    _current_test = name


def PASS():
    global _passed
    _passed += 1
    print(f"  PASS  {_current_test}")


def FAIL(detail=""):
    global _failed
    _failed += 1
    print(f"  FAIL  {_current_test}", file=sys.stderr)
    if detail:
        print(f"        {detail}", file=sys.stderr)


def ASSERT(condition, detail=""):
    """Assert a boolean condition — passes or fails the current test."""
    if condition:
        PASS()
    else:
        FAIL(detail)


def SUMMARY():
    total = _passed + _failed
    print(f"\n  {total} test(s) run. {_passed} passed. {_failed} failed.\n")
    return _failed == 0


# ─── Guardian helpers ────────────────────────────────────────────────────────

IS_WINDOWS = platform.system() == "Windows"


def write_config(directory, filename, content):
    """Write an INI config to directory/filename and return the full path."""
    path = os.path.join(directory, filename)
    with open(path, "w") as f:
        f.write(content)
    return path


def start_guardian(guardian, config):
    """
    Launch 'guardian start <config>' as a background process.

    Returns the Popen object. The caller is responsible for calling
    stop_guardian() to clean up — otherwise the child process leaks.
    """
    return subprocess.Popen(
        [guardian, "start", config],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def get_status(guardian, config):
    """
    Run 'guardian status <config>' and return stdout as a string.

    Returns an empty string if the command times out or fails (e.g. guardian
    hasn't opened the IPC socket yet — normal during the first few ticks).
    """
    try:
        result = subprocess.run(
            [guardian, "status", config],
            capture_output=True,
            text=True,
            timeout=5,
        )
        return result.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return ""


def service_is_in_state(status_output, service_name, state):
    """
    Return True if the status output contains a line with both
    'service_name' and 'state' on the same line.

    The status table looks like:
      SERVICE          STATE      PID       RESTARTS  UPTIME
      run-forever      RUNNING    12483     0         4s
    """
    for line in status_output.splitlines():
        if service_name in line and state in line:
            return True
    return False


def wait_for_state(guardian, config, service, state, timeout_s=5):
    """
    Poll 'guardian status' every 200ms until the named service reaches
    the given state, or timeout_s seconds elapse.

    Returns True if the state was reached, False on timeout.

    Why poll instead of a fixed sleep?
    The supervisor tick runs every 100ms and state transitions depend on
    process lifecycle events (spawning, crash detection) that happen at
    unpredictable times. Polling gives the fastest possible test while
    tolerating slow CI machines.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        status = get_status(guardian, config)
        if service_is_in_state(status, service, state):
            return True
        time.sleep(0.2)
    return False


def stop_guardian(guardian, config, proc):
    """
    Send 'guardian stop', then wait for the background process to exit.

    We try the graceful IPC stop first. If the process hasn't exited
    within 8 seconds, we force-kill it to prevent test hangs.
    """
    try:
        subprocess.run(
            [guardian, "stop", config],
            capture_output=True,
            timeout=5,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        pass  # stop command failed — we'll force-kill below

    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def make_ipc_socket(directory, name):
    """
    Return an ipc_socket path for a config.

    On Linux: a Unix domain socket file path inside 'directory'.
    On Windows: guardian's to_pipe_path() extracts the basename and creates
                a Named Pipe. Each test uses a unique name (t1, t2, ...) to
                avoid Named Pipe conflicts between concurrent test runs.
    """
    return os.path.join(directory, f"{name}.sock").replace("\\", "/")


# ─── Tests ───────────────────────────────────────────────────────────────────


def test_autostart_reaches_running(guardian, helper):
    """
    A service with autostart=true and a non-crashing command should reach
    RUNNING state within a few seconds of guardian starting.

    This is the most fundamental test: does the supervisor actually start
    services and track them as alive?
    """
    tmpdir = tempfile.mkdtemp()
    proc = None
    try:
        sock = make_ipc_socket(tmpdir, "t1")
        log  = os.path.join(tmpdir, "t1.log").replace("\\", "/")
        cfg  = write_config(tmpdir, "t1.ini", f"""\
[supervisor]
ipc_socket = {sock}

[run-forever]
command     = {helper}
args        = run
autostart   = true
restart     = on_failure
stdout_log  = {log}
""")
        proc = start_guardian(guardian, cfg)

        TEST("autostart service reaches RUNNING within 5 seconds")
        ASSERT(
            wait_for_state(guardian, cfg, "run-forever", "RUNNING", timeout_s=5),
            "service did not reach RUNNING — check guardian start-up logs",
        )

    finally:
        if proc:
            stop_guardian(guardian, cfg, proc)
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_crash_triggers_backoff(guardian, helper):
    """
    A service that exits immediately (crash) with restart=on_failure should
    not stay in STOPPED — it should enter BACKOFF while guardian waits to
    retry.

    This validates the core crash-detection and restart-scheduling loop.
    """
    tmpdir = tempfile.mkdtemp()
    proc = None
    try:
        sock = make_ipc_socket(tmpdir, "t2")
        log  = os.path.join(tmpdir, "t2.log").replace("\\", "/")
        cfg  = write_config(tmpdir, "t2.ini", f"""\
[supervisor]
ipc_socket = {sock}

[crash-svc]
command         = {helper}
args            = crash
autostart       = true
restart         = on_failure
max_retries     = 0
backoff_base_ms = 300
backoff_max_ms  = 60000
stdout_log      = {log}
""")
        proc = start_guardian(guardian, cfg)

        #
        # Sequence of events:
        #   t=0       guardian starts, spawns crash-svc (STARTING)
        #   t~100ms   supervisor tick: process still alive → RUNNING
        #                              (or already dead → crash handling)
        #   t~200ms   process exits (code 1) → retry_count=1, BACKOFF
        #             next_restart = now + 300*2 = 600ms from crash
        #   t~800ms   BACKOFF delay expires → restart attempt
        #   ...        (loops forever because max_retries=0)
        #
        # We expect to see BACKOFF at some point within 4 seconds.
        # We also accept STARTING/RUNNING briefly between retries.
        #
        TEST("crash service enters BACKOFF within 4 seconds")
        ASSERT(
            wait_for_state(guardian, cfg, "crash-svc", "BACKOFF", timeout_s=4),
            "BACKOFF state never appeared — crash may not have been detected",
        )

    finally:
        if proc:
            stop_guardian(guardian, cfg, proc)
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_max_retries_leads_to_failed(guardian, helper):
    """
    A service that crashes every time should exhaust max_retries and
    transition permanently to FAILED.

    With max_retries=2 and base=200ms:
      Crash 1 → BACKOFF(400ms) → restart
      Crash 2 → BACKOFF(800ms) → restart
      Crash 3 → FAILED (retry_count 3 > max_retries 2)

    Total expected time to FAILED: ~1.2 seconds. We allow 6 seconds.

    This validates the give-up logic in handle_exited_service().
    """
    tmpdir = tempfile.mkdtemp()
    proc = None
    try:
        sock = make_ipc_socket(tmpdir, "t3")
        log  = os.path.join(tmpdir, "t3.log").replace("\\", "/")
        cfg  = write_config(tmpdir, "t3.ini", f"""\
[supervisor]
ipc_socket = {sock}

[give-up-svc]
command         = {helper}
args            = crash
autostart       = true
restart         = on_failure
max_retries     = 2
backoff_base_ms = 200
backoff_max_ms  = 10000
stdout_log      = {log}
""")
        proc = start_guardian(guardian, cfg)

        TEST("service reaches FAILED after exhausting max_retries=2")
        ASSERT(
            wait_for_state(guardian, cfg, "give-up-svc", "FAILED", timeout_s=6),
            "FAILED state never appeared — check max_retries and backoff logic",
        )

    finally:
        if proc:
            stop_guardian(guardian, cfg, proc)
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_restart_never_stays_stopped(guardian, helper):
    """
    A service with restart=never should go to STOPPED after crashing and
    stay there — guardian must NOT attempt to restart it.

    This validates the restart policy gate in handle_exited_service():
      if restart_policy == RESTART_NEVER: → STATE_STOPPED
    """
    tmpdir = tempfile.mkdtemp()
    proc = None
    try:
        sock = make_ipc_socket(tmpdir, "t4")
        log  = os.path.join(tmpdir, "t4.log").replace("\\", "/")
        cfg  = write_config(tmpdir, "t4.ini", f"""\
[supervisor]
ipc_socket = {sock}

[no-restart-svc]
command         = {helper}
args            = crash
autostart       = true
restart         = never
stdout_log      = {log}
""")
        proc = start_guardian(guardian, cfg)

        TEST("restart=never service reaches STOPPED after crash")
        ASSERT(
            wait_for_state(guardian, cfg, "no-restart-svc", "STOPPED", timeout_s=4),
            "STOPPED state never appeared for restart=never service",
        )

        # Verify it stays STOPPED — wait 1 second and check again.
        # If it had restarted, it would be in STARTING or RUNNING.
        time.sleep(1.0)

        TEST("restart=never service stays STOPPED (does not restart)")
        status = get_status(guardian, cfg)
        ASSERT(
            service_is_in_state(status, "no-restart-svc", "STOPPED"),
            f"service left STOPPED state unexpectedly. Status:\n{status}",
        )

    finally:
        if proc:
            stop_guardian(guardian, cfg, proc)
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_stop_command_exits_cleanly(guardian, helper):
    """
    'guardian stop' should cause the supervisor process to exit cleanly
    within a reasonable time.

    This validates the full IPC pipeline: client sends {"cmd":"stop"},
    supervisor receives it, sets ctx->running=0, drains health threads,
    stops all services, and exits.
    """
    tmpdir = tempfile.mkdtemp()
    proc = None
    try:
        sock = make_ipc_socket(tmpdir, "t5")
        log  = os.path.join(tmpdir, "t5.log").replace("\\", "/")
        cfg  = write_config(tmpdir, "t5.ini", f"""\
[supervisor]
ipc_socket = {sock}

[stable-svc]
command    = {helper}
args       = run
autostart  = true
restart    = on_failure
stdout_log = {log}
""")
        proc = start_guardian(guardian, cfg)

        # Wait for the service to be up before stopping, so we exercise
        # the full service-shutdown path (not just "stop before anything started")
        wait_for_state(guardian, cfg, "stable-svc", "RUNNING", timeout_s=5)

        TEST("guardian stop causes supervisor to exit within 10 seconds")
        subprocess.run(
            [guardian, "stop", cfg],
            capture_output=True,
            timeout=5,
        )

        try:
            proc.wait(timeout=10)
            exited = True
        except subprocess.TimeoutExpired:
            exited = False

        ASSERT(exited, "guardian process did not exit within 10s of stop command")
        proc = None  # don't double-stop in finally

    finally:
        if proc:
            proc.kill()
            proc.wait()
        shutil.rmtree(tmpdir, ignore_errors=True)


# ─── Entry point ─────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <guardian-binary> <helper-binary>",
              file=sys.stderr)
        sys.exit(1)

    guardian = sys.argv[1]
    helper   = sys.argv[2]

    # Resolve to absolute paths so tests can run from any working directory.
    guardian = os.path.abspath(guardian)
    helper   = os.path.abspath(helper)

    if not os.path.isfile(guardian):
        print(f"error: guardian binary not found: {guardian}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(helper):
        print(f"error: helper binary not found: {helper}", file=sys.stderr)
        sys.exit(1)

    print()
    print("  guardian — integration tests")
    print("  ================================")
    print(f"  guardian: {guardian}")
    print(f"  helper:   {helper}")
    print()

    test_autostart_reaches_running(guardian, helper)
    test_crash_triggers_backoff(guardian, helper)
    test_max_retries_leads_to_failed(guardian, helper)
    test_restart_never_stays_stopped(guardian, helper)
    test_stop_command_exits_cleanly(guardian, helper)

    ok = SUMMARY()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
