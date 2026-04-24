# AD3 Record Mode Optimization Guide

Reference for optimizing Analog Discovery 3 streaming acquisition rate in WaveForms Record mode. Current observed rate: ~4 MS/s with dropped frames. Target: approach the theoretical ~10 MS/s ceiling.

## Architecture Overview

- AD3 ADC samples at up to 125 MS/s into Spartan 7 FPGA block RAM
- FPGA streams data over USB to host PC
- **AD3 uses USB 2.0** (~40 MB/s effective throughput) despite having a USB-C connector
- Record mode spec: ~10 MS/s streaming to host RAM, ~5 MS/s streaming directly to file
- Practical ceiling observed by users/Digilent: 10–12 MS/s under ideal conditions
- Device-side FIFO buffer is small (32K–64K samples depending on configuration) — overflows cause "Samples were lost" errors

## Optimization Checklist

### 1. Switch to Device Configuration 2

In WaveForms Device Manager, change from the default Configuration 1 to **Configuration 2**. This allocates more FPGA block RAM to the Scope instrument's FIFO buffer. A larger FIFO absorbs brief host-side polling gaps that would otherwise cause sample loss.

### 2. Enable Only Required Scope Channels

Total USB streaming bandwidth is shared across all active channels. If you only need one analog input channel, disable the other. This effectively doubles available per-channel streaming bandwidth.

### 3. Use a Root Hub USB Port

Connect the AD3 to a USB port directly on the root hub — not through a hub, and not on a port shared with other active USB devices (cameras, external drives, etc.). USB 2.0 bandwidth is shared per hub/controller.

### 4. Minimize Host System Load

Close other applications that consume CPU, memory, or disk I/O. Record mode streaming is latency-sensitive — the host must service USB transfers fast enough to prevent the small device-side FIFO from overflowing.

### 5. Record to RAM, Not File (When Possible)

- Streaming to host RAM: up to ~10 MS/s
- Streaming directly to file: up to ~5 MS/s
- If recording to file at higher rates, use **binary or WAV format with 16-bit samples** — not CSV/text. Serialization overhead matters.

## SDK-Specific Notes (Python/ctypes via WaveForms SDK)

### Polling Loop Performance

The `FDwfAnalogInStatusRecord` function returns `cAvailable`, `cLost`, and `cCorrupted`. If `cLost > 0`, the host polling loop isn't keeping up with the device FIFO.

Optimization strategies:
- Minimize work inside the polling loop — read data and append to a pre-allocated buffer, defer all processing
- Use `time.sleep()` values that are short enough to keep up but not so short they spin the CPU (start with 0.001s and tune)
- Pre-allocate numpy arrays rather than growing lists
- Avoid print statements or logging inside the hot loop

### Key SDK Functions

```python
# Set acquisition mode to Record
dwf.FDwfAnalogInAcquisitionModeSet(hdwf, acqmodeRecord)

# Set record length (seconds) — or 0 for infinite
dwf.FDwfAnalogInRecordLengthSet(hdwf, c_double(record_length))

# In polling loop:
dwf.FDwfAnalogInStatus(hdwf, c_int(1), byref(sts))
dwf.FDwfAnalogInStatusRecord(hdwf, byref(cAvailable), byref(cLost), byref(cCorrupted))

# Read available samples
if cAvailable.value > 0:
    dwf.FDwfAnalogInStatusData(hdwf, c_int(channel), byref(rgdSamples), cAvailable)
```

### Buffer Size Configuration via SDK

You can also set the device buffer size programmatically:
```python
# Increase device buffer for record mode
dwf.FDwfAnalogInBufferSizeSet(hdwf, c_int(32768))  # max for single channel in Config 2
```

## Hard Limits

- **USB 2.0 is the bottleneck** — no software optimization can exceed ~10–12 MS/s sustained streaming on the AD3
- For sustained rates above this, the AD3 is not sufficient. Alternatives with larger DDR buffers and USB 3.0+:
  - **ADP2230**: 256M sample DDR buffer + USB 5 Gbps
  - **ADP3450**: 128M sample buffer (but also USB 2.0/Ethernet limited in practice)
- For short captures (≤32K–64K samples), the AD3 can capture at the full 125 MS/s into its FPGA buffer — Record mode is only needed for longer acquisitions

## Sources

- [AD3 Getting Started (WaveForms 3.24/3.25)](https://files.digilent.com/manuals/WaveForms/3.24.3/start10.html)
- [AD3 Reference Manual](https://digilent.com/reference/test-and-measurement/analog-discovery-3/reference-manual)
- [AD3 Specifications](https://digilent.com/reference/test-and-measurement/analog-discovery-3/specifications)
- [Digilent Forum — attila (Digilent engineer) posts on buffer/record mode](https://forum.digilent.com/profile/36-attila/content/)
- [WaveForms SDK Reference Manual](https://digilent.com/reference/software/waveforms/waveforms-3/reference-manual)
