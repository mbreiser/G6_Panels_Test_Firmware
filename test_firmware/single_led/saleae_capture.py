#!/usr/bin/env python3
"""
Saleae Logic 2 automation for G6 panel timing + photodiode characterization.

Requires:
  - pip install logic2-automation
  - Logic 2 desktop app running with automation server enabled (port 10430)
  - Saleae Logic Pro 8 connected

Channel setup:
  Ch 0 (Digital): GP45 — external trigger input (8 kHz from function generator)
  Ch 1 (Analog):  Photodiode output — LED brightness measurement

Usage:
    # Capture during PHOTOCAL (16 intensity levels):
    python3 saleae_capture.py photocal [hold_sec]

    # Capture during BCMBURST:
    python3 saleae_capture.py bcmburst [n_triggers]

    # Manual capture for a fixed duration:
    python3 saleae_capture.py capture [duration_sec]

    # Analyze a previously exported CSV:
    python3 saleae_capture.py analyze <csv_path>
"""

import sys
import os
import time
import json
import serial
import serial.tools.list_ports

# ── Serial helpers (shared with auto_test.py) ─────────────────────────────

BAUD = 115200

def find_serial_port():
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device and "306NTVSCY" not in p.device:
            return p.device
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device:
            return p.device
    return None

def open_serial():
    port = find_serial_port()
    if not port:
        print("ERROR: No serial port found")
        return None
    ser = serial.Serial(port, BAUD, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()
    print(f"Serial: {port}")
    return ser

def send_cmd(ser, cmd, timeout=30, wait_for=None):
    ser.reset_input_buffer()
    ser.write((cmd.strip() + '\r\n').encode())
    output = ''
    deadline = time.time() + timeout
    last_data = time.time()
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            output += ser.read(n).decode(errors='replace')
            last_data = time.time()
            if wait_for and wait_for in output:
                time.sleep(0.1)
                output += ser.read(ser.in_waiting).decode(errors='replace')
                return output
        else:
            time.sleep(0.02)
        if output and (time.time() - last_data > 2.0):
            return output
    return output

# ── Saleae Logic 2 automation ─────────────────────────────────────────────

def connect_saleae(port=10430):
    """Connect to Logic 2 automation server."""
    try:
        from saleae import automation
        manager = automation.Manager.connect(address="127.0.0.1", port=port)
        print(f"Saleae: Connected to Logic 2 automation (port {port})")
        return manager
    except Exception as e:
        print(f"ERROR: Could not connect to Logic 2 automation: {e}")
        print("Make sure Logic 2 is running with automation enabled:")
        print("  Preferences → Enable Scripting API → port 10430")
        return None

def start_capture(manager, duration_sec=5.0, digital_channels=[0], analog_channels=[1],
                  digital_rate=25_000_000, analog_rate=6_250_000):
    """Start a timed capture. Returns capture object."""
    from saleae import automation

    device_config = automation.LogicDeviceConfiguration(
        enabled_digital_channels=digital_channels,
        enabled_analog_channels=analog_channels,
        digital_sample_rate=digital_rate,
        analog_sample_rate=analog_rate,
    )

    capture_config = automation.CaptureConfiguration(
        capture_mode=automation.TimedCaptureMode(duration_seconds=duration_sec),
    )

    print(f"Saleae: Starting capture ({duration_sec}s, "
          f"digital={digital_channels}@{digital_rate/1e6:.0f}MHz, "
          f"analog={analog_channels}@{analog_rate/1e6:.1f}MHz)")

    capture = manager.start_capture(
        device_configuration=device_config,
        capture_configuration=capture_config,
    )
    return capture

def export_capture(capture, output_dir):
    """Export capture data to CSV files."""
    from saleae import automation

    os.makedirs(output_dir, exist_ok=True)

    # Export raw analog data
    try:
        capture.export_raw_data_csv(
            directory=output_dir,
            analog_channels=[1],
        )
        print(f"Saleae: Exported analog data to {output_dir}/")
    except Exception as e:
        print(f"Warning: Analog export failed: {e}")

    # Export digital data
    try:
        capture.export_raw_data_csv(
            directory=output_dir,
            digital_channels=[0],
        )
        print(f"Saleae: Exported digital data to {output_dir}/")
    except Exception as e:
        print(f"Warning: Digital export failed: {e}")

    return output_dir

# ── Test workflows ────────────────────────────────────────────────────────

def run_photocal_capture(hold_sec=3, manager=None):
    """Run PHOTOCAL while capturing Saleae data."""
    ser = open_serial()
    if not ser:
        return

    # Calculate capture duration: 16 levels × hold_sec + margin
    capture_duration = 16 * hold_sec + 10

    # Setup firmware
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'FILL 15', 'EXTTRIG ON']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.3)

    # Start Saleae capture if available
    # Logic Pro 8 valid rate pairs: digital=500MHz/analog=12.5MHz is good for long captures
    capture = None
    if manager:
        capture = start_capture(manager, duration_sec=capture_duration,
                                digital_channels=[0], analog_channels=[1],
                                digital_rate=500_000_000,  # 500 MHz digital
                                analog_rate=12_500_000)    # 12.5 MHz analog
    else:
        print(f"No Saleae automation. Start manual capture now ({capture_duration}s).")
        input("Press Enter when capture is running...")

    # Run PHOTOCAL
    time.sleep(1)
    ser.reset_input_buffer()
    print(f"\nRunning PHOTOCAL {hold_sec} ({16 * hold_sec}s)...")
    ser.write(f'PHOTOCAL {hold_sec}\r\n'.encode())

    # Collect serial output
    out = ''
    deadline = time.time() + capture_duration + 10
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            chunk = ser.read(n).decode(errors='replace')
            out += chunk
            for line in chunk.split('\n'):
                line = line.strip()
                if line and (',' in line or 'PHOTOCAL' in line):
                    print(f"  {line}")
            if 'PHOTOCAL END' in out:
                break
        else:
            time.sleep(0.1)

    # Wait for capture to finish and export
    if capture:
        print("\nWaiting for Saleae capture to complete...")
        capture.wait()
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "saleae_captures", f"photocal_{timestamp}")
        export_capture(capture, output_dir)

        # Save serial output alongside
        with open(os.path.join(output_dir, "serial_output.txt"), 'w') as f:
            f.write(out)
        print(f"Serial output saved to {output_dir}/serial_output.txt")

        # Parse and save structured results
        results = parse_photocal_output(out)
        if results:
            with open(os.path.join(output_dir, "timing_results.json"), 'w') as f:
                json.dump(results, f, indent=2)
            print(f"Timing results saved to {output_dir}/timing_results.json")
            print_photocal_summary(results)

    ser.close()
    return out

def run_bcmburst_capture(n_triggers=10000, manager=None):
    """Run BCMBURST while capturing Saleae data."""
    ser = open_serial()
    if not ser:
        return

    capture_duration = n_triggers / 8000 + 5  # triggers at 8kHz + margin

    # Setup
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'FILL 15', 'EXTTRIG ON']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.3)

    # Start Saleae capture (50 MHz analog for short burst — timing precision)
    capture = None
    if manager:
        capture = start_capture(manager, duration_sec=capture_duration,
                                digital_channels=[0], analog_channels=[1],
                                digital_rate=500_000_000,  # 500 MHz digital
                                analog_rate=50_000_000)    # 50 MHz analog
    else:
        print(f"No Saleae automation. Start manual capture now ({capture_duration:.0f}s).")
        input("Press Enter when capture is running...")

    time.sleep(1)
    ser.reset_input_buffer()
    print(f"\nRunning BCMBURST {n_triggers} 8000 A...")
    ser.write(f'BCMBURST {n_triggers} 8000 A\r\n'.encode())

    out = ''
    deadline = time.time() + capture_duration + 10
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            out += ser.read(n).decode(errors='replace')
            if 'BCMBURST END' in out:
                break
        else:
            time.sleep(0.05)

    print(out)

    if capture:
        print("\nWaiting for Saleae capture to complete...")
        capture.wait()
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "saleae_captures", f"bcmburst_{timestamp}")
        export_capture(capture, output_dir)
        with open(os.path.join(output_dir, "serial_output.txt"), 'w') as f:
            f.write(out)

    ser.close()
    return out

def run_timing_capture(manager=None):
    """Test 1: LED pulse timing relative to trigger.
    Short capture (1s), high analog rate (50 MHz) to resolve rise/fall times.
    Runs BCMBURST at full intensity so photodiode sees max signal.
    """
    ser = open_serial()
    if not ser:
        return

    # Setup: full intensity, external trigger
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'FILL 15', 'EXTTRIG ON']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.3)

    if not manager:
        print("ERROR: Saleae automation required for timing capture")
        ser.close()
        return

    # Start high-rate capture (1 second = ~8000 trigger cycles)
    # Logic Pro 8 valid pair: digital=500MHz, analog=50MHz
    capture = start_capture(manager, duration_sec=1.5,
                            digital_channels=[0], analog_channels=[1],
                            digital_rate=500_000_000,  # 500 MHz digital
                            analog_rate=50_000_000)    # 50 MHz analog (20ns resolution)

    # Run BCMBURST during capture
    time.sleep(0.3)
    ser.reset_input_buffer()
    print("Running BCMBURST 10000 8000 A (capturing 1.5s window)...")
    ser.write(b'BCMBURST 10000 8000 A\r\n')

    # Wait for capture to finish (1.5s)
    capture.wait()
    print("Capture complete.")

    # Export
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "saleae_captures", f"timing_{timestamp}")
    export_capture(capture, output_dir)

    # Collect remaining serial output
    time.sleep(2)
    out = ser.read(ser.in_waiting).decode(errors='replace')
    # Wait for BCMBURST to finish
    deadline = time.time() + 10
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            out += ser.read(n).decode(errors='replace')
            if 'BCMBURST END' in out:
                break
        else:
            time.sleep(0.1)
    print(out)

    with open(os.path.join(output_dir, "serial_output.txt"), 'w') as f:
        f.write(out)

    print(f"\nTiming data in: {output_dir}/")
    print(f"  analog.csv: 50 MHz photodiode — zoom to 1 trigger period (125µs)")
    print(f"  digital.csv: 50 MHz trigger edges")
    print(f"  Analyze: average many pulses to get clean rise/fall profile")
    ser.close()

def run_linearity_capture(hold_sec=3, manager=None):
    """Test 2: Intensity linearity across all 16 BCM levels.
    Long capture, low analog rate (625 kHz) to keep file small.
    PHOTOCAL cycles through levels with 0.5s OFF gaps for easy segmentation.
    """
    ser = open_serial()
    if not ser:
        return

    # Calculate total duration: 16 levels × (hold_sec ON + 0.5s OFF gap) + margin
    capture_duration = 16 * (hold_sec + 0.5) + 5

    # Setup
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'EXTTRIG ON']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.3)

    if not manager:
        print(f"No Saleae automation. Start manual capture now ({capture_duration:.0f}s).")
        input("Press Enter when capture is running...")
        capture = None
    else:
        # Low rate for small file. Valid pair: digital=6.25MHz, analog=781.25kHz
        capture = start_capture(manager, duration_sec=capture_duration,
                                digital_channels=[0], analog_channels=[1],
                                digital_rate=6_250_000,    # 6.25 MHz trigger edges
                                analog_rate=781_250)       # 781 kHz photodiode (~50 MB for 60s)

    # Run PHOTOCAL
    time.sleep(0.5)
    ser.reset_input_buffer()
    print(f"\nRunning PHOTOCAL {hold_sec} (16 levels × {hold_sec}s + gaps ≈ {capture_duration:.0f}s)...")
    ser.write(f'PHOTOCAL {hold_sec}\r\n'.encode())

    # Collect serial output with live progress
    out = ''
    deadline = time.time() + capture_duration + 15
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            chunk = ser.read(n).decode(errors='replace')
            out += chunk
            for line in chunk.split('\n'):
                line = line.strip()
                if line and (',' in line or 'PHOTOCAL' in line or 'BCM' in line):
                    print(f"  {line}")
            if 'PHOTOCAL END' in out:
                break
        else:
            time.sleep(0.1)

    # Wait for Saleae and export
    if capture:
        print("\nWaiting for Saleae capture to complete...")
        capture.wait()
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "saleae_captures", f"linearity_{timestamp}")
        export_capture(capture, output_dir)

        with open(os.path.join(output_dir, "serial_output.txt"), 'w') as f:
            f.write(out)

        results = parse_photocal_output(out)
        if results:
            with open(os.path.join(output_dir, "timing_results.json"), 'w') as f:
                json.dump(results, f, indent=2)
            print_photocal_summary(results)

        print(f"\nLinearity data in: {output_dir}/")
        print(f"  analog.csv: ~{capture_duration * 625000 * 10 / 1e6:.0f} MB photodiode voltage")
        print(f"  Look for 16 voltage plateaus with dips between them")
    else:
        results = parse_photocal_output(out)
        if results:
            print_photocal_summary(results)

    ser.close()

def run_simple_capture(duration_sec=5.0, manager=None):
    """Just capture Saleae data for a fixed duration (no firmware commands)."""
    if not manager:
        print("ERROR: Saleae automation required for simple capture")
        return

    capture = start_capture(manager, duration_sec=duration_sec,
                            digital_channels=[0], analog_channels=[1],
                            digital_rate=500_000_000,
                            analog_rate=50_000_000)

    print(f"Capturing for {duration_sec}s...")
    capture.wait()

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "saleae_captures", f"capture_{timestamp}")
    export_capture(capture, output_dir)
    print("Done!")

# ── Analysis ──────────────────────────────────────────────────────────────

def parse_photocal_output(output):
    """Parse PHOTOCAL serial output into structured data."""
    results = []
    for line in output.split('\n'):
        line = line.strip()
        parts = line.split(',')
        if len(parts) == 7:
            try:
                level = int(parts[0])
                results.append({
                    'level': level,
                    'burst_min_us': float(parts[1]),
                    'burst_max_us': float(parts[2]),
                    'burst_mean_us': float(parts[3]),
                    'jitter_us': float(parts[4]),
                    'outliers': int(parts[5]),
                    'triggers': int(parts[6]),
                })
            except (ValueError, IndexError):
                continue
    return results

def print_photocal_summary(results):
    """Print a nice summary table of PHOTOCAL results."""
    print("\n=== PHOTOCAL SUMMARY ===")
    print(f"{'Level':>5} {'Burst(µs)':>10} {'Jitter(µs)':>11} {'Outliers':>8} {'Triggers':>8}")
    print("-" * 48)
    for r in results:
        print(f"{r['level']:>5} {r['burst_mean_us']:>10.3f} {r['jitter_us']:>11.3f} "
              f"{r['outliers']:>8} {r['triggers']:>8}")

    total_triggers = sum(r['triggers'] for r in results)
    total_outliers = sum(r['outliers'] for r in results)
    max_jitter = max(r['jitter_us'] for r in results)
    print("-" * 48)
    print(f"Total triggers: {total_triggers}")
    print(f"Total outliers: {total_outliers}")
    print(f"Max jitter: {max_jitter:.3f} µs")

def analyze_analog_csv(csv_path):
    """Analyze exported Saleae analog CSV for photodiode linearity."""
    import csv
    import statistics

    print(f"Analyzing: {csv_path}")

    times = []
    voltages = []
    with open(csv_path, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            if len(row) >= 2:
                try:
                    times.append(float(row[0]))
                    voltages.append(float(row[1]))
                except ValueError:
                    continue

    if not voltages:
        print("No data found in CSV")
        return

    print(f"  Samples: {len(voltages)}")
    print(f"  Duration: {times[-1] - times[0]:.2f}s")
    print(f"  Voltage range: {min(voltages):.4f}V — {max(voltages):.4f}V")

    # Segment by time into 16 levels (if PHOTOCAL was running)
    total_duration = times[-1] - times[0]
    n_levels = 16
    level_duration = total_duration / n_levels

    print(f"\n{'Level':>5} {'Mean V':>8} {'Std V':>8} {'Samples':>8}")
    print("-" * 34)

    level_means = []
    for level in range(n_levels):
        t_start = times[0] + level * level_duration
        t_end = t_start + level_duration
        level_v = [v for t, v in zip(times, voltages) if t_start <= t < t_end]
        if level_v:
            mean_v = statistics.mean(level_v)
            std_v = statistics.stdev(level_v) if len(level_v) > 1 else 0
            level_means.append(mean_v)
            print(f"{level:>5} {mean_v:>8.4f} {std_v:>8.4f} {len(level_v):>8}")

    if len(level_means) >= 2:
        print(f"\nLinearity check:")
        print(f"  Level 0 voltage:  {level_means[0]:.4f}V")
        print(f"  Level 15 voltage: {level_means[-1]:.4f}V")
        print(f"  Range: {level_means[-1] - level_means[0]:.4f}V")
        monotonic = all(level_means[i] <= level_means[i+1] for i in range(len(level_means)-1))
        print(f"  Monotonic increasing: {'YES' if monotonic else 'NO'}")

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 saleae_capture.py timing               — LED pulse timing (1.5s, 50MHz)")
        print("  python3 saleae_capture.py linearity [hold_sec]  — intensity linearity (16 levels)")
        print("  python3 saleae_capture.py photocal [hold_sec]   — capture + PHOTOCAL")
        print("  python3 saleae_capture.py bcmburst [n_triggers]  — capture + BCMBURST")
        print("  python3 saleae_capture.py capture [duration_sec] — simple capture")
        print("  python3 saleae_capture.py analyze <csv_path>     — analyze exported CSV")
        return

    action = sys.argv[1].lower()

    if action == "analyze":
        if len(sys.argv) < 3:
            print("Usage: python3 saleae_capture.py analyze <csv_path>")
            return
        analyze_analog_csv(sys.argv[2])
        return

    # Try connecting to Saleae
    manager = connect_saleae()

    if action == "timing":
        run_timing_capture(manager=manager)

    elif action == "linearity":
        hold_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        run_linearity_capture(hold_sec=hold_sec, manager=manager)

    elif action == "photocal":
        hold_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        run_photocal_capture(hold_sec=hold_sec, manager=manager)

    elif action == "bcmburst":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
        run_bcmburst_capture(n_triggers=n, manager=manager)

    elif action == "capture":
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
        run_simple_capture(duration_sec=dur, manager=manager)

    else:
        print(f"Unknown action: {action}")

if __name__ == "__main__":
    main()
