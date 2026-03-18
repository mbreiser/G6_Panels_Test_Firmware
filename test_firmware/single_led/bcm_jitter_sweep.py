#!/usr/bin/env python3
"""
BCM jitter sweep: measure jitter across all intensity levels and base ON times.

Runs BCMBURST for each combination of T and intensity, collects timing data.
Output: CSV table suitable for analysis.

Usage:
    python3 bcm_jitter_sweep.py
"""

import serial
import serial.tools.list_ports
import time
import sys
import os
import re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from status_dashboard import update_status

BAUD = 115200

def find_serial_port():
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device and "306NTVSCY" not in port.device:
            return port.device
    for port in serial.tools.list_ports.comports():
        if "usbmodem" in port.device:
            return port.device
    return None

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

def parse_bcmburst(output):
    """Parse BCMBURST output for burst timing stats."""
    result = {}
    for line in output.split('\n'):
        line = line.strip()
        if line.startswith('burst:'):
            for m in re.finditer(r'(\w+)=([\d.]+)us', line):
                result[f'burst_{m.group(1)}'] = float(m.group(2))
        elif 'Jitter' in line and 'max-min' in line:
            m = re.search(r'([\d.]+)us', line.split(':')[-1])
            if m:
                result['jitter_us'] = float(m.group(1))
        elif 'Outliers' in line:
            # "Outliers (>2x nominal): 0 / 10000 (0.00%)"
            m = re.search(r'(\d+)\s*/\s*(\d+)', line)
            if m:
                result['outliers'] = int(m.group(1))
                result['triggers'] = int(m.group(2))
        elif line.startswith('N='):
            m = re.search(r'N=(\d+)', line)
            if m:
                result['triggers'] = int(m.group(1))
    return result

def main():
    T_values = [0.25, 0.5, 0.75, 1.0]
    intensities = list(range(16))
    n_frames = 10000
    rate_hz = 8000

    port = find_serial_port()
    if not port:
        print("ERROR: No serial port found")
        return

    ser = serial.Serial(port, BAUD, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()
    print(f"Connected: {port}")

    # Setup
    send_cmd(ser, "ROWS 20", timeout=3)
    time.sleep(0.3)

    # Warm-up run to prime caches (first run after boot has cold-start jitter)
    print("Warm-up run...")
    send_cmd(ser, "BCM 4", timeout=3)
    time.sleep(0.3)
    send_cmd(ser, "BCMON 0.5", timeout=3)
    time.sleep(0.3)
    send_cmd(ser, "FILL 8", timeout=3)
    time.sleep(0.5)
    ser.reset_input_buffer()
    time.sleep(0.2)
    send_cmd(ser, f"BCMBURST 1000 {rate_hz} A", timeout=15, wait_for="BCMBURST END")
    time.sleep(0.5)
    print("Warm-up done. Starting sweep.\n")

    total = len(T_values) * len(intensities)
    done = 0

    # Print CSV header
    print("T_us,intensity,burst_min_us,burst_max_us,burst_mean_us,jitter_us,outliers,triggers")

    results = []

    for T in T_values:
        # Set BCM parameters
        send_cmd(ser, "BCM 4", timeout=3)
        time.sleep(0.2)
        send_cmd(ser, f"BCMON {T}", timeout=3)
        time.sleep(0.2)

        for intensity in intensities:
            done += 1
            update_status("BCM jitter sweep",
                         f"T={T}us intensity={intensity} ({done}/{total})",
                         f"{done}/{total}")

            # Set uniform intensity
            send_cmd(ser, f"FILL {intensity}", timeout=3)
            time.sleep(0.2)

            # Drain any buffered data thoroughly
            time.sleep(0.5)
            ser.reset_input_buffer()
            time.sleep(0.2)

            # Run BCMBURST
            out = send_cmd(ser, f"BCMBURST {n_frames} {rate_hz} A",
                          timeout=30, wait_for="BCMBURST END")

            if out is None:
                print(f"{T},{intensity},ERR,ERR,ERR,ERR,ERR,ERR")
                continue

            p = parse_bcmburst(out)

            row = (f"{T},{intensity},"
                   f"{p.get('burst_min', 'ERR')},"
                   f"{p.get('burst_max', 'ERR')},"
                   f"{p.get('burst_mean', 'ERR')},"
                   f"{p.get('jitter_us', 'ERR')},"
                   f"{p.get('outliers', 'ERR')},"
                   f"{p.get('triggers', 'ERR')}")
            print(row)
            sys.stdout.flush()
            results.append(p | {'T': T, 'intensity': intensity})

            time.sleep(0.1)

    ser.close()
    update_status("BCM sweep DONE", f"{total} configs tested")

    # Summary
    print("\n=== SUMMARY ===")
    print(f"Total configs: {total}")
    max_jitter = max((r.get('jitter_us', 0) for r in results), default=0)
    total_outliers = sum(r.get('outliers', 0) for r in results)
    print(f"Max jitter across all configs: {max_jitter} us")
    print(f"Total outliers across all configs: {total_outliers}")

if __name__ == "__main__":
    main()
