#!/usr/bin/env python3
"""
Autonomous firmware test runner for G6 Panel timing characterization.

Handles the full loop:
  1. Build firmware
  2. Flash via REBOOT command (or manual BOOTSEL)
  3. Run serial commands and collect measurements
  4. Report results

Designed to be called by Claude or run standalone.
Writes status to /tmp/g6_test_status.txt for visibility.

Usage:
    # Run burst-mode jitter sweep:
    python3 auto_test.py burst_sweep

    # Run a single command:
    python3 auto_test.py cmd "BURST 10000"

    # Build and flash:
    python3 auto_test.py flash

    # Full pipeline: build, flash, run burst sweep:
    python3 auto_test.py full
"""

import serial
import serial.tools.list_ports
import subprocess
import sys
import os
import time
import json
from datetime import datetime

# Import status dashboard
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from status_dashboard import update_status, clear_log

# Configuration
PROJECT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE_UF2 = os.path.join(PROJECT_DIR, ".pio/build/pico/firmware.uf2")
PIO_BIN = os.path.expanduser("~/.platformio/penv/bin/pio")
BAUD = 115200
SERIAL_TIMEOUT = 2
RESULTS_FILE = os.path.join(PROJECT_DIR, "burst_results.json")

# ── Serial helpers ──────────────────────────────────────────────────────────

def find_serial_port():
    """Find the RP2350 serial port."""
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device:
            # Skip non-CDC ports (like the debug probe)
            if "306NTVSCY" not in port.device:
                return port.device
    # Fallback: return any usbmodem
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device:
            return port.device
    return None


def wait_for_serial(timeout=15):
    """Wait for serial port to appear after reboot."""
    update_status("Waiting for serial", f"timeout={timeout}s")
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_serial_port()
        if port:
            time.sleep(1)  # Let USB settle
            update_status("Serial found", port)
            return port
        time.sleep(0.5)
    update_status("ERROR", "Serial port not found")
    return None


def open_serial(port=None, retries=3):
    """Open serial connection with retries."""
    for attempt in range(retries):
        if port is None:
            port = find_serial_port()
        if port is None:
            update_status("Waiting for serial", f"attempt {attempt+1}/{retries}")
            time.sleep(2)
            continue
        try:
            ser = serial.Serial(port, BAUD, timeout=SERIAL_TIMEOUT)
            time.sleep(1)
            ser.reset_input_buffer()
            update_status("Serial connected", port)
            return ser
        except Exception as e:
            update_status("Serial error", f"{e}, retry {attempt+1}/{retries}")
            port = None
            time.sleep(2)
    return None


def send_command(ser, cmd, timeout=30, wait_for=None):
    """Send command and collect response. Returns full output string."""
    ser.reset_input_buffer()
    ser.write((cmd.strip() + '\r\n').encode())
    update_status("Command sent", cmd, "waiting for response...")

    output = ''
    deadline = time.time() + timeout
    last_data_time = time.time()

    while time.time() < deadline:
        try:
            n = ser.in_waiting
            if n > 0:
                chunk = ser.read(n).decode(errors='replace')
                output += chunk
                last_data_time = time.time()

                # Check for completion markers
                if wait_for and wait_for in output:
                    break
                # Auto-detect common end markers
                for marker in ['END ---', 'DONE ===', 'DONE']:
                    if marker in output and output.rstrip().endswith(marker):
                        time.sleep(0.2)  # Drain any trailing data
                        output += ser.read(ser.in_waiting).decode(errors='replace')
                        break
            else:
                time.sleep(0.05)

            # If we have data and no new data for 2 seconds, assume done
            if output and (time.time() - last_data_time > 2.0):
                break

        except (OSError, serial.SerialException) as e:
            update_status("Serial ERROR", str(e))
            return None

    return output


# ── Build & Flash ───────────────────────────────────────────────────────────

def build():
    """Build firmware. Returns True on success."""
    update_status("Building", "pio run")
    result = subprocess.run(
        [PIO_BIN, "run", "-d", PROJECT_DIR],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode == 0:
        update_status("Build OK", "firmware ready")
        return True
    else:
        update_status("BUILD FAILED", result.stderr[-200:])
        print(f"Build failed:\n{result.stderr}", file=sys.stderr)
        return False


def flash_via_reboot(ser):
    """Flash by sending REBOOT command, then copying UF2."""
    update_status("Flashing", "sending REBOOT command")
    try:
        ser.write(b'REBOOT\r\n')
        time.sleep(0.5)
    except:
        pass
    try:
        ser.close()
    except:
        pass

    # Wait for RP2350 volume to appear
    update_status("Flashing", "waiting for BOOTSEL volume")
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists("/Volumes/RP2350"):
            break
        time.sleep(0.5)

    if not os.path.exists("/Volumes/RP2350"):
        update_status("FLASH FAILED", "RP2350 volume not found")
        return False

    update_status("Flashing", "copying UF2")
    try:
        subprocess.run(
            ["cp", "-X", FIRMWARE_UF2, "/Volumes/RP2350/"],
            timeout=15, check=True
        )
    except Exception as e:
        update_status("FLASH FAILED", str(e))
        return False

    # Wait for reboot
    update_status("Flashing", "waiting for reboot")
    time.sleep(3)
    deadline = time.time() + 10
    while time.time() < deadline:
        if not os.path.exists("/Volumes/RP2350"):
            break
        time.sleep(0.5)

    update_status("Flash OK", "device rebooting")
    return True


def flash_via_bootsel():
    """Flash assuming device is already in BOOTSEL mode."""
    if not os.path.exists("/Volumes/RP2350"):
        update_status("FLASH FAILED", "RP2350 volume not mounted. Hold BOOTSEL + tap RESET.")
        return False

    update_status("Flashing", "copying UF2 (manual BOOTSEL)")
    try:
        subprocess.run(
            ["cp", "-X", FIRMWARE_UF2, "/Volumes/RP2350/"],
            timeout=15, check=True
        )
    except Exception as e:
        update_status("FLASH FAILED", str(e))
        return False

    time.sleep(3)
    update_status("Flash OK", "device rebooting")
    return True


# ── Test routines ───────────────────────────────────────────────────────────

def run_burst_sweep(ser):
    """Run burst-mode jitter sweep at application-relevant ON times.

    Returns dict of results.
    """
    on_times = [0.25, 0.5, 0.75, 1.0]
    n_triggers = 10000
    results = {}

    # Setup: 20 rows, all-on pattern
    send_command(ser, "ROWS 20", timeout=3)
    send_command(ser, "PATTERN FFFFF", timeout=3)

    for i, on_t in enumerate(on_times):
        update_status("Burst sweep",
                      f"ON={on_t}us ({i+1}/{len(on_times)})",
                      f"{i}/{len(on_times)} complete")

        # Set ON time
        send_command(ser, f"ON {on_t}", timeout=3)
        time.sleep(0.3)

        # Run burst test
        out = send_command(ser, f"BURST {n_triggers}", timeout=60,
                           wait_for="BURST END")
        if out is None:
            update_status("ERROR", f"Serial lost during BURST ON={on_t}")
            results[on_t] = {"error": "serial disconnected"}
            break

        # Parse output
        parsed = parse_burst_output(out)
        parsed["raw"] = out
        results[on_t] = parsed

        # Print summary
        if "jitter_us" in parsed:
            print(f"  ON={on_t}us: frame={parsed.get('frame_mean', '?')}us "
                  f"jitter={parsed['jitter_us']}us "
                  f"fits_15us={parsed.get('fits_15us', '?')}")
        else:
            print(f"  ON={on_t}us: PARSE ERROR")
            print(f"  Raw output: {out[:300]}")

    update_status("Burst sweep complete",
                  f"{len(results)} ON times tested",
                  f"{len(on_times)}/{len(on_times)} complete")

    # Save results
    with open(RESULTS_FILE, 'w') as f:
        # Strip raw output for JSON (can be large)
        save_results = {}
        for k, v in results.items():
            save_results[str(k)] = {kk: vv for kk, vv in v.items() if kk != "raw"}
        json.dump(save_results, f, indent=2)

    return results


def parse_burst_output(output):
    """Parse BURST command output into structured data."""
    result = {}
    for line in output.split('\n'):
        line = line.strip()
        if 'frame: min=' in line.lower() or 'frame:' in line.lower():
            # Parse: frame: min=X.XXXus  max=X.XXXus  mean=X.XXXus
            parts = line.split()
            for p in parts:
                if p.startswith('min='):
                    result['frame_min'] = p.replace('min=', '').replace('us', '')
                elif p.startswith('max='):
                    result['frame_max'] = p.replace('max=', '').replace('us', '')
                elif p.startswith('mean='):
                    result['frame_mean'] = p.replace('mean=', '').replace('us', '')
        elif 'Jitter' in line and 'max-min' in line:
            # Parse: Jitter (max-min): X.XXXus
            parts = line.split(':')
            if len(parts) >= 2:
                result['jitter_us'] = parts[-1].strip().replace('us', '')
        elif 'Fits in 15us' in line:
            result['fits_15us'] = 'YES' in line
        elif 'Duty cycle' in line:
            parts = line.split(':')
            if len(parts) >= 2:
                result['duty_cycle'] = parts[-1].strip().replace('%', '')
        elif 'row_avg' in line.lower():
            parts = line.split()
            for p in parts:
                if p.startswith('min='):
                    result['row_min'] = p.replace('min=', '').replace('us', '')
                elif p.startswith('mean='):
                    result['row_mean'] = p.replace('mean=', '').replace('us', '')
    return result


def run_comparison_sweep(ser):
    """Compare PIOSCAN (continuous) vs BURST at same ON times."""
    on_times = [0.25, 0.5, 1.0]
    n = 10000
    results = {"pioscan": {}, "burst": {}}

    send_command(ser, "ROWS 20", timeout=3)
    send_command(ser, "PATTERN FFFFF", timeout=3)

    for i, on_t in enumerate(on_times):
        send_command(ser, f"ON {on_t}", timeout=3)
        time.sleep(0.3)

        # PIOSCAN (continuous)
        update_status("Comparison sweep", f"PIOSCAN ON={on_t}us", f"{2*i}/{2*len(on_times)}")
        out = send_command(ser, f"PIOSCAN {n}", timeout=60, wait_for="PIOSCAN END")
        results["pioscan"][on_t] = parse_burst_output(out or "") if out else {"error": "failed"}

        time.sleep(0.5)

        # BURST (8 kHz trigger)
        update_status("Comparison sweep", f"BURST ON={on_t}us", f"{2*i+1}/{2*len(on_times)}")
        out = send_command(ser, f"BURST {n}", timeout=60, wait_for="BURST END")
        results["burst"][on_t] = parse_burst_output(out or "") if out else {"error": "failed"}

    update_status("Comparison complete", f"{len(on_times)} ON times x 2 modes")
    return results


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 auto_test.py <command> [args]")
        print("Commands:")
        print("  flash              Build and flash firmware")
        print("  cmd <serial_cmd>   Send a single serial command")
        print("  burst_sweep        Run burst-mode jitter sweep")
        print("  comparison         Compare PIOSCAN vs BURST")
        print("  full               Build, flash, run burst sweep")
        return

    clear_log()
    action = sys.argv[1]

    if action == "flash":
        if not build():
            return
        # Try REBOOT first, fall back to manual BOOTSEL
        ser = open_serial()
        if ser:
            flash_via_reboot(ser)
        else:
            flash_via_bootsel()

    elif action == "cmd":
        cmd = ' '.join(sys.argv[2:])
        ser = open_serial()
        if not ser:
            print("ERROR: No serial connection")
            return
        out = send_command(ser, cmd, timeout=60)
        print(out)
        ser.close()

    elif action == "burst_sweep":
        ser = open_serial()
        if not ser:
            print("ERROR: No serial connection")
            return
        results = run_burst_sweep(ser)
        ser.close()
        print("\n=== BURST SWEEP RESULTS ===")
        print(json.dumps({str(k): {kk: vv for kk, vv in v.items() if kk != "raw"}
                          for k, v in results.items()}, indent=2))

    elif action == "comparison":
        ser = open_serial()
        if not ser:
            print("ERROR: No serial connection")
            return
        results = run_comparison_sweep(ser)
        ser.close()
        print("\n=== COMPARISON RESULTS ===")
        print(json.dumps(results, indent=2))

    elif action == "full":
        # Full pipeline: build → flash → test
        if not build():
            return

        ser = open_serial()
        if ser:
            if not flash_via_reboot(ser):
                print("Flash failed. Put board in BOOTSEL mode manually.")
                return
        elif os.path.exists("/Volumes/RP2350"):
            if not flash_via_bootsel():
                return
        else:
            print("ERROR: No serial port and no BOOTSEL volume.")
            print("Connect the board or hold BOOTSEL + tap RESET.")
            return

        # Wait for serial after flash
        port = wait_for_serial(timeout=15)
        if not port:
            print("ERROR: Serial not available after flash")
            return

        ser = open_serial(port)
        if not ser:
            print("ERROR: Could not open serial after flash")
            return

        results = run_burst_sweep(ser)
        ser.close()

        print("\n=== FULL PIPELINE COMPLETE ===")
        print(json.dumps({str(k): {kk: vv for kk, vv in v.items() if kk != "raw"}
                          for k, v in results.items()}, indent=2))
    else:
        print(f"Unknown action: {action}")


if __name__ == "__main__":
    main()
