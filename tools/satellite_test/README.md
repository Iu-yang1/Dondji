# Dondji Satellite USB Tester

Windows GUI test host for Dondji USB Satellite Protocol 1.1 on the `motorola_r7` firmware.

## Features

- Discovers Windows COM ports and highlights Dondji VID `36B7` / PID `FFFF`.
- Sends `SAT_HELLO`, `SAT_BEGIN`, `SAT_STATUS`, `SAT_END`, and satellite UI control commands.
- Streams `SAT_UPDATE` at 1 / 5 / 10 / 20 / 50 / 100 Hz.
- Simulates RX/TX Doppler sweeps without remote PTT; physical PTT stays on the radio.
- Displays device `received_seq`, `applied_seq`, `crc_errors`, `dropped_updates`, packet age, measured update rate, PTT state, link state, and UI visibility.
- Validates reply framing and CRC on the host side.
- Runs an automatic 1→100 Hz stress test and exports CSV results and text logs.

## Safety

For PTT tests, connect the radio to a suitable dummy load. The tester intentionally does not implement remote PTT.

## Run from source

```powershell
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python app.py
```

## Windows executable

`.github/workflows/build-satellite-tester.yml` builds `DondjiSatelliteTester.exe` as a PyInstaller single-file windowed application. Python and pyserial are bundled and are not required on the test PC.

The executable is unsigned, so Windows SmartScreen may warn on first launch.

## Typical test order

1. Connect the Dondji USB CDC COM port.
2. Click **HELLO** and verify protocol 1.1 and capabilities.
3. Click **BEGIN**; with auto-show enabled the radio should open the satellite page.
4. Start a 20 Hz realtime stream and verify device CRC/drop counters stay at zero.
5. Use physical PTT with a dummy load and confirm TX/RX frequency switching.
6. Run the automatic 1→100 Hz stress test.
7. Click **END** to restore the pre-session radio state.
