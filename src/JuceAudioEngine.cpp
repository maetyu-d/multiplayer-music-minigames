#include "JuceAudioEngine.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTau = 6.28318530718f;
}

JuceAudioEngine::JuceAudioEngine() = default;

JuceAudioEngine::~JuceAudioEngine() { stop(); }

bool JuceAudioEngine::start() {
    juce::String error = deviceManager_.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty()) {
        return false;
    }
    deviceManager_.addAudioCallback(this);
    return true;
}

void JuceAudioEngine::stop() {
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
}

void JuceAudioEngine::triggerVoice(VoiceType type,
                                   float freq,
                                   float gain,
                                   float decayPerSecond,
                                   float durationSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.push_back({type, freq, 0.0f, gain, decayPerSecond, 0.0f, durationSeconds});
}

void JuceAudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    sampleRate_ = static_cast<float>(device->getCurrentSampleRate());
    if (sampleRate_ <= 1000.0f) {
        sampleRate_ = 48000.0f;
    }
}

void JuceAudioEngine::audioDeviceStopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.clear();
}

void JuceAudioEngine::audioDeviceIOCallbackWithContext(const float* const*,
                                                       int,
                                                       float* const* outputChannelData,
                                                       int numOutputChannels,
                                                       int numSamples,
                                                       const juce::AudioIODeviceCallbackContext&) {
    if (numOutputChannels <= 0 || outputChannelData == nullptr) {
        return;
    }

    float* left = outputChannelData[0];
    float* right = numOutputChannels > 1 ? outputChannelData[1] : nullptr;
    const float dt = 1.0f / sampleRate_;

    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < numSamples; ++i) {
        float mix = 0.0f;
        for (auto& v : voices_) {
            if (v.age > v.duration) {
                continue;
            }
            const float env = std::exp(-v.decayPerSecond * v.age);
            float s = 0.0f;
            if (v.type == VoiceType::Sine) {
                s = std::sin(v.phase);
                v.phase += kTau * v.freq * dt;
            } else if (v.type == VoiceType::Noise) {
                s = noise_(rng_);
            } else {
                const float kickFreq = std::max(40.0f, v.freq * std::exp(-5.0f * v.age));
                s = std::sin(v.phase);
                v.phase += kTau * kickFreq * dt;
            }
            mix += s * env * v.gain;
            v.age += dt;
        }
        const float out = std::clamp(mix, -0.8f, 0.8f);
        left[i] = out;
        if (right != nullptr) {
            right[i] = out;
        }
    }

    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& v) { return v.age > v.duration; }),
                  voices_.end());
}

