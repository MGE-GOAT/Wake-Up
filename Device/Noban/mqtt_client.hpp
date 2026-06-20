// Phase 5.1 — device-side MQTT subscriber.
//
// Subscribes to the device's state topics on the broker; caches the
// most-recent value into thread-safe atomics/strings so main.cpp can
// read without polling /Live_Device over HTTP.
//
// Topics mirror server's mqtt_pub.py:
//   device/<id>/state/pill          (retained)
//   device/<id>/state/call_pending  (retained)
//   device/<id>/state/image_req     (retained)
//
// Publishes:
//   device/<id>/online              (LWT auto-publishes "0" on drop)
//
// Connection is fire-and-forget: if the broker is down, the client retries
// in background. Callers see "no state" (atomics report defaults) until
// the broker comes back. main.cpp keeps the HTTP /Live_Device poll alive
// as a safety net during the Phase 5.1 transition.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mq {

struct PillAlarm {
    int    id          = 0;     // server-side pill_schedules.id
    int    pill_id     = 0;     // app-local pill_id
    std::string name;
    std::string audio_url;      // "/Pill_Audio/<id>"
    int    dose = 0;
    int    of   = 0;
};

class Client {
public:
    // Connect to broker_uri (e.g. "tcp://192.168.1.104:1883"). device_id is
    // used to build topic names + the client ID. Non-blocking: returns
    // immediately, connect happens asynchronously in the background.
    Client(const std::string& broker_uri, const std::string& device_id);
    ~Client();

    // Latest cached state, set by the subscribe callback. Safe to call from
    // any thread.
    std::vector<PillAlarm> activePillAlarms() const;
    bool                   callPending()      const { return call_pending_.load(); }
    std::string            inCallBy()         const;
    int                    imageReqId()       const { return image_req_id_.load(); }

    // True once the client has finished its first connect handshake.
    bool isConnected() const { return connected_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Latest values published by the broker. Updated by the message callback.
    std::atomic<bool>                connected_{false};
    std::atomic<bool>                call_pending_{false};
    std::atomic<int>                 image_req_id_{-1};
    mutable std::mutex               state_mu_;
    std::vector<PillAlarm>           pill_alarms_;
    std::string                      in_call_by_;
};

}  // namespace mq
