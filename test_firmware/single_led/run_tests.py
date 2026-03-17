#!/usr/bin/env python3
"""
Automated test runner for Stage 3b firmware.
Compares CPU-based and PIO-based scanning.
Sends commands over serial, captures output.
"""
import serial
import time
import sys

PORT = "/dev/cu.usbmodem2101"
BAUD = 115200
OUTPUT_FILE = "/Users/reiserm/Documents/GitHub/G6_Panels_Test_Firmware/test_firmware/single_led/test_output.txt"

def send_cmd(ser, cmd, wait_for=None, timeout=120):
    """Send a command and capture output until done marker or timeout."""
    # Drain any pending input
    ser.reset_input_buffer()
    time.sleep(0.05)

    ser.write((cmd + "\n").encode())
    lines = []
    start = time.time()

    while (time.time() - start) < timeout:
        if ser.in_waiting:
            line = ser.readline().decode(errors='replace').rstrip()
            lines.append(line)
            print(line, flush=True)
            # Check for completion markers
            if wait_for and wait_for in line:
                break
            if "complete" in line.lower() or "END ---" in line:
                break
        else:
            time.sleep(0.01)

    return lines


def main():
    print(f"Connecting to {PORT}...")
    ser = serial.Serial(PORT, BAUD, timeout=1)

    # Wait for boot message
    print("Waiting for boot...")
    all_output = []
    boot_ok = False
    start = time.time()
    while (time.time() - start) < 10:
        if ser.in_waiting:
            line = ser.readline().decode(errors='replace').rstrip()
            all_output.append(line)
            print(line, flush=True)
            if "BOOT OK" in line:
                boot_ok = True
            if "Type HELP" in line:
                break
        else:
            time.sleep(0.05)

    if not boot_ok:
        print("WARNING: Did not see BOOT OK, continuing anyway...")

    time.sleep(0.5)

    # === Test sequence ===

    # 1. Stop default pulsing
    print("\n=== STOPPING DEFAULT PULSE ===")
    out = send_cmd(ser, "STOP", wait_for="STOPPED", timeout=5)
    all_output.extend(out)
    time.sleep(0.2)

    # ============================================================
    # PART A: CPU scan (Phase 3a baseline) — abbreviated
    # ============================================================
    print("\n" + "=" * 60)
    print("PART A: CPU-BASED SCAN (Phase 3a baseline)")
    print("=" * 60)

    # CPU ROWTIME
    print("\n=== CPU ROWTIME (20 rows, 10000 iterations) ===")
    send_cmd(ser, "ROWS 20", wait_for="ROWS=", timeout=5)
    send_cmd(ser, "PATTERN ALL", wait_for="columns", timeout=5)
    out = send_cmd(ser, "ROWTIME 10000", wait_for="ROWTIME END", timeout=60)
    all_output.extend(out)
    time.sleep(0.2)

    # CPU SCAN at key ON times
    print("\n=== CPU SCAN (20 rows, all columns) ===")
    for on_time in ["0.5", "1.0", "2.0", "5.0"]:
        print(f"\n--- CPU SCAN: ON={on_time}us, 10000 frames ---")
        send_cmd(ser, f"ON {on_time}", wait_for="cycles)", timeout=5)
        time.sleep(0.1)
        out = send_cmd(ser, "SCAN 10000", wait_for="SCAN END", timeout=120)
        all_output.extend(out)
        time.sleep(0.2)

    # ============================================================
    # PART B: PIO scan (Phase 3b) — full comparison
    # ============================================================
    print("\n" + "=" * 60)
    print("PART B: PIO-BASED SCAN (Phase 3b)")
    print("=" * 60)

    # PIO ROWTIME
    print("\n=== PIO ROWTIME (20 rows, 10000 iterations) ===")
    send_cmd(ser, "ROWS 20", wait_for="ROWS=", timeout=5)
    send_cmd(ser, "PATTERN ALL", wait_for="columns", timeout=5)
    out = send_cmd(ser, "PIOROWTIME 10000", wait_for="PIOROWTIME END", timeout=60)
    all_output.extend(out)
    time.sleep(0.2)

    # PIO ROWTIME with 1 row
    print("\n=== PIO ROWTIME (1 row, 10000 iterations) ===")
    send_cmd(ser, "ROWS 1", wait_for="ROWS=", timeout=5)
    out = send_cmd(ser, "PIOROWTIME 10000", wait_for="PIOROWTIME END", timeout=60)
    all_output.extend(out)
    time.sleep(0.2)

    # PIO SCAN at same ON times (protected mode)
    print("\n=== PIO SCAN - PROTECTED (20 rows, all columns) ===")
    send_cmd(ser, "ROWS 20", wait_for="ROWS=", timeout=5)
    send_cmd(ser, "PATTERN ALL", wait_for="columns", timeout=5)
    for on_time in ["0.5", "1.0", "2.0", "5.0"]:
        print(f"\n--- PIOSCAN: ON={on_time}us, 10000 frames ---")
        send_cmd(ser, f"ON {on_time}", wait_for="cycles)", timeout=5)
        time.sleep(0.1)
        out = send_cmd(ser, "PIOSCAN 10000", wait_for="PIOSCAN END", timeout=120)
        all_output.extend(out)
        time.sleep(0.2)

    # PIO SCAN unprotected (no noInterrupts) — demonstrates PIO independence
    print("\n=== PIO SCAN - UNPROTECTED (20 rows, ON=1.0us) ===")
    send_cmd(ser, "ON 1.0", wait_for="cycles)", timeout=5)
    out = send_cmd(ser, "PIOSCAN2 10000", wait_for="PIOSCAN END", timeout=120)
    all_output.extend(out)
    time.sleep(0.2)

    # PIO row scaling
    print("\n=== PIO ROW SCALING (ON=1.0us, 10000 frames) ===")
    send_cmd(ser, "PATTERN ALL", wait_for="columns", timeout=5)
    send_cmd(ser, "ON 1.0", wait_for="cycles)", timeout=5)
    for n_rows in [1, 5, 10, 15, 20]:
        print(f"\n--- PIOSCAN: {n_rows} rows ---")
        send_cmd(ser, f"ROWS {n_rows}", wait_for="ROWS=", timeout=5)
        time.sleep(0.1)
        out = send_cmd(ser, "PIOSCAN 10000", wait_for="PIOSCAN END", timeout=120)
        all_output.extend(out)
        time.sleep(0.2)

    # PIO pattern independence
    print("\n=== PIO PATTERN TESTS (ON=1.0us, 20 rows) ===")
    send_cmd(ser, "ROWS 20", wait_for="ROWS=", timeout=5)
    send_cmd(ser, "ON 1.0", wait_for="cycles)", timeout=5)
    for pattern in ["ALL", "NONE", "CHECK", "00001"]:
        print(f"\n--- PIOSCAN: pattern={pattern}, 10000 frames ---")
        send_cmd(ser, f"PATTERN {pattern}", wait_for="columns", timeout=5)
        time.sleep(0.1)
        out = send_cmd(ser, "PIOSCAN 10000", wait_for="PIOSCAN END", timeout=120)
        all_output.extend(out)
        time.sleep(0.2)

    # Save all output
    with open(OUTPUT_FILE, 'w') as f:
        f.write('\n'.join(all_output))
    print(f"\n=== ALL TESTS COMPLETE ===")
    print(f"Output saved to {OUTPUT_FILE}")

    ser.close()


if __name__ == "__main__":
    main()
