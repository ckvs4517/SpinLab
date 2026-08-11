# SpinLab firmware

ESP32-C3 firmware for the first SpinLab milestone: capture the Demo machine's
IR-sensor edges with GPIO interrupts, timestamp them, and derive RPM without
high-frequency polling. This repository intentionally starts with a serial
diagnostic baseline before adding data recording, BLE, the web application,
SP calibration, or IMU analysis.

## Prerequisites

- ESP-IDF v5.5.2 (installed under `C:\\Espressif\\v5.5.2`)
- An ESP32-C3 development board and a USB data cable

Open a new PowerShell window, activate ESP-IDF, then build:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. C:\Espressif\tools\Microsoft.v5.5.2.PowerShell_profile.ps1
idf.py set-target esp32c3
idf.py build
```

With the board connected, replace `COMx` with its actual serial port:

```powershell
idf.py -p COMx flash monitor
```

Exit the monitor with `Ctrl+]`.

## First hardware validation

1. Confirm the purchased Demo machine produces stable RPM with its original firmware.
2. Wire its IR sensor output to the configured `SPINLAB_IR_SENSOR_GPIO` in `main/spinlab_main.c` (currently GPIO 2), with a shared ground and appropriate 3.3 V logic level.
3. Set `SPINLAB_PULSES_PER_REVOLUTION` to match the optical pattern.
4. Flash this firmware and record the serial `edge`, `period`, and `rpm` logs.
5. Keep the raw edge timestamps in the next recorder milestone; do not treat this starter's calculated RPM as final calibrated data.

## Scope and next milestones

1. Replace serial-only output with an in-memory raw pulse ring buffer and shot recorder.
2. Add launcher Loaded/Release event capture.
3. Define and implement the custom BLE protocol.
4. Integrate live and historical analysis with the existing Beyblade web system.
