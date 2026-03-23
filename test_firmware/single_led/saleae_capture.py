#!/usr/bin/env python3
"""
Saleae Logic 2 automation for G6 panel timing verification.

Captures digital timing (trigger, row, column pins) and optionally analog
(photodiode) signals. Provides automated capture + analysis pipeline.

Requires:
    pip install logic2-automation
    Logic 2 desktop app running with automation server enabled (port 10430)

Channels (configured for current bodge-wire setup, GP46/47 not available):
    D0 = GP45 (EINT trigger input)
    D1 = GP1  (column 0 — LED timing, BCM bit-plane pattern)
    D2 = GP21 (row 0 — row activation, burst timing)
    D3 = GP2  (column 1 — optional cross-check)
    A0 = Photodiode (optional analog, for linearity characterization)

Usage:
    # Automated capture during BCMBURST:
    python3 saleae_capture.py bcmburst 1000

    # Automated capture during PHOTOCAL:
    python3 saleae_capture.py photocal 3

    # Manual capture with timed duration:
    python3 saleae_capture.py manual 5

    # Analyze previously exported CSV:
    python3 saleae_capture.py analyze path/to/digital.csv
"""

import os
import sys
import time
import csv
import json
import argparse
import statistics
from pathlib import Path
from datetime import datetime

# Serial helpers (reuse from auto_test infrastructure)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BAUD = 115200
CAPTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "saleae_captures")

# ── Serial helpers ──────────────────────────────────────────────────────────

def find_serial_port():
    """Find the RP2350 serial port."""
    import serial.tools.list_ports
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device and "306NTVSCY" not in port.device:
            return port.device
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device:
            return port.device
    return None


def open_serial(port=None):
    """Open serial connection."""
    import serial
    if port is None:
        port = find_serial_port()
    if port is None:
        print("ERROR: No serial port found")
        return None
    ser = serial.Serial(port, BAUD, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()
    print(f"Serial connected: {port}")
    return ser


def send_cmd(ser, cmd, timeout=30, wait_for=None):
    """Send command and collect response."""
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


# ── Saleae Logic 2 automation ──────────────────────────────────────────────

class SaleaeCapture:
    """Wrapper around Logic 2 Automation API."""

    def __init__(self, digital_channels=None, analog_channels=None,
                 digital_sample_rate=100_000_000, analog_sample_rate=1_000_000):
        self.digital_channels = digital_channels or [0, 1, 2, 3]
        self.analog_channels = analog_channels or []
        self.digital_sample_rate = digital_sample_rate
        self.analog_sample_rate = analog_sample_rate
        self.manager = None
        self.capture = None

    def connect(self, port=10430):
        """Connect to Logic 2 automation server."""
        try:
            from saleae import automation
            self.manager = automation.Manager.connect(port=port)
            print(f"Connected to Logic 2 automation server (port {port})")
            return True
        except ImportError:
            print("ERROR: saleae package not installed. Run: pip install logic2-automation")
            return False
        except Exception as e:
            print(f"ERROR: Could not connect to Logic 2 automation server: {e}")
            print("Make sure Logic 2 is running with automation enabled (port 10430)")
            return False

    def start_capture(self, duration_sec=None):
        """Start a capture. If duration_sec is None, capture runs until stop()."""
        from saleae import automation

        device_config = automation.LogicDeviceConfiguration(
            enabled_digital_channels=self.digital_channels,
            enabled_analog_channels=self.analog_channels,
            digital_sample_rate=self.digital_sample_rate,
            analog_sample_rate=self.analog_sample_rate if self.analog_channels else None,
        )

        if duration_sec:
            capture_config = automation.CaptureConfiguration(
                capture_mode=automation.TimedCaptureMode(duration_seconds=duration_sec)
            )
        else:
            capture_config = automation.CaptureConfiguration()

        self.capture = self.manager.start_capture(
            device_configuration=device_config,
            capture_configuration=capture_config,
        )
        print(f"Capture started (digital: {self.digital_channels}, "
              f"analog: {self.analog_channels}, "
              f"rate: {self.digital_sample_rate/1e6:.0f} MHz)")
        return self.capture

    def stop_and_export(self, output_dir=None):
        """Stop capture and export to CSV."""
        from saleae import automation

        if output_dir is None:
            os.makedirs(CAPTURE_DIR, exist_ok=True)
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            output_dir = os.path.join(CAPTURE_DIR, timestamp)
        os.makedirs(output_dir, exist_ok=True)

        self.capture.stop()
        print("Capture stopped.")

        # Export digital data
        digital_csv = os.path.join(output_dir, "digital.csv")
        self.capture.export_raw_data_csv(
            directory=output_dir,
            digital_channels=self.digital_channels,
            analog_channels=self.analog_channels,
        )
        print(f"Data exported to: {output_dir}")
        return output_dir


# ── Analysis functions ─────────────────────────────────────────────────────

def parse_digital_csv(csv_path):
    """Parse Saleae digital CSV export.

    Logic 2 exports digital data as transition timestamps:
    Time [s], Channel N
    Each row is a transition event with timestamp and new value.

    Returns dict of channel_name -> list of (timestamp, value) tuples.
    """
    channels = {}
    with open(csv_path, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)
        # Parse channel names from header
        for i, col in enumerate(header):
            if i > 0:  # skip Time column
                channels[col.strip()] = []

        ch_names = list(channels.keys())
        for row in reader:
            if not row or not row[0]:
                continue
            try:
                t = float(row[0])
                for i, name in enumerate(ch_names):
                    if i + 1 < len(row) and row[i + 1].strip():
                        channels[name].append((t, int(row[i + 1])))
            except (ValueError, IndexError):
                continue

    return channels


def find_rising_edges(events):
    """Find rising edge timestamps from a list of (time, value) transitions."""
    edges = []
    for i, (t, v) in enumerate(events):
        if v == 1 and (i == 0 or events[i-1][1] == 0):
            edges.append(t)
    return edges


def find_falling_edges(events):
    """Find falling edge timestamps from a list of (time, value) transitions."""
    edges = []
    for i, (t, v) in enumerate(events):
        if v == 0 and (i > 0 and events[i-1][1] == 1):
            edges.append(t)
    return edges


def find_pulse_durations(events):
    """Find HIGH pulse durations from transition events."""
    durations = []
    rise_time = None
    for t, v in events:
        if v == 1:
            rise_time = t
        elif v == 0 and rise_time is not None:
            durations.append(t - rise_time)
            rise_time = None
    return durations


def analyze_trigger_signal(trigger_events):
    """Analyze trigger signal: period, frequency, duty cycle."""
    rising = find_rising_edges(trigger_events)
    falling = find_falling_edges(trigger_events)

    if len(rising) < 2:
        print("  Not enough trigger edges for analysis")
        return {}

    periods = [rising[i+1] - rising[i] for i in range(len(rising)-1)]
    frequencies = [1.0 / p for p in periods if p > 0]
    pulses = find_pulse_durations(trigger_events)

    result = {
        'n_edges': len(rising),
        'period_mean_us': statistics.mean(periods) * 1e6,
        'period_stdev_us': statistics.stdev(periods) * 1e6 if len(periods) > 1 else 0,
        'freq_mean_hz': statistics.mean(frequencies),
        'freq_stdev_hz': statistics.stdev(frequencies) if len(frequencies) > 1 else 0,
    }
    if pulses:
        result['duty_cycle_pct'] = statistics.mean(pulses) / statistics.mean(periods) * 100

    return result


def analyze_burst_timing(trigger_events, row_events):
    """Analyze burst timing: trigger-to-burst latency, burst duration, jitter.

    Uses Row 0 HIGH periods as burst indicators (since GP46 sync not available).
    Row 0 is active every 20 triggers (once per frame at 400 Hz).
    """
    trigger_rising = find_rising_edges(trigger_events)
    row_pulses = find_pulse_durations(row_events)
    row_rising = find_rising_edges(row_events)

    result = {}

    # Burst duration from row HIGH time
    if row_pulses:
        result['burst_durations_us'] = [d * 1e6 for d in row_pulses]
        result['burst_mean_us'] = statistics.mean(result['burst_durations_us'])
        result['burst_min_us'] = min(result['burst_durations_us'])
        result['burst_max_us'] = max(result['burst_durations_us'])
        result['burst_jitter_us'] = result['burst_max_us'] - result['burst_min_us']
        if len(result['burst_durations_us']) > 1:
            result['burst_stdev_us'] = statistics.stdev(result['burst_durations_us'])
        result['n_bursts'] = len(row_pulses)

    # Trigger-to-burst latency: find nearest trigger edge before each row rising edge
    if trigger_rising and row_rising:
        latencies = []
        trig_idx = 0
        for row_t in row_rising:
            # Find the trigger edge just before this row activation
            while trig_idx < len(trigger_rising) - 1 and trigger_rising[trig_idx + 1] <= row_t:
                trig_idx += 1
            if trig_idx < len(trigger_rising) and trigger_rising[trig_idx] <= row_t:
                latency = row_t - trigger_rising[trig_idx]
                if latency < 0.001:  # sanity: < 1ms
                    latencies.append(latency)

        if latencies:
            result['latency_mean_us'] = statistics.mean(latencies) * 1e6
            result['latency_max_us'] = max(latencies) * 1e6
            result['latency_min_us'] = min(latencies) * 1e6
            result['n_latency_samples'] = len(latencies)

    return result


def analyze_bcm_pattern(col_events, row_events):
    """Analyze BCM bit-plane pattern on a column pin during row activation.

    During each row burst, the column should show 4 bit-plane pulses
    with durations in ratio 1:2:4:8 (for 4-bit BCM).
    """
    row_rising = find_rising_edges(row_events)
    row_falling = find_falling_edges(row_events)

    if not row_rising or not row_falling:
        return {}

    # For each row burst, extract column pulse pattern
    burst_patterns = []
    for i in range(min(len(row_rising), len(row_falling))):
        r_start = row_rising[i]
        r_end = row_falling[i] if i < len(row_falling) else row_rising[i] + 0.001

        # Find column transitions during this burst
        col_pulses = []
        pulse_start = None
        for t, v in col_events:
            if t < r_start:
                if v == 0:  # column LOW = LED ON (reversed polarity)
                    pulse_start = t
                continue
            if t > r_end:
                break
            if v == 0:  # LED ON (column LOW for reversed polarity)
                pulse_start = t
            elif v == 1 and pulse_start is not None:  # LED OFF
                col_pulses.append((t - max(pulse_start, r_start)) * 1e6)  # µs
                pulse_start = None

        if col_pulses:
            burst_patterns.append(col_pulses)

    if not burst_patterns:
        return {}

    result = {
        'n_bursts_analyzed': len(burst_patterns),
        'example_pattern_us': burst_patterns[0] if burst_patterns else [],
    }

    # Check BCM ratio for first few bursts
    if burst_patterns:
        first = burst_patterns[0]
        if len(first) >= 2:
            ratios = [p / first[0] for p in first]
            result['bit_plane_ratios'] = [round(r, 1) for r in ratios]
            expected = [1, 2, 4, 8][:len(first)]
            result['expected_ratios'] = expected
            result['ratio_match'] = all(
                abs(r - e) / e < 0.15 for r, e in zip(ratios, expected)
            )

    return result


def print_analysis_report(trigger_analysis, burst_analysis, bcm_analysis=None):
    """Print a formatted analysis report."""
    print("\n" + "=" * 60)
    print("SALEAE TIMING ANALYSIS REPORT")
    print("=" * 60)

    if trigger_analysis:
        print("\n--- Trigger Signal (GP45) ---")
        print(f"  Edges detected:  {trigger_analysis.get('n_edges', 0)}")
        print(f"  Period:          {trigger_analysis.get('period_mean_us', 0):.1f} ± "
              f"{trigger_analysis.get('period_stdev_us', 0):.3f} µs")
        print(f"  Frequency:       {trigger_analysis.get('freq_mean_hz', 0):.1f} ± "
              f"{trigger_analysis.get('freq_stdev_hz', 0):.1f} Hz")
        if 'duty_cycle_pct' in trigger_analysis:
            print(f"  Duty cycle:      {trigger_analysis['duty_cycle_pct']:.1f}%")

    if burst_analysis:
        print("\n--- Burst Timing (from GP21 Row 0) ---")
        print(f"  Bursts measured: {burst_analysis.get('n_bursts', 0)}")
        if 'burst_mean_us' in burst_analysis:
            print(f"  Duration:        {burst_analysis['burst_mean_us']:.3f} µs "
                  f"(min={burst_analysis['burst_min_us']:.3f}, "
                  f"max={burst_analysis['burst_max_us']:.3f})")
            print(f"  Jitter (max-min): {burst_analysis['burst_jitter_us']:.3f} µs")
            if 'burst_stdev_us' in burst_analysis:
                print(f"  Stdev:           {burst_analysis['burst_stdev_us']:.3f} µs")
        if 'latency_mean_us' in burst_analysis:
            print(f"\n  Trigger→Burst latency:")
            print(f"    Mean:  {burst_analysis['latency_mean_us']:.3f} µs")
            print(f"    Max:   {burst_analysis['latency_max_us']:.3f} µs")
            print(f"    Min:   {burst_analysis['latency_min_us']:.3f} µs")

    if bcm_analysis:
        print("\n--- BCM Pattern (GP1 Column 0) ---")
        print(f"  Bursts analyzed: {bcm_analysis.get('n_bursts_analyzed', 0)}")
        if 'example_pattern_us' in bcm_analysis:
            patt = bcm_analysis['example_pattern_us']
            print(f"  Bit-plane durations: {[f'{p:.2f}' for p in patt]} µs")
        if 'bit_plane_ratios' in bcm_analysis:
            print(f"  Measured ratios: {bcm_analysis['bit_plane_ratios']}")
            print(f"  Expected ratios: {bcm_analysis['expected_ratios']}")
            print(f"  Ratio match:     {'✅ YES' if bcm_analysis.get('ratio_match') else '✗ NO'}")

    print("\n" + "=" * 60)


# ── High-level capture workflows ──────────────────────────────────────────

def capture_bcmburst(n_triggers=1000, mode='A', use_exttrig=False):
    """Capture a BCMBURST test with Saleae + serial."""
    import serial

    ser = open_serial()
    if not ser:
        return

    sc = SaleaeCapture(
        digital_channels=[0, 1, 2, 3],
        analog_channels=[],
    )
    if not sc.connect():
        print("\nFalling back to manual capture mode.")
        print("1. Start capture in Logic 2 manually")
        input("2. Press Enter when capture is running...")

    # Setup firmware
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'FILL 15']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.5)

    if use_exttrig:
        send_cmd(ser, 'EXTTRIG ON', timeout=3)
        time.sleep(0.3)

    time.sleep(0.5)
    ser.reset_input_buffer()

    # Start capture
    duration = n_triggers / 8000 + 2  # add 2s buffer
    if sc.manager:
        sc.start_capture(duration_sec=duration)
        time.sleep(0.5)

    # Run BCMBURST
    print(f"\nSending BCMBURST {n_triggers} 8000 {mode}...")
    out = send_cmd(ser, f'BCMBURST {n_triggers} 8000 {mode}',
                   timeout=int(duration + 10), wait_for='BCMBURST END')
    print("Firmware response:")
    if out:
        for line in out.strip().split('\n'):
            print(f"  {line}")

    # Export
    if sc.manager and sc.capture:
        output_dir = sc.stop_and_export()
        # Analyze
        digital_csv = os.path.join(output_dir, "digital.csv")
        if os.path.exists(digital_csv):
            analyze_csv(digital_csv)
    else:
        print("\nManual capture mode: stop capture in Logic 2, export CSV, then run:")
        print(f"  python3 {__file__} analyze <path/to/digital.csv>")

    ser.close()


def capture_photocal(hold_sec=3, use_exttrig=False):
    """Capture a PHOTOCAL test with Saleae (including analog for photodiode)."""
    import serial

    ser = open_serial()
    if not ser:
        return

    sc = SaleaeCapture(
        digital_channels=[0, 1, 2],
        analog_channels=[0],  # photodiode on analog channel 0
        analog_sample_rate=1_000_000,
    )
    if not sc.connect():
        print("\nFalling back to manual capture mode.")
        input("Start capture in Logic 2, then press Enter...")

    # Setup
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5']:
        send_cmd(ser, cmd, timeout=3)
        time.sleep(0.5)

    if use_exttrig:
        send_cmd(ser, 'EXTTRIG ON', timeout=3)
        time.sleep(0.3)

    time.sleep(0.5)
    ser.reset_input_buffer()

    # PHOTOCAL runs 16 levels x hold_sec each
    total_time = 16 * hold_sec + 10  # buffer

    if sc.manager:
        sc.start_capture(duration_sec=total_time)
        time.sleep(0.5)

    print(f"\nSending PHOTOCAL {hold_sec}...")
    out = send_cmd(ser, f'PHOTOCAL {hold_sec}',
                   timeout=int(total_time + 30), wait_for='PHOTOCAL END')
    print("Firmware response:")
    if out:
        for line in out.strip().split('\n'):
            print(f"  {line}")

    if sc.manager and sc.capture:
        output_dir = sc.stop_and_export()
        print(f"\nCapture saved to: {output_dir}")
        print("Analyze with: python3 saleae_capture.py analyze <path/to/digital.csv>")
    else:
        print("\nManual capture mode: stop capture, export CSV, then analyze.")

    ser.close()


def capture_manual(duration_sec=5):
    """Start a timed capture without sending any serial commands."""
    sc = SaleaeCapture(
        digital_channels=[0, 1, 2, 3],
        analog_channels=[0],
        analog_sample_rate=1_000_000,
    )
    if not sc.connect():
        print("Cannot connect to Logic 2. Capture manually.")
        return

    sc.start_capture(duration_sec=duration_sec)
    print(f"Capturing for {duration_sec} seconds...")
    time.sleep(duration_sec + 1)

    output_dir = sc.stop_and_export()
    print(f"Saved to: {output_dir}")


def analyze_csv(csv_path):
    """Analyze a previously exported Saleae digital CSV."""
    print(f"Analyzing: {csv_path}")
    channels = parse_digital_csv(csv_path)

    if not channels:
        print("ERROR: No data found in CSV")
        return

    print(f"Channels found: {list(channels.keys())}")
    for name, events in channels.items():
        print(f"  {name}: {len(events)} transitions")

    # Map channels by position (D0=trigger, D1=col0, D2=row0, D3=col1)
    ch_names = list(channels.keys())
    trigger_events = channels.get(ch_names[0], []) if len(ch_names) > 0 else []
    col0_events = channels.get(ch_names[1], []) if len(ch_names) > 1 else []
    row0_events = channels.get(ch_names[2], []) if len(ch_names) > 2 else []

    trigger_analysis = analyze_trigger_signal(trigger_events) if trigger_events else {}
    burst_analysis = analyze_burst_timing(trigger_events, row0_events) if row0_events else {}
    bcm_analysis = analyze_bcm_pattern(col0_events, row0_events) if col0_events and row0_events else {}

    print_analysis_report(trigger_analysis, burst_analysis, bcm_analysis)

    # Save results as JSON
    results = {
        'csv_path': csv_path,
        'timestamp': datetime.now().isoformat(),
        'trigger': trigger_analysis,
        'burst': {k: v for k, v in burst_analysis.items() if k != 'burst_durations_us'},
        'bcm': bcm_analysis,
    }
    json_path = csv_path.replace('.csv', '_analysis.json')
    with open(json_path, 'w') as f:
        json.dump(results, f, indent=2, default=str)
    print(f"\nResults saved to: {json_path}")


# ── Main ───────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='Saleae Logic 2 capture & analysis for G6 panel')
    subparsers = parser.add_subparsers(dest='command')

    # bcmburst
    p_burst = subparsers.add_parser('bcmburst', help='Capture BCMBURST test')
    p_burst.add_argument('n_triggers', type=int, nargs='?', default=1000)
    p_burst.add_argument('--mode', default='A', choices=['A', 'B', 'C'])
    p_burst.add_argument('--exttrig', action='store_true', help='Use external trigger')

    # photocal
    p_photo = subparsers.add_parser('photocal', help='Capture PHOTOCAL linearity test')
    p_photo.add_argument('hold_sec', type=float, nargs='?', default=3)
    p_photo.add_argument('--exttrig', action='store_true')

    # manual
    p_manual = subparsers.add_parser('manual', help='Timed capture (no serial commands)')
    p_manual.add_argument('duration', type=float, nargs='?', default=5)

    # analyze
    p_analyze = subparsers.add_parser('analyze', help='Analyze exported CSV')
    p_analyze.add_argument('csv_path', help='Path to Saleae digital CSV export')

    args = parser.parse_args()

    if args.command == 'bcmburst':
        capture_bcmburst(args.n_triggers, args.mode, args.exttrig)
    elif args.command == 'photocal':
        capture_photocal(args.hold_sec, args.exttrig)
    elif args.command == 'manual':
        capture_manual(args.duration)
    elif args.command == 'analyze':
        analyze_csv(args.csv_path)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
