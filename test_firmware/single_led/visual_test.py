#!/usr/bin/env python3
"""
Visual verification tests for Stage 3a.
Runs each pattern slowly enough to see, pauses for confirmation.
Sends a newline to interrupt SCAN when user presses Enter.
"""
import serial
import time

PORT = "/dev/cu.usbmodem2101"
BAUD = 115200

def send_cmd(ser, cmd, timeout=5):
    """Send command, print response, wait for completion."""
    ser.reset_input_buffer()
    time.sleep(0.05)
    ser.write((cmd + "\n").encode())
    start = time.time()
    while (time.time() - start) < timeout:
        if ser.in_waiting:
            line = ser.readline().decode(errors='replace').rstrip()
            if line:
                print(f"  >> {line}")
            if "END ---" in line or "complete" in line.lower():
                break
            if "STOPPED" in line or "RUNNING" in line:
                break
            if "cycles)" in line or "ROWS=" in line or "PATTERN=" in line:
                break
        else:
            time.sleep(0.01)
    time.sleep(0.1)


def start_scan(ser, n_frames=1000000):
    """Start a long SCAN (interruptible). Don't wait for it to finish."""
    ser.reset_input_buffer()
    ser.write(f"SCAN {n_frames}\n".encode())
    # Wait for the "SCAN START" header to appear
    start = time.time()
    while (time.time() - start) < 3:
        if ser.in_waiting:
            line = ser.readline().decode(errors='replace').rstrip()
            if line:
                print(f"  >> {line}")
            if "SCAN START" in line:
                break
        else:
            time.sleep(0.01)


def stop_scan(ser):
    """Interrupt a running SCAN by sending a newline, then STOP."""
    # Send newline to trigger the serial interrupt check in scan loop
    ser.write(b"\n")
    time.sleep(0.3)
    # Drain any scan output
    while ser.in_waiting:
        line = ser.readline().decode(errors='replace').rstrip()
        if line:
            print(f"  >> {line}")
    # Send STOP to ensure clean state
    send_cmd(ser, "STOP", timeout=3)
    time.sleep(0.2)


def wait_for_user(msg):
    """Pause and wait for user to press Enter."""
    response = input(f"\n>>> {msg}\n>>> (Press Enter to continue, or type notes): ")
    return response


def main():
    print(f"Connecting to {PORT}...")
    ser = serial.Serial(PORT, BAUD, timeout=1)

    # Drain boot messages / heartbeats
    print("Waiting for board...")
    time.sleep(2)
    while ser.in_waiting:
        line = ser.readline().decode(errors='replace').rstrip()
        if line:
            print(f"  >> {line}")

    # Stop default pulsing
    send_cmd(ser, "STOP")
    time.sleep(0.3)

    # Set visible scan rate: 100us per row = 2ms per frame = 500 Hz
    send_cmd(ser, "ON 100")

    results = []
    print("\n" + "=" * 60)
    print("VISUAL VERIFICATION TESTS")
    print("Each test scans continuously until you press Enter.")
    print("=" * 60)

    # --- Test 1: All LEDs on ---
    print("\nTEST 1: ALL columns ON, all 20 rows scanning")
    print("Expected: Entire 20x20 display should be lit (uniformly dim)")
    send_cmd(ser, "ROWS 20")
    send_cmd(ser, "PATTERN ALL")
    start_scan(ser)
    r = wait_for_user("Is the entire display lit uniformly? Any dark rows/columns?")
    results.append(("Test 1: ALL on", r))
    stop_scan(ser)

    # --- Test 2: No LEDs ---
    print("\nTEST 2: NO columns ON, all 20 rows scanning")
    print("Expected: Display should be completely OFF")
    send_cmd(ser, "PATTERN NONE")
    start_scan(ser)
    r = wait_for_user("Is the display completely OFF?")
    results.append(("Test 2: NONE", r))
    stop_scan(ser)

    # --- Test 3: Top half only ---
    print("\nTEST 3: First 10 rows only, all columns")
    print("Expected: Half the display lit, other half dark")
    send_cmd(ser, "ROWS 10")
    send_cmd(ser, "PATTERN ALL")
    start_scan(ser)
    r = wait_for_user("Is half the display lit and half dark? Which half?")
    results.append(("Test 3: 10 rows", r))
    stop_scan(ser)

    # --- Test 4: Single row ---
    print("\nTEST 4: Single row (row 0), all columns")
    print("Expected: Only one row of 20 LEDs lit")
    send_cmd(ser, "ROWS 1")
    start_scan(ser)
    r = wait_for_user("Is only one row lit? Where on the panel?")
    results.append(("Test 4: 1 row", r))
    stop_scan(ser)

    # --- Test 5: Checkerboard columns ---
    print("\nTEST 5: Checkerboard columns (0xAAAAA), all 20 rows")
    print("Expected: Vertical stripes — every other column lit")
    send_cmd(ser, "ROWS 20")
    send_cmd(ser, "PATTERN AAAAA")
    start_scan(ser)
    r = wait_for_user("Do you see vertical stripes (alternating columns)?")
    results.append(("Test 5: CHECK", r))
    stop_scan(ser)

    # --- Test 6: Inverse checkerboard ---
    print("\nTEST 6: Inverse checkerboard (0x55555), all 20 rows")
    print("Expected: Opposite vertical stripes from Test 5")
    send_cmd(ser, "PATTERN 55555")
    start_scan(ser)
    r = wait_for_user("Are the stripes the OPPOSITE of Test 5?")
    results.append(("Test 6: INV CHECK", r))
    stop_scan(ser)

    # --- Test 7: Single column ---
    print("\nTEST 7: Single column (col 0 = 0x00001), all 20 rows")
    print("Expected: One vertical line of LEDs")
    send_cmd(ser, "PATTERN 00001")
    start_scan(ser)
    r = wait_for_user("Do you see a single vertical column? Where?")
    results.append(("Test 7: 1 col", r))
    stop_scan(ser)

    # --- Test 8: Brightness scaling ---
    print("\nTEST 8: Brightness vs ON time")
    print("Expected: Gets dimmer as ON time decreases")
    send_cmd(ser, "PATTERN ALL")
    send_cmd(ser, "ROWS 20")

    for on_time in ["200", "100", "50", "10", "5", "1"]:
        send_cmd(ser, f"ON {on_time}")
        start_scan(ser)
        r = wait_for_user(f"ON={on_time}us — How bright? (bright/medium/dim/barely/off)")
        results.append((f"Test 8: ON={on_time}us", r))
        stop_scan(ser)

    # --- Summary ---
    print("\n" + "=" * 60)
    print("VISUAL TESTS COMPLETE — Summary:")
    print("=" * 60)
    for name, note in results:
        print(f"  {name}: {note if note else '(no notes)'}")

    # Save results
    outfile = "/Users/reiserm/Documents/GitHub/G6_Panels_Test_Firmware/test_firmware/single_led/visual_test_results.txt"
    with open(outfile, 'w') as f:
        for name, note in results:
            f.write(f"{name}: {note if note else '(no notes)'}\n")
    print(f"\nResults saved to {outfile}")

    ser.close()


if __name__ == "__main__":
    main()
