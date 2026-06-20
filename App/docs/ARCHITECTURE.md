# Noban App — Architecture & Function Reference

The Noban app is the **caregiver** side of an elderly-care system. It pairs
with one or more devices (a Raspberry-Pi-based monitor), receives fall / voice /
wake-word / call events, places audio+video calls, manages pill schedules, and
handles end-to-end-encrypted media. UI is Persian, RTL, `fa-IR`
(`lib/main.dart:83-102`).

Two push channels run in parallel: **MQTT** (primary, Phase 5.1) and an HTTP
**heartbeat** poll (30s fallback) — see [Push & messaging](#push--messaging).

---

## App entry & global state

### `lib/main.dart`
`main()` initializes Firebase, `NotificationService`, runs the one-time
secure-storage password migration (`AuthStore.migrateFromPrefs`), reads
`isLoggedIn` from `SharedPreferences`, and opens the SQLite DB
(`initializeDatabase`). **Every step is guarded** so a failure can't black-screen
startup (`lib/main.dart:36-70`). `MyApp` builds a `MaterialApp` forced RTL with
Persian-first localization delegates; `home` is `MainPage` if logged in, else
`LoginPage`.

### `lib/configs.dart`
Compile-time constants. `serverUrl` / `device_serverUrl` via
`String.fromEnvironment` (no runtime override). Defines the Phase-2 endpoint
path constants (`/Register_Device`, `/Subscribe_Device`, `/Replace_Subscriber`,
`/Unsubscribe_Device`, `/Device_Subscribers`) and
`kDevicePasswordMinChars = 32`.

### `lib/theme.dart`
Centralized design tokens (`AppPalette`, `AppSpacing`, `AppRadius`) and
`appTheme`. Dark slate (`#37474F`) background, green accent.

### `lib/stream.dart`
Four app-wide singleton broadcast streams used as a lightweight event bus:
- `NotificationStream` — generic "something changed, re-fetch" tick.
- `DeviceStream` — pushes the refreshed device list to the dashboard.
- `EventStream` — new event rows.
- `VoiceStream` — carries a `device_id` when a new voice message lands.

---

## Services (`lib/services/`)

### `secure_storage.dart`
- **`AuthStore`** — caregiver **account** credentials. Password in
  Keystore/Keychain (`flutter_secure_storage`), username (email) in
  `SharedPreferences`. `creds()` returns `(username, password)` and is the
  one-stop call used by every API site. `migrateFromPrefs()` lifts a legacy
  plaintext password out of prefs.
- **`DeviceSecretStore`** — the raw 32+ char **device** password, keyed
  `device_pw::<deviceId>`, in secure storage only. Never in SQLite. This secret
  is the input to E2E media crypto and to the server key-hash.

### `device_api.dart`
HTTP wrapper for the Phase-2 subscription endpoints (`DeviceApi`). Auth model:
headers `Username` + `Password` (account creds) plus body
`device_id` + `key_hash` = `sha256_hex(devicePassword)` (`deviceKeyHash`). The
server **never** sees the raw device password. Returns sealed `ApiResult`s:
`ApiOk`, `ApiDeviceFull` (409 when a device already has 3 subscribers), or
`ApiError`. Wraps `registerDevice`, `subscribeDevice`, `replaceSubscriber`,
`unsubscribeDevice`, `deviceSubscribers`.

### `media_crypto.dart`
End-to-end media encryption, byte-compatible with the device's
`media_crypto.cpp` and server's `media_crypto.py`. Scheme:
`root_key = HKDF-SHA256(salt=device_id, ikm=sha256(devicePassword),
info="elderly-care/media-root/v1")`; per blob a random 16-byte salt + 24-byte
nonce, `blobKey = HKDF(salt, root_key, "elderly-care/media-blob/v1")`, wire =
`salt || nonce || XChaCha20-Poly1305(ct||tag)`. Key functions:
`encryptForDevice`, `decryptFromDevice`, `saveIncomingMedia` (decode+decrypt
fail-closed → write to app docs dir), and `downloadFallVideo` (POST
`/Fall_Video_App`, decrypt, write temp `.mp4`). Throws if the device password
isn't stored yet (user hasn't joined the device).

### `mqtt_service.dart`
`MqttService` singleton, one client per logged-in user. Connects on login with
auto-reconnect + exponential initial-connect backoff (cap 60s). See
[Push & messaging](#push--messaging) for topics and routing.

### `ble_provisioning.dart`
`BleProvisioningService` — `flutter_blue_plus` GATT client for device Wi-Fi
provisioning. Mirrors device-side `ble_provisioning.py` UUIDs (service
`11111111-2222-3333-4444-555555555555`; chars `wifi_scan` …`5001`, `wifi_join`
`…5002`, `status` `…5003`, `commit` `…5004`, `done` `…5005`). Advertised name
prefix `Noban-`. Exposes `scanForDevices` / `connectTo` / `scanAndConnect`,
`scanWifi`, `joinWifi(ssid, psk)`, `commitSetup(deviceId, password)`, and
`status` / `done` broadcast streams for UI progress.

---

## Screens / pages (`lib/`)

| File | Responsibility |
|------|----------------|
| `login.dart` | `LoginPage`. POST `/Login_App` (account creds in headers). On 200: opens DB, **ingests historical fall events** from `subscribed_devices` in the body (decode/decrypt media, insert as `read`), persists `isLoggedIn`/username + password to Keystore, routes to `MainPage`. |
| `signup_page.dart` | `SignUpPage`. Step 1 of sign-up: POST `/Registeration_App` (`Packet=Create_Account`) → server emails a 6-digit code → routes to `PasswordSetupPage`. |
| `password_setup_page.dart` | `PasswordSetupPage`. Step 2: POST `/Registeration_App` (`Packet=Validation_Code`) with code + chosen password → returns to `LoginPage`. |
| `main_page.dart` | `MainPage` (dashboard) + `DeviceCard`. Loads devices, urgency-sorts them (call > fall > voice), starts MQTT + heartbeat, sends FCM token to `/Config_App`, pull-to-refresh requests fresh snapshots (`/Image_Request_App`). Cards show per-type unread badges, a call-pending banner (→ `CallPage`), pill disclaimer, and a guest/sleep-mode toggle (`/Set_Guest_App`). Handles logout (stops both push channels, wipes DB). |
| `device_hub_page.dart` | `DeviceHubPage`. Tapped from a device card. Tiles: Video Call (`CallPage`), Fall Events (`DeviceDetailsPage`), Voice Messages (`VoiceInboxTab`), Pill reminder (`PillsPage`); app-bar action → `SubscribersPage`. |
| `devices.dart` | `DeviceDetailsPage` (**fall view** — fall events only) + `EventCard` + `WakeUpWordCard`. On open, marks `fall` events read. `EventCard` renders fall events, opens full-screen image, and lazily downloads + plays the fall clip (`VideoPlayerScreen`). `WakeUpWordCard` renders wake-word events. |
| `add_device_entry_page.dart` | `AddDeviceEntryPage`. Pure navigation: "connect to existing" (→ `ConnectDevicePage`, `/Subscribe_Device`) vs "set up new" (→ `SetupNewDevicePage`, `/Register_Device`). |
| `connect_device_page.dart` | `ConnectDevicePage`. Join an existing device: POST `/Subscribe_Device`. 200/201 → save locally + secure storage; 409 `device_full` → `DeviceFullPage`; 403/404 → inline error. |
| `device_full_page.dart` | `DeviceFullPage`. When a device already has 3 subscribers, pick one to kick → POST `/Replace_Subscriber` (atomic). Pops `true` on success. |
| `subscribers_page.dart` | `SubscribersPage`. Lists current 1–3 subscribers (`/Device_Subscribers`); the caller can leave (`/Unsubscribe_Device`). |
| `setup_new_device_page.dart` | `SetupNewDevicePage`. BLE onboarding wizard (instructions → scan/connect → Wi-Fi pick/join → device-id+password commit). Uses `BleProvisioningService` then POSTs `/Register_Device`. Has a manual fallback. |
| `switch_wifi_page.dart` | `SwitchWifiPage`. Change Wi-Fi of an already-registered device over BLE **without** re-registering — commits with the existing id+password from `DeviceSecretStore`. |
| `wifi_setup_page.dart` | `WifiSetupPage`. **Legacy** AP-based Wi-Fi setup talking to the device's `wifi_setup_server.py` at `http://192.168.4.1:8080` (superseded by the BLE flow). |
| `search_devices.dart` / `setup_page.dart` | Legacy device discovery/config against `device_serverUrl` (`/configurable_devices`, `/config_device`). Largely the older onboarding path; much is commented out. |
| `add_pill_page.dart` | `AddPillPage`. Add a per-device pill course (Jalali date/time, interval chips, dose count, optional audio). Converts Jalali→Gregorian→UTC, persists locally + syncs to server (`/Pill_Sync_App`). |
| `pills_page.dart` | `PillsPage`. Lists/edits a device's pill courses; fetches consumption progress from `/Pill_Status_App` (done/total); delete → `/Pill_Delete_App`. |
| `call_page.dart` | `CallPage`. WebRTC call screen (see [Calls](#calls-flutter_webrtc--janus-sfu)). |
| `voice_message_button.dart` | `VoiceMessageButton`. Hold-to-record caregiver→device voice note; POST `/Voice_To_Device` (multipart). Transient, nothing persisted. |
| `voice_inbox.dart` | `VoiceInboxTab`. Device→caregiver voice messages. POST `/Voice_Messages_App`, decrypts WAV clips (if `encrypted`), plays via `audioplayers`, tracks per-message "seen" in prefs, clears the blue badge. |
| `video_player_screen.dart` | `VideoPlayerScreen`. Plays a decrypted fall clip from a temp file; **deletes the plaintext temp file on dispose**. |
| `FullScreenImage.dart` | `FullScreenImage_static`. Pinch-zoom (`photo_view`) of a saved snapshot. |
| `settings_page.dart` | `SettingsPage`. Entry to `SwitchWifiPage` and account actions. |
| `firebase.dart` | `NotificationService` (FCM + local notifications) — see [Push & messaging](#push--messaging). |
| `firebase_options.dart` | Generated `DefaultFirebaseOptions`. |
| `db.dart` | SQLite layer — see [Local database](#local-database-dbdart). |
| `heartbeat.dart` | `HeartbeatService` — 30s fallback poll — see [Push & messaging](#push--messaging). |

---

## Local database (`db.dart`)

SQLite via `sqflite`, file `app_database.db`, **schema version 7** with ordered
migrations (`db.dart:16-101`). Tables:
- **`devices`** — `device_id` (PK), `device_password` (scrubbed to NULL in v7;
  password now lives in `DeviceSecretStore`), `device_name`, `last_image`.
- **`events`** — `event_id` (PK), `device_id`, `event_text`, `event_image`,
  `event_video`, `event_time`, `event_status` (`unread`/`read`), `event_type`
  (`fall`/`voice`/`call`/`wake`; NULL = legacy fall).
- **`pills`** — per-device course: `name`, `start_at_utc`, `interval_seconds`,
  `total_count`, `audio_path`.

Key helpers: `insertDevice` / `deleteDevice`, `insertEvent` (with `ifAbsent`
fast-path for MQTT so it never clobbers the richer heartbeat row),
`getUnreadCountsByType`, `markTypeRead`, `_pruneLocalEvents` (keeps newest 50
per device + deletes their media files), `insertPill` / `getPills` /
`deletePill` (each pill mutation fire-and-forget syncs to the server),
`updateDeviceImage`. The fall view (`getDeviceEvents`) returns only `fall`/NULL
events.

---

## Push & messaging

### MQTT (primary — `services/mqtt_service.dart`)
Broker host = `MQTT_HOST` define or the `SERVER_URL` host; port `MQTT_PORT`
(default 1883). Client id `app-<username>-<ts>`. Started from
`MainPage.initState` (`main_page.dart:54`), stopped on logout
(`main_page.dart:189`).

**Subscribed topics** (per logged-in user, re-fetched on every reconnect, and
extended via `subscribeDeviceTopics` after a new pairing):
- `user/<username>/notify`
- `device/<id>/event/fall`
- `device/<id>/event/voice`
- `device/<id>/event/wake_command`
- `device/<id>/state/pill`
- `device/<id>/state/call_pending`

**Routing** (`_route`, `mqtt_service.dart:169-207`): `event/fall` inserts a
`fall` event (`ifAbsent`) then ticks `NotificationStream`; other event/state
topics just tick `NotificationStream` so listening screens re-fetch;
`user/<username>/notify` with `{"type":"device_removed","device_id":…}` deletes
the device locally (`_handleDeviceRemoved`).

### Heartbeat fallback (`heartbeat.dart`)
`HeartbeatService` polls `/Message_Check_App` every 30s (account creds in
headers), pauses when the app is backgrounded (`WidgetsBindingObserver`),
exposes `pollNow()` for pull-to-refresh. For each message in `new_messages` it
calls `_handleNewMessage`, dispatching by `message_type`:
- `image captured` → decrypt+save snapshot, `updateDeviceImage`.
- `Event` / `Wake up word` → insert as `fall` / `wake`; clip is **not** inlined
  (server sends `has_video`; app stores a `remote` sentinel and downloads on
  tap via `/Fall_Video_App`).
- `Voice` → insert `voice` event, tick `VoiceStream`.
- `CallRequest` → insert `call` event (drives the green call banner).
- `PillAck` → notification only.
When FCM is unavailable (`!NotificationService.pushAvailable`) the handler
raises **local** system-tray notifications (color/emoji by severity/type).

### FCM + local notifications (`firebase.dart`)
`NotificationService` singleton. `initialize()` (time-boxed `getToken`, all
guarded) sets `_fcmToken`; `pushAvailable` = token obtained. Background handler
`_firebaseMessagingBackgroundHandler`. Per-type Android channels: `fall_channel`
(max), `call_channel` (high), `voice_channel` (default), `pill_channel` (low),
plus legacy `high_importance_channel`. `showLocalNotification` is used by the
heartbeat fallback; `showNotification` renders incoming FCM `RemoteMessage`s
(color from `data['color']`). The FCM token is sent to the server via
`MainPage._sendConfigToServer` → `/Config_App`.

---

## Calls (flutter_webrtc + Janus SFU)

`CallPage` (`call_page.dart`) is server-relayed (SFU), never P2P. Flow:
1. Request mic permission; POST `/Call_Start` (account creds + `device_id`) →
   server starts the device side and returns the Janus `room` number.
2. `getUserMedia` audio-only (one-sided video — caregiver speaks, sees the
   device feed).
3. Open a raw WebSocket to Janus at `ws://<SERVER_URL host>:8188`
   (subprotocol `janus-protocol`) — speaks the Janus VideoRoom API directly so
   the pinned `flutter_webrtc 0.12.x` stays put (no `janus_client` dep).
4. Janus `create` session (+25s keepalive), `attach` videoroom publisher
   handle, `join` as `publisher`, then `_publish()` the caregiver mic
   (`configure` audio:true/video:false). ICE servers: `stun:<host>:3478`.
5. On the `joined` event, `_subscribe()` to the device's feed (one feed); answer
   the subscriber offer (`_answerSubscriber`), render via `RTCVideoRenderer`,
   route audio to the loudspeaker.
6. Hangup → POST `/Call_Stop`, tear down peer connections + WS + keepalive.

Entry points: `device_hub_page.dart:64` and the dashboard call banner
(`main_page.dart:677`).

---

## End-to-end flows (summary)

- **Account creation:** `SignUpPage` → email code → `PasswordSetupPage` →
  `LoginPage` → `/Login_App` (also ingests fall-event history).
- **Pair existing device:** `AddDeviceEntryPage` → `ConnectDevicePage` →
  `/Subscribe_Device` (`key_hash`). 409 → `DeviceFullPage` → `/Replace_Subscriber`.
  On success: `DeviceSecretStore.save` + `insertDevice` +
  `MqttService.subscribeDeviceTopics`.
- **Provision new device:** `SetupNewDevicePage` → BLE (scan/connect → Wi-Fi
  join → commit id+password) → `/Register_Device`.
- **Pills:** `AddPillPage` (Jalali → UTC) → local insert + `/Pill_Sync_App`;
  `PillsPage` shows progress from `/Pill_Status_App`; delete → `/Pill_Delete_App`.
- **Fall + video:** event arrives (MQTT/heartbeat) → `fall` row with `remote`
  video sentinel → user taps in `DeviceDetailsPage` → `/Fall_Video_App` →
  decrypt → `VideoPlayerScreen` (temp file deleted after).
- **Voice (both directions):** device→caregiver via `/Voice_Messages_App`
  (`VoiceInboxTab`, decrypt+play); caregiver→device via `/Voice_To_Device`
  (`VoiceMessageButton`).
- **Wake command / call request:** `wake_command` MQTT topic /
  `Wake up word` & `CallRequest` heartbeat messages → `wake`/`call` event rows
  and notifications; `CallRequest` raises the dashboard call banner.
- **Snapshots:** `MainPage` POSTs `/Image_Request_App`; the device replies via
  the event pipeline as an `image captured` message → decrypt → `updateDeviceImage`.

---

## Server contract: how the app talks to the server

**Base URL:** `serverUrl` (compile-time). Calls use plain HTTP (allow-listed in
`network_security_config.xml`).

**Auth (almost every endpoint):** HTTP headers `Username` + `Password` (the
caregiver account creds from `AuthStore.creds()`). Device-scoped operations
additionally prove device-password knowledge with body field
`key_hash = sha256_hex(devicePassword)` — the raw device password is never sent.

### Endpoints called by the app
| Endpoint | Method | Where | Purpose |
|----------|--------|-------|---------|
| `/Login_App` | POST | `login.dart:73` | Sign in; returns subscribed devices + fall history |
| `/Registeration_App` | POST | `signup_page.dart:56`, `password_setup_page.dart:47` | Account creation (code request + validation) |
| `/Config_App` | POST | `main_page.dart:82` | Register FCM token + prefs |
| `/Message_Check_App` | POST | `heartbeat.dart:93` | 30s fallback poll for new messages |
| `/Image_Request_App` | POST | `main_page.dart:157` | Ask devices for a fresh snapshot |
| `/Register_Device` | POST | `device_api.dart` | First-time device registration |
| `/Subscribe_Device` | POST | `device_api.dart` | Join an existing device |
| `/Replace_Subscriber` | POST | `device_api.dart` | Kick a subscriber + join |
| `/Unsubscribe_Device` | POST | `device_api.dart` | Leave a device |
| `/Device_Subscribers` | GET | `device_api.dart` | List 1–3 subscribers |
| `/Call_Start`, `/Call_Stop` | POST | `call_page.dart:77,250` | Start/stop a WebRTC call (Janus room) |
| `/Fall_Video_App` | POST | `media_crypto.dart:132` | Lazy-download a fall clip |
| `/Voice_Messages_App` | POST | `voice_inbox.dart:74` | Fetch device→caregiver voice messages |
| `/Voice_To_Device` | POST (multipart) | `voice_message_button.dart:103` | Send caregiver→device voice note |
| `/Pill_Sync_App` | POST (multipart) | `db.dart:176` | Sync a pill course (+optional audio) |
| `/Pill_Status_App` | POST | `pills_page.dart:67` | Fetch dose progress (done/total) |
| `/Pill_Delete_App` | POST | `db.dart:231` | Delete a pill course |
| `/Set_Guest_App` | POST | `main_page.dart:406` | Toggle device guest/sleep mode |
| `/configurable_devices`, `/config_device` | GET/POST | `search_devices.dart:169`, `setup_page.dart:521` | **Legacy** AP-based device config (`device_serverUrl`) |

**Janus (media):** WebSocket `ws://<SERVER_URL host>:8188`
(`call_page.dart:59-62`); STUN `stun:<host>:3478`; the box behind `SERVER_URL`
must also host Janus + STUN.

**MQTT (broker):** `tcp://<MQTT_HOST or SERVER_URL host>:<MQTT_PORT|1883>`;
topics listed under [Push & messaging](#push--messaging).
