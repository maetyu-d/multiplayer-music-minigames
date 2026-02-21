#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <mutex>
#include <random>
#include <vector>

class JuceAudioEngine : private juce::AudioIODeviceCallback {
public:
    enum class VoiceType { Sine, Noise, Kick };

    JuceAudioEngine();
    ~JuceAudioEngine() override;

    bool start();
    void stop();

    void triggerVoice(VoiceType type, float freq, float gain, float decayPerSecond, float durationSeconds);

private:
    struct Voice {
        VoiceType type;
        float freq;
        float phase;
        float gain;
        float decayPerSecond;
        float age;
        float duration;
    };

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    juce::ScopedJuceInitialiser_GUI juceInit_;
    juce::AudioDeviceManager deviceManager_;

    std::mutex mutex_;
    std::vector<Voice> voices_;
    std::mt19937 rng_{42};
    std::uniform_real_distribution<float> noise_{-1.0f, 1.0f};
    float sampleRate_ = 48000.0f;
};

