# ZMK Studio `settings` Subsystem — Frontend Integration Guide

Target audience: the ZMK Studio frontend team building the UI for device settings.

This document describes the new `settings` RPC subsystem added to the ZMK firmware
(branch `feat/studio-device-settings`, commit `0cfb3ba0`). It covers the wire format,
the full message schema, the semantics of every field, and the recommended client
behavior for building a settings UI.

Proto source: `zmk-studio-messages` commit `ff7f991`
(`proto/zmk/settings.proto`, `proto/zmk/settings.options`, `proto/zmk/studio.proto`).

---

## 1. Overview

The `settings` subsystem (subsystem tag **9**) lets Studio:

- **Read** a full snapshot of device settings and live diagnostics (`get_settings`).
- **Write** settings (`set_settings`):
  - Bluetooth: select the active profile, clear one or all bonds, disconnect profiles.
  - Sleep: enable/disable deep sleep, set the idle timeout and the deep-sleep timeout.
  - Endpoint: set the preferred transport (USB or BLE).
- **Receive** a `settings_changed` notification whenever relevant device state
  changes, so the UI can refresh.

Diagnostics are read-only: battery level, USB power, WPM, HID indicators
(num/caps/scroll lock, etc.), split (second half) status, uptime, and activity state.

Everything is transport-agnostic (same API over UART or BLE GATT). The Coban
keyboards use the `studio-rpc-usb-uart` transport (USB CDC-ACM serial port).

---

## 2. Transport and wire format

Messages are **protobuf** encoded (`zmk_studio_Request` from host → device,
`zmk_studio_Response` from device → host), wrapped in a byte-stuffed frame:

| Byte | Value  | Meaning                               |
| ---- | ------ | ------------------------------------- |
| SOF  | `0xAB` | Start of frame                        |
| ESC  | `0xAC` | Escape: the next byte is literal data |
| EOF  | `0xAD` | End of frame                          |

- A frame is: `SOF <escaped payload> EOF`.
- Byte stuffing: any payload byte equal to `0xAB`, `0xAC`, or `0xAD` is prefixed
  with `0xAC` on the wire. The device strips `0xAC` and passes the following byte
  through unchanged.
- If the device loses sync (e.g. unescaped SOF mid-frame, data before SOF), it
  discards bytes until the next SOF/EOF. The host should treat a garbled frame as
  "no message" and keep listening; no retransmission protocol exists.
- There is **no ACK/retry**: if a request gets no response, the host may simply
  re-send it. All `set_settings` operations are idempotent in practice (setting a
  value to its current value is a no-op).

---

## 3. Envelope

All traffic uses the `zmk.studio` envelope (see `studio.proto`):

```proto
message Request {
  uint32 request_id = 1;          // host-chosen, echoed back in the response
  oneof subsystem {
    ...
    zmk.settings.Request settings = 9;
  }
}

message Response {
  oneof type {
    RequestResponse request_response = 1;   // has request_id + subsystem response
    Notification notification = 2;          // unsolicited, no request_id
  }
}
```

Client rules:

- Generate a monotonically increasing `request_id` per request.
- Correlate `RequestResponse` messages by `request_id`.
- `Notification` messages arrive unsolicited at any time — they are never tied to
  a request. Handle them independently of the request/response flow.
- `zmk_studio_Response` may also carry a `meta` error response (see §8) instead of
  a `settings` response.

---

## 4. API

### 4.1 `get_settings`

```proto
// request
message Request {
  oneof request_type {
    bool get_settings = 1;   // set to true (presence selects the handler)
    SetSettingsRequest set_settings = 2;
  }
}

// response
message Response {
  oneof response_type {
    Settings get_settings = 1;      // full snapshot, see §5
    SetSettingsResponse set_settings = 2;
  }
}
```

The request body is empty. The response is always a **complete** `Settings`
snapshot: Bluetooth, sleep, endpoint, and diagnostics — every sub-message is
always present.

### 4.2 `set_settings`

```proto
message SetSettingsRequest {
  SetBluetoothSettingsRequest bluetooth = 1;  // optional
  SetSleepSettingsRequest sleep = 2;          // optional
  SetEndpointSettingsRequest endpoint = 3;    // optional
}

message SetSettingsResponse {
  SettingsResult result = 1;   // OK only if every applied section succeeded
  Settings settings = 2;       // always present: fresh snapshot after applying
}
```

- **Partial updates**: include only the sections you want to change. Omitted
  sections are not touched.
- **Application order**: `bluetooth` → `sleep` → `endpoint`. Application stops at
  the first section that fails; later sections are not applied.
- The response always contains a **fresh full snapshot**, so the UI can update
  directly from the response without a follow-up `get_settings`.

### 4.3 `settings_changed` notification

```proto
message Notification {
  oneof notification_type {
    bool settings_changed = 1;   // always true when sent
  }
}
```

The notification is deliberately a **boolean signal, not a payload**. On receipt,
the client should re-issue `get_settings` to fetch the current snapshot. This keeps
firmware event-path memory usage small.

It is emitted (best-effort) when any of these device events occur:

| Trigger                    | Example                                   |
| -------------------------- | ----------------------------------------- |
| Activity state changed     | device went idle / woke / deep-slept      |
| Active BLE profile changed | profile switched, connection made/dropped |
| Endpoint changed           | transport switched (USB ↔ BLE)            |
| Battery state changed      | battery level updated                     |
| HID indicators changed     | num/caps/scroll lock toggled by host      |

Notifications are **coalesced with a ~100 ms debounce**: a burst of related events
(e.g. a profile switch, which can raise several events at once) produces a single
notification. They are also sent asynchronously from a work queue, so a
notification may arrive slightly after the change and is never sent in the middle
of a `set_settings` response.

Treat notifications as a _hint to refresh_, not as a guarantee: always be able to
render from a `get_settings` response alone (e.g. on connect, open the settings
page by fetching, and optionally poll every few seconds as a fallback).

---

## 5. Schema reference

### 5.1 `Settings` (snapshot)

```proto
message Settings {
  BluetoothSettings bluetooth = 1;
  SleepSettings sleep = 2;
  EndpointSettings endpoint = 3;
  Diagnostics diagnostics = 4;
}
```

#### `BluetoothSettings`

```proto
message BluetoothSettings {
  repeated BluetoothProfile profiles = 1;  // one entry per profile, index order
  uint32 active_profile = 2;               // currently selected profile index
}

message BluetoothProfile {
  uint32 index = 1;              // 0-based profile index
  bool bonded = 2;               // a peer address is stored for this profile
  bool connected = 3;            // currently connected to that peer
  bool active = 4;               // this is the active profile
  bytes peer_address = 5;        // 6 bytes LE address; empty when not bonded
  AddressType address_type = 6;  // PUBLIC or RANDOM (of the stored peer)
  string label = 7;              // stored profile name; empty if unset
}
```

Notes for the UI:

- Profile count is fixed at compile time: **4** on the Coban boards
  (`CONFIG_BT_MAX_PAIRED=4`). The proto allows up to 8; render whatever
  `profiles` contains.
- `label` is the stored profile name (max 15 chars). It can be empty.
- `peer_address` is the raw 6-byte LE address of the bonded peer (empty when
  not bonded). Format it as `XX:XX:XX:XX:XX:XX` for display.
- A profile can be `bonded` but not `connected`; `active` means it is the profile
  the keyboard currently uses for BLE HID.

#### `SleepSettings`

```proto
message SleepSettings {
  bool supported = 1;            // deep sleep compiled in for this firmware
  bool enabled = 2;              // deep sleep currently enabled at runtime
  uint32 idle_timeout_ms = 3;    // ms of inactivity before idle state
  uint32 sleep_timeout_ms = 4;   // ms of inactivity before deep sleep (0 if unsupported)
  ActivityState activity_state = 5;  // live: ACTIVE / IDLE / SLEEP
  bool usb_powered = 6;          // live: USB power present (deep sleep is skipped when true)
}
```

Notes:

- `idle_timeout_ms` always applies (idle state shuts down the display, etc.).
- `sleep_timeout_ms` only matters when `supported && enabled`. Deep sleep is
  additionally skipped while USB power is present.
- `activity_state` is live state, not a setting.

#### `EndpointSettings`

```proto
message EndpointSettings {
  Transport preferred_transport = 1;  // user preference: USB or BLE
  Transport selected_transport = 2;   // transport actually in use right now
  uint32 selected_ble_profile = 3;    // BLE profile index of the selected endpoint
                                      // (meaningful only when selected_transport == BLE)
  bool connected = 4;                 // selected endpoint is currently connected
}
```

The _preferred_ transport is the setting the UI writes. The _selected_ transport
reflects reality (e.g. preference is BLE but no BLE connection → selected is USB,
or NONE if nothing is connected).

#### `Diagnostics`

```proto
message Diagnostics {
  bool battery_supported = 1;
  uint32 battery_level = 2;       // 0–100 %
  bool usb_powered = 3;
  bool wpm_supported = 4;
  uint32 wpm = 5;                 // current words-per-minute estimate (0 when idle)
  bool hid_indicators_supported = 6;
  HidIndicators hid_indicators = 7;
  bool split_supported = 8;       // this firmware acts as a split central
  SplitDiagnostics split = 9;
  uint32 uptime_ms = 10;          // wraps after ~49.7 days (int32 ms)
  ActivityState activity_state = 11;
}

message HidIndicators {
  uint32 raw = 1;                 // raw bitmask
  bool num_lock = 2;
  bool caps_lock = 3;
  bool scroll_lock = 4;
  bool compose = 5;
  bool kana = 6;
}

message SplitDiagnostics {
  bool available = 1;             // a split transport is present
  bool enabled = 2;               // the transport is enabled
  SplitConnectionsStatus connections = 3;  // DISCONNECTED / SOME_CONNECTED / ALL_CONNECTED
  repeated SplitPeripheral peripherals = 4;
}

message SplitPeripheral {
  uint32 source = 1;              // peripheral source id
  bool connected = 2;
  bool battery_known = 3;         // battery level was reported by the peripheral
  uint32 battery_level = 4;       // 0–100 %, valid only when battery_known
}
```

Everything under `Diagnostics` is **read-only**. `*_supported` flags tell the UI
which widgets to show (hide battery/WPM/indicators/split sections when the
firmware doesn't have that feature compiled in).

### 5.2 `SetSettingsRequest` sub-messages

#### `SetBluetoothSettingsRequest`

```proto
message SetBluetoothSettingsRequest {
  message Index { uint32 value = 1; }

  Index active_profile = 1;               // optional: make this profile active
  repeated uint32 clear_bond_profiles = 2; // optional: clear bond(s) for these indexes
  bool clear_all_bonds = 3;               // optional: clear every bond
  repeated uint32 disconnect_profiles = 4; // optional: disconnect these profiles
}
```

- All fields are optional; include only what the user did.
- Device-side order: `clear_all_bonds`, then `clear_bond_profiles`, then
  `disconnect_profiles`, finally `active_profile` selection.
- Index out of range → `INVALID_VALUE`.
- Clearing the bond of the _active_ profile restarts advertising (the keyboard
  becomes discoverable again).
- These actions affect the stored bonds/profiles, which persist across reboots.

#### `SetSleepSettingsRequest`

```proto
message SetSleepSettingsRequest {
  message BoolValue { bool value = 1; }
  message UInt32Value { uint32 value = 1; }

  BoolValue enabled = 1;            // optional: enable/disable deep sleep
  UInt32Value idle_timeout_ms = 2;  // optional
  UInt32Value sleep_timeout_ms = 3; // optional
}
```

- The wrapper messages (`BoolValue`, `UInt32Value`) exist because proto3 scalars
  have no presence: **omit the wrapper to mean "don't change"**.
- Validation:
  - `enabled = true` on a firmware without deep sleep → `UNSUPPORTED`.
  - `idle_timeout_ms = 0` → `INVALID_VALUE` (must be > 0).
  - `sleep_timeout_ms = 0` → `INVALID_VALUE` (must be > 0 when provided).
  - `sleep_timeout_ms` on a firmware without deep sleep → `UNSUPPORTED`.
- Valid values are **persisted immediately** and survive reboot.
- Applying a sleep config resets the inactivity timer, so the device will not
  immediately idle/sleep because of the time spent in the settings UI.

#### `SetEndpointSettingsRequest`

```proto
message SetEndpointSettingsRequest {
  message TransportValue { Transport value = 1; }

  TransportValue preferred_transport = 1;  // optional: USB or BLE
}
```

- `TRANSPORT_NONE` → `INVALID_VALUE`.
- Requesting a transport that isn't compiled in → `UNSUPPORTED`.
- The preference is persisted and applied immediately (the device switches or
  starts advertising accordingly).

### 5.3 Enums

```proto
enum ActivityState {
  ACTIVITY_STATE_ACTIVE = 0;
  ACTIVITY_STATE_IDLE = 1;
  ACTIVITY_STATE_SLEEP = 2;
}

enum Transport {
  TRANSPORT_NONE = 0;
  TRANSPORT_USB = 1;
  TRANSPORT_BLE = 2;
}

enum AddressType {
  ADDRESS_TYPE_PUBLIC = 0;
  ADDRESS_TYPE_RANDOM = 1;
}

enum SplitConnectionsStatus {
  SPLIT_CONNECTIONS_STATUS_DISCONNECTED = 0;
  SPLIT_CONNECTIONS_STATUS_SOME_CONNECTED = 1;
  SPLIT_CONNECTIONS_STATUS_ALL_CONNECTED = 2;
}

enum SettingsResult {
  SETTINGS_RESULT_OK = 0;
  SETTINGS_RESULT_INVALID_VALUE = 1;  // e.g. zero timeout, out-of-range profile index
  SETTINGS_RESULT_UNSUPPORTED = 2;    // feature not compiled into this firmware
  SETTINGS_RESULT_BLE_ERROR = 3;      // the BLE stack rejected the operation
}
```

---

## 6. Recommended client behavior

1. **On connect / opening the settings page**: send `get_settings` and render the
   full snapshot. Do not assume any defaults.
2. **On `settings_changed` notification**: re-send `get_settings` and merge the
   result into the UI. (The notification carries no payload.)
3. **On user action**: send one `set_settings` with only the changed sections,
   then update the UI from the fresh snapshot in the response. No follow-up
   `get_settings` needed.
4. **On non-OK result**: show the error per result code:
   - `INVALID_VALUE` → the input was rejected; re-validate client-side
     (timeouts > 0, profile index in range).
   - `UNSUPPORTED` → this firmware doesn't have the feature; disable the control.
   - `BLE_ERROR` → transient BLE failure; allow retry.
5. **Resilience**: if a request gets no response within a timeout (e.g. 1–2 s),
   re-send it. The protocol has no retries.
6. **Live widgets** (activity state, WPM, battery, connection state): refresh on
   notifications; optionally poll `get_settings` every 2–5 s for WPM/battery,
   since those change without emitting a notification in all cases.

---

## 7. Limits and per-board capabilities

| Item                       | Value / note                                                              |
| -------------------------- | ------------------------------------------------------------------------- |
| BLE profiles               | 4 (fixed at compile time; proto allows up to 8)                           |
| Profile label length       | max 15 characters                                                         |
| Peer address               | 6 bytes (LE address)                                                      |
| Deep sleep                 | compile-time: `sleep.supported`. `cobanpad16a`: yes. `cobanpad12b`: no    |
| Idle timeout               | any value > 0 ms (default 30 000 ms)                                      |
| Deep-sleep timeout         | any value > 0 ms (default 900 000 ms); ignored when USB powered           |
| Preferred transport        | USB and/or BLE depending on firmware (`endpoint` fields)                  |
| Battery / WPM / indicators | present only when compiled in (`diagnostics.*_supported` flags)           |
| Split diagnostics          | only on firmwares acting as split central (`diagnostics.split_supported`) |

Always drive UI availability from the `supported`/`*_supported` flags in the
snapshot rather than from board-specific knowledge.

---

## 8. Error handling and locking

- If the Studio lock is enabled (`CONFIG_ZMK_STUDIO_LOCKING`, currently **off** on
  the Coban builds) and the device is locked, secured handlers — which includes
  both `get_settings` and `set_settings` — respond with a `meta` error instead of
  a `settings` response:

  ```proto
  // zmk.meta.Response
  message Response {
    oneof response_type {
      bool no_response = 1;
      ErrorConditions simple_error = 2;  // UNLOCK_REQUIRED when locked
    }
  }
  ```

  Handle `which_subsystem == meta` with `simple_error == UNLOCK_REQUIRED` by
  prompting for unlock (see the existing core subsystem unlock flow).

- `MSG_DECODE_FAILED` / `MSG_ENCODE_FAILED` indicate a protocol-level problem
  (corrupt frame, version mismatch) — surface as a connection error.

---

## 9. Compatibility and versioning

- The schema is proto3: fields are additive and unknown fields are ignored by
  both sides. New fields may appear in future firmware without breaking older
  Studio clients (they will simply be ignored).
- Subsystem tag `9` is reserved for `settings` in `studio.proto`; request/response
  oneof choices `1`/`2` are `get_settings`/`set_settings` respectively.
- If the frontend generates TypeScript/JS bindings, generate them from
  `zmk-studio-messages` commit `ff7f991` (or later) so `settings.proto` is
  included.

---

## 10. Examples (protobuf text format)

### Fetch all settings

```text
Request {
  request_id: 1
  settings { get_settings: true }
}
```

### Enable deep sleep and set timeouts

```text
Request {
  request_id: 2
  settings {
    set_settings {
      sleep {
        enabled { value: true }
        idle_timeout_ms { value: 60000 }
        sleep_timeout_ms { value: 1800000 }
      }
    }
  }
}
```

Response (abridged):

```text
Response {
  request_response {
    request_id: 2
    settings {
      set_settings {
        result: SETTINGS_RESULT_OK
        settings {
          sleep {
            supported: true
            enabled: true
            idle_timeout_ms: 60000
            sleep_timeout_ms: 1800000
            activity_state: ACTIVITY_STATE_ACTIVE
            usb_powered: false
          }
          ...  // bluetooth, endpoint, diagnostics also present
        }
      }
    }
  }
}
```

### Select BLE profile 2 and clear the bond on profile 1

```text
Request {
  request_id: 3
  settings {
    set_settings {
      bluetooth {
        active_profile { value: 2 }
        clear_bond_profiles: 1
      }
    }
  }
}
```

### Unsolicited notification

```text
Response {
  notification {
    settings { settings_changed: true }
  }
}
```

→ Client should now send `get_settings`.
