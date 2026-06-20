// Minimal C shim around WebRTC AudioProcessing (AEC3) for the call helper.
// Processes one 10 ms mono frame in place per call (rate/100 samples).
//   reverse() = far-end (caregiver audio played on the amp) — call FIRST
//   near()    = near-end (device mic) — echo removed in place
#include "modules/audio_processing/include/audio_processing.h"
#include <cstdint>
using webrtc::AudioProcessing;
using webrtc::AudioProcessingBuilder;
using webrtc::StreamConfig;

extern "C" {
void* aec_create() {
    AudioProcessing* apm = AudioProcessingBuilder().Create();
    AudioProcessing::Config c;
    c.echo_canceller.enabled = true;
    c.echo_canceller.mobile_mode = false;   // AEC3 (full, best double-talk)
    c.high_pass_filter.enabled = true;
    apm->ApplyConfig(c);
    return apm;
}
void aec_reverse(void* h, int16_t* d, int rate) {
    auto* apm = static_cast<AudioProcessing*>(h);
    StreamConfig sc(rate, 1);
    apm->ProcessReverseStream(d, sc, sc, d);
}
void aec_near(void* h, int16_t* d, int rate, int delay_ms) {
    auto* apm = static_cast<AudioProcessing*>(h);
    apm->set_stream_delay_ms(delay_ms);
    StreamConfig sc(rate, 1);
    apm->ProcessStream(d, sc, sc, d);
}
void aec_destroy(void* h){ delete static_cast<AudioProcessing*>(h); }
}
