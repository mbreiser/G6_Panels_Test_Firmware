#!/opt/homebrew/bin/python3.14
"""
Digilent Analog Discovery 3 integration for G6 panel testing.

Replaces: external function generator (trigger) + Saleae Logic Pro 8 (capture).

Capabilities:
  - Wavegen 1: 8 kHz trigger signal → GP45 (external trigger for burst mode)
  - Scope Ch 1: Photodiode analog capture (LED brightness measurement)
  - Scope Ch 2: (optional) Direct LED pin measurement or second photodiode

Channel wiring:
  W1 (wavegen 1 output) → GP45 (bodge wire, MCU external trigger input)
  1+ (scope ch 1 +)     → Photodiode output
  1- (scope ch 1 -)     → GND
  2+ (scope ch 2 +)     → GP45 (trigger reference, tee with W1)
  2- (scope ch 2 -)     → GND
  GND                   → Shared ground with MCU board

Usage:
    # Verify loopback (W1 → Ch1):
    python3 ad3_capture.py loopback

    # Verify AD3 is detected:
    python3 ad3_capture.py detect

    # Start 8 kHz trigger and capture photodiode during BCMBURST:
    python3 ad3_capture.py bcmburst [n_triggers]

    # Start trigger + capture during PHOTOCAL (16 BCM levels):
    python3 ad3_capture.py photocal [hold_sec]

    # Manual capture (trigger running, capture for N seconds):
    python3 ad3_capture.py capture [duration_sec]

    # Pulse-triggered averaging (like Saleae analysis):
    python3 ad3_capture.py analyze <npz_path>
"""

import sys
import os
import time
import ctypes
import json
import numpy as np
import serial
import serial.tools.list_ports

# ── DWF library loading ─────────────────────────────────────────────────────

DWF_LIB_PATH = "/Library/Frameworks/dwf.framework/dwf"

# DWF constants (from dwf.h)
hdwfNone = ctypes.c_int(0)

# Device filter
enumfilterAll = ctypes.c_int(0)

# Trigger source
trigsrcNone = ctypes.c_byte(0)
trigsrcPC = ctypes.c_byte(1)
trigsrcDetectorAnalogIn = ctypes.c_byte(2)
trigsrcDetectorDigitalIn = ctypes.c_byte(3)
trigsrcAnalogIn = ctypes.c_byte(4)
trigsrcDigitalIn = ctypes.c_byte(5)
trigsrcDigitalOut = ctypes.c_byte(6)
trigsrcAnalogOut1 = ctypes.c_byte(7)
trigsrcAnalogOut2 = ctypes.c_byte(8)
trigsrcAnalogOut3 = ctypes.c_byte(9)
trigsrcAnalogOut4 = ctypes.c_byte(10)
trigsrcExternal1 = ctypes.c_byte(11)

# Analog out function
funcDC = ctypes.c_byte(0)
funcSine = ctypes.c_byte(1)
funcSquare = ctypes.c_byte(2)
funcTriangle = ctypes.c_byte(3)
funcRampUp = ctypes.c_byte(4)
funcRampDown = ctypes.c_byte(5)
funcNoise = ctypes.c_byte(6)
funcPulse = ctypes.c_byte(7)
funcTrapezoid = ctypes.c_byte(8)
funcSinePower = ctypes.c_byte(9)
funcCustom = ctypes.c_byte(30)
funcPlay = ctypes.c_byte(31)

# Acquisition mode
acqmodeSingle = ctypes.c_int(0)
acqmodeScanShift = ctypes.c_int(1)
acqmodeScanScreen = ctypes.c_int(2)
acqmodeRecord = ctypes.c_int(3)

# Analog input range
DwfAnalogInChannelRange5V = 5.0

# State
DwfStateReady = ctypes.c_byte(0)
DwfStateConfig = ctypes.c_byte(4)
DwfStatePrefill = ctypes.c_byte(5)
DwfStateArmed = ctypes.c_byte(1)
DwfStateWait = ctypes.c_byte(7)
DwfStateTriggered = ctypes.c_byte(3)
DwfStateRunning = ctypes.c_byte(3)
DwfStateDone = ctypes.c_byte(2)


def load_dwf():
    """Load the DWF shared library."""
    try:
        dwf = ctypes.cdll.LoadLibrary(DWF_LIB_PATH)
        ver = ctypes.create_string_buffer(32)
        dwf.FDwfGetVersion(ver)
        print(f"DWF SDK v{ver.value.decode()}")
        return dwf
    except OSError as e:
        print(f"ERROR: Cannot load DWF library from {DWF_LIB_PATH}")
        print(f"  Install Digilent WaveForms: https://digilent.com/shop/software/digilent-waveforms/")
        raise SystemExit(1) from e


# ── Serial helpers (shared with auto_test.py) ────────────────────────────────

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
        print("ERROR: No serial port found for MCU")
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


# ── AD3 device management ────────────────────────────────────────────────────

class AD3:
    """Digilent Analog Discovery 3 controller."""

    def __init__(self):
        self.dwf = load_dwf()
        self.hdwf = ctypes.c_int()
        self._open = False

    def detect(self):
        """Enumerate connected devices."""
        c_count = ctypes.c_int()
        self.dwf.FDwfEnum(enumfilterAll, ctypes.byref(c_count))
        n = c_count.value
        print(f"Devices found: {n}")
        devices = []
        for i in range(n):
            name = ctypes.create_string_buffer(64)
            sn = ctypes.create_string_buffer(64)
            self.dwf.FDwfEnumDeviceName(ctypes.c_int(i), name)
            self.dwf.FDwfEnumSN(ctypes.c_int(i), sn)
            d = {"index": i, "name": name.value.decode(), "serial": sn.value.decode()}
            devices.append(d)
            print(f"  [{i}] {d['name']} (SN: {d['serial']})")
        return devices

    def open(self, device_index=0, config=1):
        """Open a device.

        Args:
            device_index: Device index (0 = first)
            config: Device configuration (0=default 16K, 1=32K scope buffer, 2=8K)
        """
        if self._open:
            return
        # Must enumerate before opening
        c_count = ctypes.c_int()
        self.dwf.FDwfEnum(enumfilterAll, ctypes.byref(c_count))
        if c_count.value == 0:
            raise RuntimeError("No Digilent devices found. Check USB connection.")
        result = self.dwf.FDwfDeviceConfigOpen(ctypes.c_int(device_index),
                                                ctypes.c_int(config), ctypes.byref(self.hdwf))
        if self.hdwf.value == hdwfNone.value:
            szerr = ctypes.create_string_buffer(512)
            self.dwf.FDwfGetLastErrorMsg(szerr)
            raise RuntimeError(f"Failed to open device {device_index}: {szerr.value.decode()}")
        self._open = True
        print(f"AD3: Device {device_index} opened (handle={self.hdwf.value})")

    def close(self):
        """Close the device."""
        if self._open:
            self.dwf.FDwfDeviceClose(self.hdwf)
            self._open = False
            print("AD3: Device closed")

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *args):
        self.close()

    # ── Wavegen (Analog Out) ─────────────────────────────────────────────

    def wavegen_start(self, channel=0, freq_hz=8000.0, amplitude_v=3.3, offset_v=1.65,
                      func=None, duty_cycle=0.5):
        """Start wavegen output.

        Default: 3.3V square wave at 8 kHz (0V low, 3.3V high) — suitable for
        driving GP45 external trigger input.

        Args:
            channel: 0 = W1, 1 = W2
            freq_hz: Frequency in Hz
            amplitude_v: Peak-to-peak amplitude
            offset_v: DC offset (amplitude/2 for 0-to-peak)
            func: Waveform function (default: square)
            duty_cycle: Duty cycle for square/pulse (0.0-1.0)
        """
        if func is None:
            func = funcSquare

        ch = ctypes.c_int(channel)
        self.dwf.FDwfAnalogOutNodeEnableSet(self.hdwf, ch, ctypes.c_int(0), ctypes.c_int(1))  # carrier node
        self.dwf.FDwfAnalogOutNodeFunctionSet(self.hdwf, ch, ctypes.c_int(0), func)
        self.dwf.FDwfAnalogOutNodeFrequencySet(self.hdwf, ch, ctypes.c_int(0), ctypes.c_double(freq_hz))
        self.dwf.FDwfAnalogOutNodeAmplitudeSet(self.hdwf, ch, ctypes.c_int(0), ctypes.c_double(amplitude_v))
        self.dwf.FDwfAnalogOutNodeOffsetSet(self.hdwf, ch, ctypes.c_int(0), ctypes.c_double(offset_v))
        if func.value == funcSquare.value:
            self.dwf.FDwfAnalogOutNodeSymmetrySet(self.hdwf, ch, ctypes.c_int(0),
                                                   ctypes.c_double(duty_cycle * 100.0))
        self.dwf.FDwfAnalogOutConfigure(self.hdwf, ch, ctypes.c_int(1))  # start
        print(f"Wavegen W{channel+1}: {func_name(func)} {freq_hz:.0f} Hz, "
              f"{amplitude_v:.2f}Vpp, offset {offset_v:.2f}V, duty {duty_cycle*100:.0f}%")

    def wavegen_stop(self, channel=0):
        """Stop wavegen output."""
        self.dwf.FDwfAnalogOutConfigure(self.hdwf, ctypes.c_int(channel), ctypes.c_int(0))
        print(f"Wavegen W{channel+1}: stopped")

    def wavegen_trigger(self, channel=0, freq_hz=8000.0):
        """Configure wavegen as 8 kHz 3.3V trigger for GP45.

        Produces a 0-3.3V square wave at the specified frequency.
        Connect W1 → GP45 on the MCU board.
        """
        self.wavegen_start(
            channel=channel,
            freq_hz=freq_hz,
            amplitude_v=3.3,    # 3.3Vpp
            offset_v=1.65,      # center at 1.65V → swings 0 to 3.3V
            func=funcSquare,
            duty_cycle=0.5
        )

    # ── Scope (Analog In) ────────────────────────────────────────────────

    def scope_record(self, duration_sec=1.0, sample_rate=1_000_000, channels=(0,),
                     ranges=None, trigger_channel=None, trigger_level=1.5,
                     trigger_rising=True):
        """Record analog data.

        Args:
            duration_sec: Recording duration
            sample_rate: Samples per second (AD3 max: 125 MHz single ch, 62.5 MHz dual)
            channels: Tuple of channels to enable (0=Ch1, 1=Ch2)
            ranges: Dict of {channel: range_volts}. Default 5V for all.
            trigger_channel: Channel index for trigger, or None for immediate
            trigger_level: Trigger voltage level
            trigger_rising: True for rising edge trigger

        Returns:
            dict with 'time', 'ch1', 'ch2' numpy arrays, 'sample_rate', 'duration'
        """
        if ranges is None:
            ranges = {ch: 5.0 for ch in channels}

        n_samples = int(duration_sec * sample_rate)

        # Disable all channels first, then enable only requested ones
        for ch_idx in range(2):
            enable = 1 if ch_idx in channels else 0
            self.dwf.FDwfAnalogInChannelEnableSet(self.hdwf, ctypes.c_int(ch_idx),
                                                   ctypes.c_int(enable))
            if enable:
                self.dwf.FDwfAnalogInChannelRangeSet(self.hdwf, ctypes.c_int(ch_idx),
                                                      ctypes.c_double(ranges.get(ch_idx, 5.0)))

        # Maximize device-side FIFO buffer (absorbs host polling gaps)
        self.dwf.FDwfAnalogInBufferSizeSet(self.hdwf, ctypes.c_int(32768))

        # Acquisition mode: record
        self.dwf.FDwfAnalogInAcquisitionModeSet(self.hdwf, acqmodeRecord)
        self.dwf.FDwfAnalogInFrequencySet(self.hdwf, ctypes.c_double(float(sample_rate)))
        self.dwf.FDwfAnalogInRecordLengthSet(self.hdwf, ctypes.c_double(duration_sec))

        # Trigger
        if trigger_channel is not None:
            self.dwf.FDwfAnalogInTriggerAutoTimeoutSet(self.hdwf, ctypes.c_double(5.0))
            self.dwf.FDwfAnalogInTriggerSourceSet(self.hdwf, trigsrcDetectorAnalogIn)
            self.dwf.FDwfAnalogInTriggerTypeSet(self.hdwf, ctypes.c_int(0))  # edge
            self.dwf.FDwfAnalogInTriggerChannelSet(self.hdwf, ctypes.c_int(trigger_channel))
            self.dwf.FDwfAnalogInTriggerLevelSet(self.hdwf, ctypes.c_double(trigger_level))
            cond = ctypes.c_int(0) if trigger_rising else ctypes.c_int(1)
            self.dwf.FDwfAnalogInTriggerConditionSet(self.hdwf, cond)
        else:
            self.dwf.FDwfAnalogInTriggerSourceSet(self.hdwf, trigsrcNone)

        # Start
        self.dwf.FDwfAnalogInConfigure(self.hdwf, ctypes.c_int(0), ctypes.c_int(1))
        print(f"Scope: Recording {duration_sec:.2f}s at {sample_rate/1e6:.2f} MHz "
              f"({n_samples} samples, ch {channels})")

        # Pre-allocate output arrays for zero-copy collection
        result_arrays = {ch: np.empty(n_samples, dtype=np.float64) for ch in channels}
        c_available = ctypes.c_int()
        c_lost = ctypes.c_int()
        c_corrupted = ctypes.c_int()
        total_collected = 0
        total_lost = 0
        total_corrupted = 0

        t0 = time.time()
        buf_size = min(n_samples, 65536)
        buf = (ctypes.c_double * buf_size)()

        while total_collected < n_samples:
            sts = ctypes.c_byte()
            self.dwf.FDwfAnalogInStatus(self.hdwf, ctypes.c_int(1), ctypes.byref(sts))
            self.dwf.FDwfAnalogInStatusRecord(self.hdwf,
                                               ctypes.byref(c_available),
                                               ctypes.byref(c_lost),
                                               ctypes.byref(c_corrupted))

            total_lost += c_lost.value
            total_corrupted += c_corrupted.value

            if c_available.value > 0:
                to_read = min(c_available.value, buf_size, n_samples - total_collected)
                for ch in channels:
                    self.dwf.FDwfAnalogInStatusData(self.hdwf, ctypes.c_int(ch),
                                                     buf, ctypes.c_int(to_read))
                    # Copy directly into pre-allocated array (no list append)
                    result_arrays[ch][total_collected:total_collected + to_read] = \
                        np.frombuffer(buf, dtype=np.float64, count=to_read)
                total_collected += to_read

            if sts.value == DwfStateDone.value:
                break

            # Progress (throttled to avoid I/O overhead in hot loop)
            if total_collected % 500000 < to_read if c_available.value > 0 else False:
                pct = total_collected / n_samples * 100
                sys.stdout.write(f"\r  {pct:.0f}% ({total_collected}/{n_samples})")
                sys.stdout.flush()

            time.sleep(0.001)

        elapsed = time.time() - t0
        print(f"\n  Done: {total_collected} samples in {elapsed:.2f}s "
              f"(lost={total_lost}, corrupted={total_corrupted})")

        # Assemble result (slice pre-allocated arrays to actual length)
        result = {
            'sample_rate': sample_rate,
            'duration': duration_sec,
            'n_samples': total_collected,
            'lost': total_lost,
            'corrupted': total_corrupted,
        }
        dt = 1.0 / sample_rate
        result['time'] = np.arange(total_collected) * dt
        for ch in channels:
            result[f'ch{ch+1}'] = result_arrays[ch][:total_collected]

        return result

    def scope_triggered(self, n_captures=100, sample_rate=12_500_000, channels=(0, 1),
                        ranges=None, trigger_source='analogout1', trigger_channel=1,
                        trigger_level=1.5, pre_trigger_pct=5, buf_size=32768):
        """Single-shot triggered acquisition using on-device FPGA buffer.

        Captures at full ADC speed (up to 125 MHz) into FPGA block RAM,
        then transfers to host. No USB streaming bottleneck.

        Args:
            n_captures: Number of triggered acquisitions to collect
            sample_rate: Sample rate (up to 125 MHz single, 62.5 MHz dual)
            channels: Tuple of channels (0=Ch1, 1=Ch2)
            ranges: Dict of {channel: range_volts}
            trigger_source: 'analogout1' (W1), 'detector' (scope ch), 'none'
            trigger_channel: Channel for detector trigger
            trigger_level: Trigger voltage threshold
            pre_trigger_pct: Percentage of buffer before trigger (0-50)
            buf_size: Buffer size in samples (max 32768 with Config 1)

        Returns:
            dict with per-channel arrays (n_captures, buf_size), time axis, metadata
        """
        if ranges is None:
            ranges = {0: 0.5, 1: 5.0}  # Ch1=PD (500mV), Ch2=trigger (5V)

        # Configure channels
        for ch_idx in range(2):
            enable = 1 if ch_idx in channels else 0
            self.dwf.FDwfAnalogInChannelEnableSet(self.hdwf, ctypes.c_int(ch_idx),
                                                   ctypes.c_int(enable))
            if enable:
                self.dwf.FDwfAnalogInChannelRangeSet(self.hdwf, ctypes.c_int(ch_idx),
                                                      ctypes.c_double(ranges.get(ch_idx, 5.0)))

        # Single acquisition mode (on-device buffer)
        self.dwf.FDwfAnalogInAcquisitionModeSet(self.hdwf, acqmodeSingle)
        self.dwf.FDwfAnalogInFrequencySet(self.hdwf, ctypes.c_double(float(sample_rate)))
        self.dwf.FDwfAnalogInBufferSizeSet(self.hdwf, ctypes.c_int(buf_size))

        # Get actual values (device may adjust)
        actual_freq = ctypes.c_double()
        actual_buf = ctypes.c_int()
        self.dwf.FDwfAnalogInFrequencyGet(self.hdwf, ctypes.byref(actual_freq))
        self.dwf.FDwfAnalogInBufferSizeGet(self.hdwf, ctypes.byref(actual_buf))
        sr = actual_freq.value
        bl = actual_buf.value
        window_us = bl / sr * 1e6

        # Trigger setup
        if trigger_source == 'analogout1':
            self.dwf.FDwfAnalogInTriggerSourceSet(self.hdwf, trigsrcAnalogOut1)
        elif trigger_source == 'detector':
            self.dwf.FDwfAnalogInTriggerSourceSet(self.hdwf, trigsrcDetectorAnalogIn)
            self.dwf.FDwfAnalogInTriggerTypeSet(self.hdwf, ctypes.c_int(0))  # edge
            self.dwf.FDwfAnalogInTriggerChannelSet(self.hdwf, ctypes.c_int(trigger_channel))
            self.dwf.FDwfAnalogInTriggerLevelSet(self.hdwf, ctypes.c_double(trigger_level))
            self.dwf.FDwfAnalogInTriggerConditionSet(self.hdwf, ctypes.c_int(0))  # rising
        else:
            self.dwf.FDwfAnalogInTriggerSourceSet(self.hdwf, trigsrcNone)

        self.dwf.FDwfAnalogInTriggerAutoTimeoutSet(self.hdwf, ctypes.c_double(5.0))
        # Pre-trigger position
        post_pct = (100 - pre_trigger_pct) / 100.0
        self.dwf.FDwfAnalogInTriggerPositionSet(self.hdwf,
                                                 ctypes.c_double(bl * post_pct / sr))

        print(f"Scope triggered: {sr/1e6:.2f} MHz, {bl} samples, {window_us:.0f} us window, "
              f"{pre_trigger_pct}% pre-trigger, {n_captures} captures")

        # Pre-allocate arrays
        data = {ch: np.zeros((n_captures, bl), dtype=np.float64) for ch in channels}
        bufs = {ch: (ctypes.c_double * bl)() for ch in channels}

        # Capture loop
        import time
        t0 = time.time()
        for i in range(n_captures):
            self.dwf.FDwfAnalogInConfigure(self.hdwf, ctypes.c_int(0), ctypes.c_int(1))
            sts = ctypes.c_byte()
            while True:
                self.dwf.FDwfAnalogInStatus(self.hdwf, ctypes.c_int(1), ctypes.byref(sts))
                if sts.value == DwfStateDone.value:
                    break
                time.sleep(0.00005)
            for ch in channels:
                self.dwf.FDwfAnalogInStatusData(self.hdwf, ctypes.c_int(ch),
                                                 bufs[ch], ctypes.c_int(bl))
                data[ch][i] = np.frombuffer(bufs[ch], dtype=np.float64)

            if (i + 1) % 100 == 0:
                elapsed = time.time() - t0
                rate = (i + 1) / elapsed
                sys.stdout.write(f"\r  {i+1}/{n_captures} ({rate:.0f}/sec)")
                sys.stdout.flush()

        elapsed = time.time() - t0
        print(f"\n  Done: {n_captures} captures in {elapsed:.1f}s ({n_captures/elapsed:.0f}/sec)")

        # Time axis (t=0 at trigger edge)
        pre_samples = int(bl * pre_trigger_pct / 100)
        t_us = (np.arange(bl) - pre_samples) / sr * 1e6

        result = {
            'sample_rate': sr,
            'buf_size': bl,
            'n_captures': n_captures,
            'window_us': window_us,
            't_us': t_us,
        }
        for ch in channels:
            result[f'ch{ch+1}'] = data[ch]
            result[f'ch{ch+1}_avg'] = data[ch].mean(axis=0)
            result[f'ch{ch+1}_std'] = data[ch].std(axis=0)

        return result

    # ── High-level test sequences ────────────────────────────────────────

    def loopback_test(self):
        """Verify loopback: W1 → Ch1.

        Generates a 1 kHz sine on W1 and captures on Ch1.
        Checks amplitude, frequency, and waveform shape.
        """
        print("\n=== Loopback Test (W1 → Ch1) ===")

        # Generate 1 kHz sine, 2Vpp, 0V offset
        self.wavegen_start(channel=0, freq_hz=1000.0, amplitude_v=2.0, offset_v=0.0,
                           func=funcSine)
        time.sleep(0.1)  # let settle

        # Capture 50 ms at 1 MHz (50k samples, ~50 cycles)
        result = self.scope_record(
            duration_sec=0.05,
            sample_rate=1_000_000,
            channels=(0,),
            ranges={0: 5.0},
        )

        self.wavegen_stop(0)

        # Analyze
        ch1 = result['ch1']
        if len(ch1) == 0:
            print("  FAIL: No data captured")
            return False

        vmin, vmax, vmean = ch1.min(), ch1.max(), ch1.mean()
        vpp = vmax - vmin
        print(f"  Ch1: min={vmin:.3f}V, max={vmax:.3f}V, Vpp={vpp:.3f}V, mean={vmean:.3f}V")

        # Check amplitude (expect ~2Vpp)
        if vpp < 1.5:
            print(f"  FAIL: Vpp too low ({vpp:.3f}V, expected ~2.0V)")
            print("  Check: W1 output connected to Ch1+ input?")
            return False

        # Check frequency via zero crossings
        zero_crossings = np.where(np.diff(np.sign(ch1 - vmean)))[0]
        if len(zero_crossings) > 4:
            periods = np.diff(zero_crossings[::2]) / result['sample_rate']
            freq_est = 1.0 / np.median(periods)
            print(f"  Frequency: {freq_est:.0f} Hz (expected 1000 Hz)")
            if abs(freq_est - 1000) > 50:
                print(f"  WARN: Frequency off by {abs(freq_est - 1000):.0f} Hz")
        else:
            print(f"  WARN: Too few zero crossings ({len(zero_crossings)}) to estimate frequency")

        # Quick trigger test: 8 kHz square wave
        print("\n--- 8 kHz trigger signal test ---")
        self.wavegen_trigger(channel=0, freq_hz=8000.0)
        time.sleep(0.05)

        result2 = self.scope_record(
            duration_sec=0.01,  # 10 ms = 80 cycles
            sample_rate=10_000_000,  # 10 MHz
            channels=(0,),
            ranges={0: 5.0},
        )
        self.wavegen_stop(0)

        ch1_trig = result2['ch1']
        if len(ch1_trig) > 0:
            vmin2, vmax2 = ch1_trig.min(), ch1_trig.max()
            print(f"  8kHz square: min={vmin2:.3f}V, max={vmax2:.3f}V, Vpp={vmax2-vmin2:.3f}V")

            # Count rising edges only (positive threshold crossings)
            thresh = (vmin2 + vmax2) / 2
            above = (ch1_trig > thresh).astype(np.int8)
            rising = np.where(np.diff(above) == 1)[0]
            if len(rising) > 2:
                periods = np.diff(rising) / result2['sample_rate']
                freq8k = 1.0 / np.median(periods)
                print(f"  Measured frequency: {freq8k:.0f} Hz (expected 8000 Hz)")
                if abs(freq8k - 8000) < 100:
                    print(f"  Trigger signal: OK")
                else:
                    print(f"  WARN: Frequency off by {abs(freq8k - 8000):.0f} Hz")
            else:
                print(f"  WARN: Only {len(rising)} rising edges detected")

        print("\n  PASS: Loopback verified")
        return True

    def bcmburst_capture(self, ser, n_triggers=1000, hold_sec=3, trigger_freq=8000,
                         sample_rate=4_000_000, bcm_bits=4, bcm_on=0.5,
                         dual_channel=True, pixel_cmds=None):
        """Run BCMBURST with AD3 generating trigger and capturing photodiode.

        Args:
            ser: Serial connection to MCU
            n_triggers: Number of trigger cycles
            hold_sec: Extra seconds to record after burst
            trigger_freq: Trigger frequency (Hz)
            sample_rate: Scope sample rate
            bcm_bits: BCM bit depth
            bcm_on: BCM ON time (µs)
            dual_channel: If True, capture Ch1 (photodiode) + Ch2 (trigger ref)
            pixel_cmds: Optional list of serial commands to run after FILL 0
                        (e.g., ["PIXEL 10 0 15", "PIXEL 10 1 15", ...])

        Returns:
            dict with capture data and firmware output
        """
        print(f"\n=== BCMBURST Capture ===")
        print(f"  {n_triggers} triggers at {trigger_freq} Hz, "
              f"BCM {bcm_bits}-bit T={bcm_on} µs")
        if dual_channel:
            print(f"  Dual channel: Ch1=photodiode, Ch2=trigger reference (GP45)")

        # Configure MCU
        send_cmd(ser, "EXTTRIG ON", wait_for="External trigger: ON")
        send_cmd(ser, f"BCM {bcm_bits}", wait_for="BCM")
        send_cmd(ser, f"BCMON {bcm_on}", wait_for="BCM")
        send_cmd(ser, "ROWS 20", wait_for="Rows")

        if pixel_cmds:
            # Custom pixel setup
            send_cmd(ser, "FILL 0", wait_for="Fill")
            for cmd in pixel_cmds:
                send_cmd(ser, cmd, wait_for="PIXEL")
                time.sleep(0.05)
        else:
            send_cmd(ser, "FILL 15", wait_for="Fill")
            send_cmd(ser, "PATTERN FFFFF", wait_for="Pattern")

        # Calculate capture duration
        burst_duration = n_triggers / trigger_freq
        capture_duration = burst_duration + hold_sec + 0.5  # extra buffer

        # Channel setup
        channels = (0, 1) if dual_channel else (0,)
        ranges = {0: 5.0, 1: 5.0} if dual_channel else {0: 5.0}

        print(f"  Capture: {capture_duration:.1f}s at {sample_rate/1e6:.1f} MHz")

        # Start trigger signal FIRST (so GP45 is already toggling)
        self.wavegen_trigger(channel=0, freq_hz=trigger_freq)
        time.sleep(0.3)  # let trigger settle

        # Start BCMBURST command (firmware will see triggers immediately)
        ser.reset_input_buffer()
        ser.write(f"BCMBURST {n_triggers}\r\n".encode())
        time.sleep(0.2)

        # Record both channels simultaneously
        result = self.scope_record(
            duration_sec=capture_duration,
            sample_rate=sample_rate,
            channels=channels,
            ranges=ranges,
        )

        # Stop trigger
        self.wavegen_stop(0)

        # Collect firmware output
        time.sleep(1)
        fw_output = ''
        deadline = time.time() + 10
        while time.time() < deadline:
            n = ser.in_waiting
            if n > 0:
                fw_output += ser.read(n).decode(errors='replace')
                time.sleep(0.1)
            else:
                if fw_output and 'BCMBURST' in fw_output.upper():
                    break
                time.sleep(0.2)

        send_cmd(ser, "EXTTRIG OFF", wait_for="External trigger: OFF")

        result['firmware_output'] = fw_output
        print(f"\nFirmware output:\n{fw_output}")

        # Save
        ts = time.strftime("%Y%m%d_%H%M%S")
        npz_path = f"ad3_bcmburst_{ts}.npz"
        save_dict = dict(
            time=result['time'],
            ch1=result['ch1'],
            sample_rate=result['sample_rate'],
            firmware_output=fw_output,
        )
        if 'ch2' in result:
            save_dict['ch2'] = result['ch2']
        np.savez_compressed(npz_path, **save_dict)
        print(f"Saved: {npz_path}")

        return result

    def photocal_capture(self, ser, hold_sec=3, trigger_freq=8000,
                         sample_rate=100_000):
        """Run PHOTOCAL with AD3 generating trigger and capturing photodiode.

        PHOTOCAL cycles through 16 BCM intensity levels, holding each for
        hold_sec seconds with 1-second gaps between levels.

        Args:
            ser: Serial connection to MCU
            hold_sec: Seconds per intensity level
            trigger_freq: Trigger frequency
            sample_rate: Scope sample rate (lower rate OK for slow level changes)

        Returns:
            dict with capture data, per-level analysis
        """
        print(f"\n=== PHOTOCAL Capture ===")
        print(f"  16 levels × {hold_sec}s hold, {trigger_freq} Hz trigger")

        total_duration = 16 * (hold_sec + 1) + 5  # levels + gaps + buffer

        # Configure MCU
        send_cmd(ser, "EXTTRIG ON", wait_for="External trigger: ON")
        send_cmd(ser, "ROWS 20", wait_for="Rows")
        send_cmd(ser, "PATTERN FFFFF", wait_for="Pattern")

        # Start scope recording
        print(f"  Capture: {total_duration:.0f}s at {sample_rate/1e3:.0f} kHz")

        # Start trigger
        self.wavegen_trigger(channel=0, freq_hz=trigger_freq)
        time.sleep(0.2)

        # Start PHOTOCAL
        ser.reset_input_buffer()
        ser.write(f"PHOTOCAL {hold_sec}\r\n".encode())

        # Record
        result = self.scope_record(
            duration_sec=total_duration,
            sample_rate=sample_rate,
            channels=(0,),
            ranges={0: 5.0},
        )

        # Wait for PHOTOCAL to complete
        fw_output = ''
        deadline = time.time() + total_duration + 10
        while time.time() < deadline:
            n = ser.in_waiting
            if n > 0:
                fw_output += ser.read(n).decode(errors='replace')
                if 'PHOTOCAL done' in fw_output or 'PHOTOCAL complete' in fw_output:
                    break
                time.sleep(0.1)
            else:
                time.sleep(0.5)

        # Stop trigger
        self.wavegen_stop(0)
        send_cmd(ser, "EXTTRIG OFF", wait_for="External trigger: OFF")

        result['firmware_output'] = fw_output

        # Save raw data
        ts = time.strftime("%Y%m%d_%H%M%S")
        npz_path = f"ad3_photocal_{ts}.npz"
        np.savez_compressed(npz_path,
                            time=result['time'],
                            ch1=result['ch1'],
                            sample_rate=result['sample_rate'],
                            firmware_output=fw_output)
        print(f"Saved: {npz_path}")

        # Analyze levels
        levels = analyze_photocal_levels(result, hold_sec=hold_sec)
        if levels is not None:
            result['levels'] = levels

        return result


# ── Analysis functions ───────────────────────────────────────────────────────

def analyze_photocal_levels(result, hold_sec=3, n_levels=16):
    """Extract per-level mean voltage from PHOTOCAL capture.

    PHOTOCAL format: level 0 for hold_sec, 1s gap, level 1 for hold_sec, ...
    """
    ch1 = result.get('ch1')
    sr = result.get('sample_rate', 1_000_000)
    if ch1 is None or len(ch1) == 0:
        print("  No data to analyze")
        return None

    period = hold_sec + 1  # hold + gap
    levels = []

    print(f"\n  Level analysis ({n_levels} levels, {hold_sec}s hold):")
    print(f"  {'Level':>5} {'Mean V':>8} {'Std V':>8} {'Norm':>8}")

    for i in range(n_levels):
        # Sample from the middle of each hold period (skip first/last 0.5s)
        t_start = i * period + 0.5
        t_end = i * period + hold_sec - 0.5
        i_start = int(t_start * sr)
        i_end = int(t_end * sr)

        if i_end > len(ch1):
            print(f"  WARN: Data too short for level {i}")
            break

        segment = ch1[i_start:i_end]
        mean_v = np.mean(segment)
        std_v = np.std(segment)
        levels.append({'level': i, 'mean_v': mean_v, 'std_v': std_v})

    if levels:
        max_v = max(l['mean_v'] for l in levels) or 1.0
        for l in levels:
            l['normalized'] = l['mean_v'] / max_v
            print(f"  {l['level']:>5} {l['mean_v']:>8.4f} {l['std_v']:>8.4f} {l['normalized']:>8.3f}")

        # Linearity check
        ideal = np.linspace(0, 1, n_levels)
        actual = np.array([l['normalized'] for l in levels])
        error = np.abs(actual - ideal[:len(actual)]) * 100
        max_err = np.max(error)
        monotonic = all(levels[i+1]['mean_v'] >= levels[i]['mean_v']
                        for i in range(len(levels)-1))
        print(f"\n  Max linearity error: {max_err:.1f}%")
        print(f"  Monotonic: {'YES' if monotonic else 'NO'}")

    return levels


def analyze_pulse_average(npz_path, trigger_threshold=1.5, pre_samples=100, post_samples=2000):
    """Pulse-triggered averaging from saved capture.

    Similar to Saleae pulse averaging: align on trigger edges and average
    the photodiode signal across all pulses.
    """
    data = np.load(npz_path)
    ch1 = data['ch1']
    sr = float(data['sample_rate'])
    print(f"Loaded {npz_path}: {len(ch1)} samples at {sr/1e6:.1f} MHz")

    # For loopback or trigger-on-ch1 analysis, find rising edges
    threshold = trigger_threshold
    rising = np.where(np.diff((ch1 > threshold).astype(int)) == 1)[0]
    print(f"Found {len(rising)} trigger edges (threshold={threshold}V)")

    if len(rising) < 3:
        print("Not enough edges for averaging")
        return None

    # Pulse-triggered average
    valid_edges = rising[(rising >= pre_samples) & (rising < len(ch1) - post_samples)]
    window = pre_samples + post_samples
    stack = np.zeros((len(valid_edges), window))

    for i, edge in enumerate(valid_edges):
        stack[i] = ch1[edge - pre_samples:edge + post_samples]

    avg = np.mean(stack, axis=0)
    t_us = (np.arange(window) - pre_samples) / sr * 1e6

    result = {
        'time_us': t_us,
        'average': avg,
        'n_pulses': len(valid_edges),
        'sample_rate': sr,
    }

    print(f"Pulse average: {len(valid_edges)} pulses, window={window} samples")
    print(f"  Pre-trigger: {avg[:pre_samples].mean():.4f}V")
    print(f"  Peak: {avg[pre_samples:].max():.4f}V")

    return result


# ── Utility ──────────────────────────────────────────────────────────────────

def func_name(func):
    names = {0: 'DC', 1: 'Sine', 2: 'Square', 3: 'Triangle', 4: 'RampUp',
             5: 'RampDown', 6: 'Noise', 7: 'Pulse'}
    return names.get(func.value, f'func{func.value}')


# ── CLI ──────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    cmd = sys.argv[1].lower()

    if cmd == 'detect':
        ad3 = AD3()
        ad3.detect()

    elif cmd == 'loopback':
        ad3 = AD3()
        devices = ad3.detect()
        if not devices:
            print("\nNo AD3 detected. Check USB connection.")
            return
        with ad3:
            ad3.loopback_test()

    elif cmd == 'capture':
        duration = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
        sample_rate = int(float(sys.argv[3])) if len(sys.argv) > 3 else 4_000_000
        dual = '--dual' in sys.argv
        ad3 = AD3()
        with ad3:
            ad3.wavegen_trigger(channel=0, freq_hz=8000.0)
            time.sleep(0.1)
            channels = (0, 1) if dual else (0,)
            result = ad3.scope_record(
                duration_sec=duration,
                sample_rate=sample_rate,
                channels=channels,
            )
            ad3.wavegen_stop(0)

            ts = time.strftime("%Y%m%d_%H%M%S")
            npz_path = f"ad3_capture_{ts}.npz"
            save_dict = dict(time=result['time'], ch1=result['ch1'],
                             sample_rate=result['sample_rate'])
            if 'ch2' in result:
                save_dict['ch2'] = result['ch2']
            np.savez_compressed(npz_path, **save_dict)
            print(f"Saved: {npz_path}")

    elif cmd == 'bcmburst':
        n_triggers = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
        sample_rate = int(float(sys.argv[3])) if len(sys.argv) > 3 else 4_000_000
        # --row10: only light row 10 (all 20 columns at intensity 15)
        row10_mode = '--row10' in sys.argv
        # --single-ch: disable Ch2 trigger reference
        dual = '--single-ch' not in sys.argv
        ad3 = AD3()
        ser = open_serial()
        if not ser:
            return
        pixel_cmds = None
        if row10_mode:
            pixel_cmds = [f"PIXEL 10 {c} 15" for c in range(20)]
        with ad3:
            ad3.bcmburst_capture(ser, n_triggers=n_triggers,
                                 sample_rate=sample_rate,
                                 dual_channel=dual,
                                 pixel_cmds=pixel_cmds)
        ser.close()

    elif cmd == 'photocal':
        hold_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        ad3 = AD3()
        ser = open_serial()
        if not ser:
            return
        with ad3:
            ad3.photocal_capture(ser, hold_sec=hold_sec)
        ser.close()

    elif cmd == 'analyze':
        if len(sys.argv) < 3:
            print("Usage: ad3_capture.py analyze <npz_path>")
            return
        analyze_pulse_average(sys.argv[2])

    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)


if __name__ == '__main__':
    main()
