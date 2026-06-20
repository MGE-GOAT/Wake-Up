// Server URL. Resolved at build time via --dart-define so the same APK can
// target the Android emulator (host alias 10.0.2.2), a real phone over LAN,
// or a hosted server later, without source edits.
//
// Defaults: `serverUrl` falls back to the emulator host alias, which is
// also what the original setup assumed. To target a real phone hitting
// the PC's server over Wi-Fi:
//
//   flutter run --dart-define=SERVER_URL=http://192.168.1.104:5000
//
// And to also point at a non-default device-side AP (e.g. when running
// against a Pi instead of the laptop dev mode):
//
//   flutter run --dart-define=DEVICE_SERVER_URL=http://192.168.4.1:8080
//
// Both endpoints honor the override; production builds can bake them in
// via `--dart-define-from-file=prod.json`.
const String serverUrl = String.fromEnvironment(
  'SERVER_URL',
  defaultValue: 'http://10.0.2.2:5000',
);
const String device_serverUrl = String.fromEnvironment(
  'DEVICE_SERVER_URL',
  defaultValue: 'http://10.0.2.2:6000',
);

// ── Phase 2 device-onboarding endpoint paths ───────────────────────────────
// Server side: see myserver.py "Phase 2 subscription endpoints" section.
//
// Auth for every subscription endpoint:
//   Headers: Username + Password (the user's app account)
//   Body:    device_id + key_hash (sha256-hex of the 32-char device password)
//            — proves the caller knows the device password without ever
//            transmitting it to the server.
const String registerDeviceEndpoint     = "/Register_Device";      // POST — first-time setup
const String subscribeDeviceEndpoint    = "/Subscribe_Device";     // POST — join existing device
const String replaceSubscriberEndpoint  = "/Replace_Subscriber";   // POST — kick + add when full
const String unsubscribeDeviceEndpoint  = "/Unsubscribe_Device";   // POST — leave a device
const String deviceSubscribersEndpoint  = "/Device_Subscribers";   // GET  — list current 1..3

// 32-char minimum enforced both client- and server-side. The server stores
// only sha256(password); the raw value lives on the device (id.txt) and
// in each subscriber's flutter_secure_storage.
const int kDevicePasswordMinChars = 32;

// global vars

