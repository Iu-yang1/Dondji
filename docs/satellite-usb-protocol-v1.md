# Dondji USB Satellite Protocol v1

Dondji remains the RF executor. Orbit propagation, transponder mapping and Doppler calculation stay on the host (for example Look4Sat-FT4). The radio receives final corrected RX/TX frequencies and lightweight display telemetry.

## Transport

The protocol reuses Dondji's existing USB CDC framing and CRC-CCITT implementation.

All multibyte fields are little-endian.

```text
AB CD
uint16 inner_size
(inner payload XOR-obfuscated with the existing 16-byte Dondji mask)
(uint16 CRC-CCITT over the decrypted inner payload, then XOR-obfuscated as the next two bytes)
DC BA
```

The decrypted inner payload starts with:

```c
uint16 command_id;
uint16 data_size;
uint8  data[data_size];
```

Realtime frames are constrained to at most 64 bytes on the wire so a `SAT_UPDATE` fits in one USB Full-Speed bulk packet.

## Units

- Host frequencies: absolute Hz (`uint32_t`).
- Radio tuning resolution: 10 Hz. Dondji rounds `(hz + 5) / 10` internally.
- Doppler values: signed Hz.
- UTC/AOS/LOS: Unix seconds UTC (`uint32_t`).
- Azimuth/elevation: 0.1 degree.
- CTCSS: 0.1 Hz, matching the existing Dondji CTCSS table; zero disables TX CTCSS.

## Commands

| Request | Reply | Purpose |
| --- | --- | --- |
| `0x0700 SAT_HELLO` | `0x0701` | Version/capability negotiation |
| `0x0702 SAT_BEGIN` | `0x0703` | Start an exclusive satellite-control session |
| `0x0704 SAT_UPDATE` | `0x0705` optional | High-rate RX/TX/Doppler/telemetry state stream |
| `0x0706 SAT_STATUS` | `0x0707` | Read link/RF state and counters |
| `0x0708 SAT_END` | `0x0709` | End session and restore the user's radio state |
| `0x070A SAT_UI_CONTROL` | `0x070B` | Show/hide/toggle the tracking screen |
| `0x070C SAT_PTT` | `0x070D` | Reserved for a later remote-PTT phase; v1 replies unsupported |

The C wire structures live in `App/app/satellite_protocol.h` and are the normative field layout.

## Session model

`SAT_BEGIN` snapshots the active VFO and temporary receive-mode state, disables dual-watch/cross-band for deterministic RF ownership, and configures one VFO with independent RX and TX target frequencies. It does not write these temporary values to persistent settings.

A second `SAT_BEGIN` is rejected while a session is active. `SAT_END` restores the snapshot. Starting a session is also rejected while transmitting, scanning, or another serial configuration session is active.

## Realtime update model

`SAT_UPDATE` is a state stream, not a command queue:

- Sequence numbers are 16-bit and compared wrap-safely.
- Duplicate/old updates are ignored.
- Missing sequence numbers increment the drop counter.
- Only the newest valid RX/TX pair is kept.
- RX and TX are atomically replaced from one update.
- A band change inside the realtime stream is rejected; start a new session for a different band pair.

The host normally sends 10-20 updates/s. `SAT_HELLO` advertises a protocol capability of 100 updates/s. Dondji applies frequency changes directly through BK4829 REG38/REG39 instead of running a full RF reconfiguration for every Doppler step.

`SAT_UPDATE` does not generate an ACK by default. Set `SAT_UPDATE_ACK_REQUEST` when periodic applied-state confirmation is desired.

## Physical PTT

The live VFO always contains the newest corrected TX target. Therefore a physical PTT transition uses the current TX frequency through the normal Dondji TX state machine. During transmission, later `SAT_UPDATE` frames continue retuning the TX frequency directly. Releasing PTT returns to the newest RX target.

The v1 capability set intentionally does not advertise remote PTT.

## Link watchdog

- `0-500 ms`: fresh
- `>500 ms to 3000 ms`: stale; last valid pair remains usable
- `>3000 ms`: lost

On link loss Dondji stops satellite control. If PTT is held, serial configuration interlock is asserted so TX is terminated/inhibited until the user releases PTT; only then is the saved pre-session radio state restored. This avoids falling back to an unrelated VFO while PTT is still physically held.

## UI scheduling

USB parsing/RF application and LCD refresh are deliberately decoupled. Incoming updates may run at high rate, while the tracking page requests redraw at about 5 Hz. LCD work must never be performed from the USB ISR or directly from the packet parser.
