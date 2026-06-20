// Phase 5.1 device-side MQTT subscriber — see mqtt_client.hpp for the
// rationale + topic layout.
//
// When paho-mqtt-cpp isn't installed (dev box without MQTT), HAVE_MQTT is
// 0 and this whole file compiles as a no-op stub: the Client object exists
// but does nothing, isConnected() returns false forever, all state queries
// return defaults. main.cpp can safely call into it either way; the
// existing HTTP /Live_Device polling continues to drive the pipeline.
#include "mqtt_client.hpp"

#if HAVE_MQTT
#include <mqtt/async_client.h>
#include <mqtt/callback.h>
#include <mqtt/connect_options.h>

#include <iostream>
#include <regex>

namespace mq {

namespace {

// Minimal JSON-field extractors. The state payloads are flat and small
// (1-5 fields), so a real JSON lib would be heavier than what we save.
//   `"key":123`         → 123
//   `"key":"abc"`       → "abc"
// Returns false if the key isn't present or value can't be parsed.
bool extract_int(const std::string& body, const std::string& key, int& out) {
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return false;
    if (body[pos] == 'n') return false; // null
    try { out = std::stoi(body.substr(pos)); return true; }
    catch (...) { return false; }
}

bool extract_bool(const std::string& body, const std::string& key, bool& out) {
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    if (body.find("true", pos) == pos + 1 || body.find("true", pos) == pos + 2) {
        out = true; return true;
    }
    if (body.find("false", pos) == pos + 1 || body.find("false", pos) == pos + 2) {
        out = false; return true;
    }
    return false;
}

std::string extract_string(const std::string& body, const std::string& key) {
    std::regex r("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(body, m, r)) return m[1].str();
    return {};
}

}  // namespace

struct Client::Impl : public virtual mqtt::callback {
    Client* parent;
    mqtt::async_client client;
    std::string device_id;
    std::string topic_pill;
    std::string topic_call;
    std::string topic_image;
    std::string topic_online;

    Impl(const std::string& broker_uri, const std::string& dev_id, Client* p)
      : parent(p),
        client(broker_uri, "device-" + dev_id),
        device_id(dev_id),
        topic_pill ("device/" + dev_id + "/state/pill"),
        topic_call ("device/" + dev_id + "/state/call_pending"),
        topic_image("device/" + dev_id + "/state/image_req"),
        topic_online("device/" + dev_id + "/online") {
        client.set_callback(*this);
    }

    void connect_async() {
        // Last Will and Testament — broker publishes "0" to <id>/online if
        // our TCP connection drops without a clean disconnect. Subscribers
        // (server, caregiver apps) see device-offline immediately.
        mqtt::message lwt(topic_online, "0", 1, /*retained=*/true);
        auto opts = mqtt::connect_options_builder()
                        .clean_session(false)        // keep server-side sub queue
                        .keep_alive_interval(std::chrono::seconds(30))
                        .automatic_reconnect(true)
                        // paho-mqtt-cpp 1.5+ renamed will_message() → will()
                        .will(lwt)
                        .finalize();
        try {
            client.connect(opts)->wait_for(std::chrono::seconds(5));
        } catch (const mqtt::exception& e) {
            std::cerr << "[mqtt] initial connect failed: " << e.what()
                      << " (will retry in background)\n";
        }
    }

    // Connection succeeded (or auto-reconnect re-established it). Subscribe
    // to all state topics and announce we're online.
    void connected(const std::string& /*cause*/) override {
        std::cerr << "[mqtt] connected\n";
        parent->connected_.store(true);
        // QoS 1 — must-deliver, server's retained values flush immediately
        // so we get the current pill/call/image state on connect.
        client.subscribe(topic_pill, 1);
        client.subscribe(topic_call, 1);
        client.subscribe(topic_image, 1);
        // Announce online (retained so late-joining subscribers see it).
        client.publish(mqtt::message_ptr_builder()
            .topic(topic_online).payload("1").qos(1).retained(true).finalize());
    }

    void connection_lost(const std::string& cause) override {
        std::cerr << "[mqtt] connection lost: " << cause << "\n";
        parent->connected_.store(false);
    }

    // Each broker → device message. Topic determines which state slot updates.
    void message_arrived(mqtt::const_message_ptr msg) override {
        const std::string& topic = msg->get_topic();
        const std::string& body  = msg->to_string();

        if (topic == topic_pill) {
            // body looks like: {"active_pill_alarms":[{"id":1,"pill_id":3,"name":"...","audio_url":"...","dose":2,"of":21}, ...]}
            std::vector<PillAlarm> parsed;
            // Crude — split on "{" entries inside active_pill_alarms array.
            auto arr_pos = body.find("active_pill_alarms");
            if (arr_pos != std::string::npos) {
                size_t i = body.find('[', arr_pos);
                while (i != std::string::npos) {
                    size_t obj_start = body.find('{', i + 1);
                    if (obj_start == std::string::npos) break;
                    size_t obj_end = body.find('}', obj_start);
                    if (obj_end == std::string::npos) break;
                    std::string obj = body.substr(obj_start, obj_end - obj_start + 1);
                    PillAlarm a;
                    extract_int(obj, "id", a.id);
                    extract_int(obj, "pill_id", a.pill_id);
                    extract_int(obj, "dose", a.dose);
                    extract_int(obj, "of", a.of);
                    a.name      = extract_string(obj, "name");
                    a.audio_url = extract_string(obj, "audio_url");
                    if (a.id > 0) parsed.push_back(a);
                    i = obj_end + 1;
                    if (body[i] != ',') break;
                }
            }
            std::lock_guard<std::mutex> lk(parent->state_mu_);
            parent->pill_alarms_ = std::move(parsed);
        } else if (topic == topic_call) {
            bool p = false;
            if (extract_bool(body, "pending", p)) parent->call_pending_.store(p);
            std::string who = extract_string(body, "in_call_by");
            std::lock_guard<std::mutex> lk(parent->state_mu_);
            parent->in_call_by_ = who;
        } else if (topic == topic_image) {
            int req = -1;
            if (extract_int(body, "req_id", req)) parent->image_req_id_.store(req);
        }
    }

    void delivery_complete(mqtt::delivery_token_ptr) override {}
};

Client::Client(const std::string& broker_uri, const std::string& device_id)
  : impl_(std::make_unique<Impl>(broker_uri, device_id, this)) {
    impl_->connect_async();
}

Client::~Client() {
    try {
        if (impl_ && impl_->client.is_connected()) {
            // Publish "0" to online before clean disconnect.
            impl_->client.publish(mqtt::message_ptr_builder()
                .topic(impl_->topic_online).payload("0").qos(1).retained(true).finalize())
                ->wait_for(std::chrono::seconds(1));
            impl_->client.disconnect()->wait_for(std::chrono::seconds(1));
        }
    } catch (...) {}
}

std::vector<PillAlarm> Client::activePillAlarms() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return pill_alarms_;
}

std::string Client::inCallBy() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return in_call_by_;
}

}  // namespace mq

#else  // HAVE_MQTT == 0 — no-op stub when paho isn't installed

namespace mq {
struct Client::Impl {};
Client::Client(const std::string&, const std::string&) : impl_(nullptr) {}
Client::~Client() = default;
std::vector<PillAlarm> Client::activePillAlarms() const { return {}; }
std::string            Client::inCallBy()         const { return {}; }
}  // namespace mq

#endif  // HAVE_MQTT
