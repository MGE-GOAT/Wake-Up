# Noban App — Setup & Build Guide

The Noban caregiver app is a Flutter (Android) project. Pubspec package name:
`elderly_care_app_1` (`pubspec.yaml:1`). Android `applicationId` /
`namespace`: `com.example.elderly_care_app_2`
(`android/app/build.gradle:12,30`). App display label: **Noban**
(`android/app/src/main/AndroidManifest.xml:18`).

This build is **offline-pinned**. Read the cautions below before running any
`flutter pub` command.

---

## 1. Prerequisites (exact, from the code)

| Tool | Version | Source |
|------|---------|--------|
| Dart SDK constraint | `^3.6.1` | `pubspec.yaml:22` |
| Flutter SDK | **3.41.2 (pinned)** — must match the offline-resolved `pubspec.lock` | build constraint |
| Flutter SDK path | `/home/mahrad/flutter` | `android/local.properties` (`flutter.sdk`) |
| Android SDK path | `/home/mahrad/Android/Sdk` | `android/local.properties` (`sdk.dir`) |
| Gradle | 8.3 | `android/gradle/wrapper/gradle-wrapper.properties` |
| Android Gradle Plugin | 8.2.1 | `android/settings.gradle` |
| Kotlin Gradle Plugin | 2.1.0 | `android/settings.gradle` |
| google-services plugin | 4.3.15 | `android/settings.gradle` |
| Java / `jvmTarget` | 1.8 | `android/app/build.gradle:20-25` |

Why the Flutter pin matters: `pubspec.lock` was resolved once, offline, against
a specific dependency graph that includes a **vendored** `record_linux`
(see §3). Re-resolving with a different Flutter/Dart toolchain can change the
lock and break the Android Dart compile.

### Key Dart dependencies (`pubspec.yaml:30-82`)
- `flutter_webrtc: ^0.12.5` — caregiver↔device audio/video calls (Janus SFU).
- `mqtt_client: ^10.5.1` — primary push channel (Phase 5.1).
- `firebase_core: ^3.11.0`, `firebase_messaging: ^15.2.2`,
  `flutter_local_notifications: ^18.0.1` — FCM push + local fallback notifications.
- `flutter_blue_plus: ^1.34.5` — BLE device provisioning.
- `cryptography: ^2.7.0`, `crypto: ^3.0.5` — E2E media crypto
  (XChaCha20-Poly1305 + HKDF-SHA256) and device key-hashing.
- `flutter_secure_storage: ^9.2.4` — account & per-device passwords in Keystore.
- `sqflite`, `shared_preferences`, `path_provider` — local persistence.
- `record: ^5.1.2`, `audioplayers: ^6.1.0`, `video_player: ^2.10.0` — voice
  recording/playback and fall-clip playback.
- `shamsi_date`, `persian_datetime_picker`, `flutter_localizations` — Jalali
  (Persian) calendar; app is RTL/`fa-IR` (`lib/main.dart:83-102`).

---

## 2. Server URL (compile-time only)

The server address is baked in at build time via `--dart-define`. There is
**no runtime/UI override** — it is read with `String.fromEnvironment` in
`lib/configs.dart:18-25`:

```dart
const String serverUrl = String.fromEnvironment(
  'SERVER_URL', defaultValue: 'http://10.0.2.2:5000');     // main API + Janus host
const String device_serverUrl = String.fromEnvironment(
  'DEVICE_SERVER_URL', defaultValue: 'http://10.0.2.2:6000'); // legacy device-AP config
```

Defaults target the Android emulator host alias `10.0.2.2`. Override per build:

```
--dart-define=SERVER_URL=http://192.168.1.104:5000
--dart-define=DEVICE_SERVER_URL=http://192.168.4.1:8080   # only for the legacy AP setup flow
```

Two more optional defines exist:
- `MQTT_HOST` — broker host; **defaults to the host parsed from `SERVER_URL`**
  if unset (`lib/services/mqtt_service.dart:31-36`).
- `MQTT_PORT` — broker port; defaults to `1883`
  (`lib/services/mqtt_service.dart:38`).

The call page derives the Janus WebSocket from `SERVER_URL`'s host:
`ws://<host>:8188`, and STUN as `stun:<host>:3478`
(`lib/call_page.dart:59-62,184`). So `SERVER_URL` must point at the box that
also runs Janus + the STUN server.

### Cleartext HTTP allow-list (IMPORTANT)
The server runs plain HTTP (no TLS). Cleartext is blocked app-wide **except**
for an explicit host allow-list in
`android/app/src/main/res/xml/network_security_config.xml` (referenced from
the manifest, line 21). Currently allow-listed: `192.168.0.154`, `noban.local`,
`192.168.1.104`, `10.253.212.246`, `127.0.0.1`, `localhost`, `10.0.2.2`.

> If you point `SERVER_URL` at a host **not** in this list, all API calls will
> fail with cleartext-not-permitted errors. Add the new host to both the
> `SERVER_URL` define **and** this XML file.

---

## 3. Offline build cautions (do NOT skip)

1. **Proxy must be OFF** during the build. The offline resolution relied on
   local caches / the Myket Maven mirror (`https://mvn.myket.ir`) configured as
   the primary repo in `android/settings.gradle`.
2. **Do not re-resolve `pubspec.lock`.** Never run `flutter pub upgrade` /
   `flutter pub get` against a different toolchain or with network changes that
   alter the graph. Use the committed lock as-is.
3. **Do not touch `vendor/record_linux`.** The `record` plugin federates a
   Linux implementation that Flutter's plugin registrant imports
   unconditionally even on Android. Published `record_linux 0.7.2` does not
   satisfy `record_platform_interface 1.6.0`, which breaks the Android Dart
   compile, so a minimal patched copy is vendored and pinned via
   `dependency_overrides` (`pubspec.yaml:84-91`). The directory exists at
   `vendor/record_linux/`.
4. Plugin-marker resolution: `settings.gradle` maps
   `com.google.gms.google-services` and `com.android.application` plugin ids to
   their real Maven modules because those markers aren't reachable from this
   network. Keep that `resolutionStrategy` block intact.

---

## 4. Android signing & SDK levels

- **minSdk / targetSdk / compileSdk**: inherited from the Flutter tool
  (`flutter.minSdkVersion` / `flutter.targetSdkVersion` /
  `flutter.compileSdkVersion`) in `android/app/build.gradle:13,34-35`.
  Note: `android/local.properties` also carries
  `flutter.compileSdkVersion=33`, but the active gradle config reads the value
  from the Flutter SDK, not this property (the explicit
  `localProperties` line is commented out at `build.gradle:14`).
- **multiDex**: enabled (`build.gradle:38`).
- **Core library desugaring**: enabled (`build.gradle:18,59`), required by
  `flutter_local_notifications`.
- **Signing**: there is **no release keystore**. The `release` build type signs
  with the **debug** keys (`build.gradle:43-47`), so `flutter run --release`
  works but the output is not Play-Store-ready. Add a real signing config
  before any store release.

### Permissions declared (`AndroidManifest.xml`)
- BLE provisioning: `BLUETOOTH_SCAN` (`neverForLocation`), `BLUETOOTH_CONNECT`,
  and legacy `BLUETOOTH` / `BLUETOOTH_ADMIN` / `ACCESS_FINE_LOCATION`
  (`maxSdkVersion=30`).
- **No** external-storage permissions (all media is written to app-private
  dirs).
- Microphone/Camera permission for calls/voice notes is requested at **runtime**
  via `permission_handler` (`lib/call_page.dart:69`), not declared as a
  `<uses-permission>` here — `flutter_webrtc` / `record` contribute the manifest
  entries through manifest merging.
- FCM notification icon meta-data: `@drawable/ic_notification`
  (`AndroidManifest.xml:40-43`).

---

## 5. Build commands

Standard offline debug APK for arm64 (the documented form):

```bash
flutter build apk --debug \
  --target-platform android-arm64 \
  --dart-define=SERVER_URL=http://<server-ip>:5000
```

Optionally add MQTT overrides (only if the broker is not on the SERVER_URL host):

```bash
  --dart-define=MQTT_HOST=<broker-ip> --dart-define=MQTT_PORT=1883
```

For local dev with hot reload against a phone over LAN
(`lib/configs.dart:9` example):

```bash
flutter run --dart-define=SERVER_URL=http://192.168.1.104:5000
```

Production-style builds can bundle the defines in a file:
`--dart-define-from-file=prod.json` (`lib/configs.dart:17`).
(No `prod.json` is committed.)

Output APK:
`build/app/outputs/flutter-apk/app-arm64-v8a-debug.apk` (debug, arm64 split).

---

## 6. Install (adb)

```bash
adb install -r build/app/outputs/flutter-apk/app-arm64-v8a-debug.apk
```

For emulator/dev against a host server, the allow-listed `10.0.2.2` alias and
`adb reverse` paths (`127.0.0.1`) are already permitted by the network-security
config.

---

## 7. Troubleshooting

- **All network calls fail / "cleartext not permitted":** the `SERVER_URL` host
  is not in `network_security_config.xml`. Add it there and rebuild.
- **Black screen on launch:** historically caused by Firebase `getToken()`
  failing on restricted networks. `main()` now guards every startup step
  (Firebase, notifications, secure-storage migration, DB) and `getToken()` is
  time-boxed to 8s (`lib/main.dart:36-70`, `lib/firebase.dart:107-114`). If you
  still see it, check logcat for `initializeDatabase failed` — the DB is the
  only hard requirement.
- **No push notifications:** if FCM is unreachable, `pushAvailable` is false and
  the 30s heartbeat poll raises **local** notifications instead
  (`lib/firebase.dart:45`, `lib/heartbeat.dart:155`). This is expected on
  networks that can't reach Google.
- **Android Dart compile error mentioning `record_linux` /
  `record_platform_interface`:** something re-resolved the lock or modified the
  vendored package. Restore `vendor/record_linux` and the committed
  `pubspec.lock`; do not run `flutter pub upgrade`.
- **Gradle can't resolve AGP / google-services plugin markers:** proxy is on, or
  the Myket mirror / `resolutionStrategy` block in `settings.gradle` was
  removed. Turn the proxy off and restore `settings.gradle`.
- **Call screen stuck on "در حال اتصال...":** `SERVER_URL`'s host must run Janus
  on `:8188` (WS) and a STUN server on `:3478` (`lib/call_page.dart:59-62,184`).
- **Calendar shows Gregorian dates in Persian script:** the
  `PersianMaterialLocalizations` delegate must be listed **first** in
  `localizationsDelegates` (`lib/main.dart:89-95`).
