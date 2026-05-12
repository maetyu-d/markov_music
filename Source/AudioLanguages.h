#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>

struct LaneDefinition
{
    juce::String name;
    juce::String language;
    juce::String code;
    juce::StringPairArray params;
    float gain = 0.7f;
    bool muted = false;
};

struct AudioParameterInfo
{
    juce::String id;
    juce::String label;
    float currentValue = 0.0f;
    float defaultValue = 0.0f;
    float minimumValue = 0.0f;
    float maximumValue = 1.0f;
    float step = 0.0f;
};

class AudioProgram
{
public:
    virtual ~AudioProgram() = default;
    virtual void prepare (double sampleRate, int maxBlockSize, int numChannels) = 0;
    virtual void reset() = 0;
    virtual bool setParameter (const juce::String& parameterId, float value) = 0;
    virtual juce::Array<AudioParameterInfo> getParameters() const = 0;
    virtual void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) = 0;
    virtual void releaseResources() {}
    virtual juce::String describe() const = 0;
};

class AudioLanguageHost
{
public:
    virtual ~AudioLanguageHost() = default;
    virtual juce::String languageId() const = 0;
    virtual std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) = 0;
};

class AudioLanguageRegistry
{
public:
    AudioLanguageRegistry();

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) const;
    juce::StringArray getLanguageIds() const;

private:
    juce::OwnedArray<AudioLanguageHost> hosts;
};
