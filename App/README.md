# Noban — App

Flutter **caregiver app** (Android) for the Noban elderly-care system. It pairs with one
or more devices (Raspberry-Pi monitors), then surfaces what they see and lets the
caregiver act:

- **Device pairing** — register a new device (BLE provisioning) or join an existing one
  (up to 3 subscribers per device).
- **Pill schedules** — create/edit per-device pill courses on a Jalali (Persian) calendar;
  track dose progress.
- **Events** — fall, voice, wake-word, and call-request events, with lazy-downloaded,
  end-to-end-encrypted fall clips.
- **Two-way calls** — audio+video calls to the device over the server's **Janus SFU**
  (`flutter_webrtc`, server-relayed, never P2P).
- **Push** — MQTT (primary) + FCM, with a 30s HTTP heartbeat and local-notification
  fallback when Google/FCM is unreachable.
- **Sleep / guest mode** — a per-device toggle that tells the device to pause monitoring
  (see [Sleep mode](#sleep--guest-mode)); the device card shows a "sleeping / not
  monitoring" banner so a caregiver is never misled.

UI is Persian, RTL, `fa-IR`. The app talks to the server (`Data/Server`) over plain HTTP +
MQTT, and to the device over BLE (provisioning) and the Janus SFU (calls).

---

## ⚠️ This app builds OFFLINE with a pinned toolchain — read first

The build is **offline-pinned**. These rules break the build (often silently) if ignored:

1. **Flutter 3.41.2 is pinned.** A newer Dart/Flutter re-resolves the dependency graph
   (notably the vendored `record_linux`) and breaks the Android Dart compile.
2. **Keep the proxy OFF** during the build. Offline resolution relied on local caches and
   the Myket Maven mirror (`https://mvn.myket.ir`) wired as the primary repo in
   `android/settings.gradle`.
3. **Do not re-resolve `pubspec.lock`.** Never run `flutter pub upgrade` (or `pub get`
   against a different toolchain/network). Use the committed lock as-is; fetch with
   `flutter pub get --offline`.
4. **Do not touch `vendor/record_linux`.** It is a minimal patched stub pinned via
   `dependency_overrides` (the published `record_linux 0.7.2` doesn't satisfy
   `record_platform_interface 1.6.0`, which breaks the Android compile). Keep the
   `resolutionStrategy` block in `settings.gradle` intact too.
5. **`SERVER_URL` is compile-time** (`--dart-define`) — there is no runtime/UI override.
   Rebuild the APK to change it.

Full toolchain/signing/troubleshooting reference: [`docs/SETUP.md`](docs/SETUP.md).

---

## Prerequisites

| Tool | Version | Source |
|------|---------|--------|
| Dart SDK constraint | `^3.6.1` | `pubspec.yaml` |
| Flutter SDK | **3.41.2 (pinned)** | matches the offline `pubspec.lock` |
| Android SDK | installed; path in `android/local.properties` | — |
| Gradle / AGP / Kotlin | 8.3 / 8.2.1 / 2.1.0 | `android/settings.gradle` |
| adb | any recent | install / device deploy |

---

## Build + install (debug, arm64)

```bash
git clone https://github.com/rayanobinorg/App.git && cd App

# 1) fetch deps from the local cache (proxy OFF, do NOT re-resolve)
flutter pub get --offline

# 2) build the debug arm64 APK with the server baked in
flutter build apk --debug --target-platform android-arm64 \
  --dart-define=SERVER_URL=http://<server-ip>:5000

# 3) install on a connected device
adb install -r build/app/outputs/flutter-apk/app-arm64-v8a-debug.apk
```

Optional `--dart-define`s:

| Define | Default | Use |
|--------|---------|-----|
| `SERVER_URL` | `http://10.0.2.2:5000` (emulator) | **required** in practice; the box that also runs Janus (`:8188`) + STUN (`:3478`) |
| `MQTT_HOST` | host parsed from `SERVER_URL` | only if the broker is on a different host |
| `MQTT_PORT` | `1883` | non-default broker port |
| `DEVICE_SERVER_URL` | `http://10.0.2.2:6000` | legacy AP-based device config flow only |

> **Cleartext HTTP allow-list:** the server runs plain HTTP, so any `SERVER_URL` host must
> be in `android/app/src/main/res/xml/network_security_config.xml` (e.g. `10.0.2.2`,
> `127.0.0.1`, `192.168.1.104`, …). Point it at a host not on that list and every API call
> fails with "cleartext not permitted" — add the host there **and** to `SERVER_URL`, then
> rebuild.

For local dev with hot reload: `flutter run --dart-define=SERVER_URL=http://<ip>:5000`.

---

## Sleep / guest mode

Each device card has a **moon button** (`Icons.bedtime`) that toggles the device's
sleep/guest mode via `POST /Set_Guest_App` (`main_page.dart`). The same mode can be set on
the device itself by **3 rapid taps** on the physical button (and a single tap wakes it).

In sleep mode the device **pauses fall detection + camera + snapshots** (privacy), while
**pills, calls, and voice keep working** and wake-word detection stays active (double-gated
"Hey Noban"). The app makes this unmistakable: the card shows a slate-grey "sleeping / not
monitoring" banner so the caregiver never assumes falls are being watched while the device
is asleep. The toggle is optimistic and confirmed by the next heartbeat/poll.

---

## Docs

- [`docs/SETUP.md`](docs/SETUP.md) — exact toolchain versions, the offline-build cautions,
  Server URL / cleartext config, Android signing & SDK levels, and troubleshooting.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — screens & services, SQLite schema,
  MQTT + FCM push routing, the Janus call flow, E2E media crypto, and the full server
  endpoint contract.

---

## Related repos

- **Server** (backend): https://github.com/rayanobinorg/Server
- **Device** (Raspberry Pi): https://github.com/rayanobinorg/Device
