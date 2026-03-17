#!/usr/bin/env python3
"""
Status dashboard for autonomous firmware test loops.

Usage:
    # In a separate terminal, watch the status:
    watch -n 2 cat /tmp/g6_test_status.txt

    # Or run this script directly for a live view:
    python3 status_dashboard.py --watch

    # Claude calls update_status() from test_runner.py to post updates.

The status file at /tmp/g6_test_status.txt is updated by the test runner
and can be viewed from any terminal window.
"""

import os
import sys
import time
from datetime import datetime

STATUS_FILE = "/tmp/g6_test_status.txt"
LOG_FILE = "/tmp/g6_test_log.txt"

def update_status(phase: str, detail: str = "", progress: str = ""):
    """Write current status to the shared status file."""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = [
        f"=== G6 Panel Test Status ===",
        f"Updated: {now}",
        f"Phase:   {phase}",
    ]
    if detail:
        lines.append(f"Detail:  {detail}")
    if progress:
        lines.append(f"Progress: {progress}")
    lines.append("=" * 30)

    with open(STATUS_FILE, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    # Also append to log
    with open(LOG_FILE, 'a') as f:
        f.write(f"[{now}] {phase}: {detail} {progress}\n")


def clear_log():
    """Clear the log file at the start of a new test session."""
    with open(LOG_FILE, 'w') as f:
        f.write(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] === New test session ===\n")


def watch_status():
    """Live-watch the status file (like tail -f)."""
    print("Watching G6 test status (Ctrl+C to quit)...")
    print(f"Status: {STATUS_FILE}")
    print(f"Log:    {LOG_FILE}")
    print()
    last_content = ""
    while True:
        try:
            if os.path.exists(STATUS_FILE):
                with open(STATUS_FILE, 'r') as f:
                    content = f.read()
                if content != last_content:
                    os.system('clear')
                    print(content)
                    # Show last 10 log lines
                    if os.path.exists(LOG_FILE):
                        with open(LOG_FILE, 'r') as f:
                            log_lines = f.readlines()
                        if log_lines:
                            print("\n--- Recent Log ---")
                            for line in log_lines[-10:]:
                                print(line, end='')
                    last_content = content
            else:
                print("Waiting for test to start...")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\nStopped watching.")
            break


if __name__ == "__main__":
    if "--watch" in sys.argv:
        watch_status()
    else:
        print(f"Status file: {STATUS_FILE}")
        print(f"Log file:    {LOG_FILE}")
        print()
        print("To watch status from another terminal:")
        print(f"  watch -n 2 cat {STATUS_FILE}")
        print(f"  tail -f {LOG_FILE}")
        print()
        print("Or run:")
        print(f"  python3 {__file__} --watch")
