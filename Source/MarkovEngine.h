#pragma once

#include "AudioLanguages.h"
#include "ControlScripting.h"
#include <memory>
#include <vector>

struct TransitionDefinition
{
    juce::String targetState;
    double weight = 1.0;
};

struct MarkovStateDefinition
{
    juce::String id;
    juce::String section;
    double durationBeats = 16.0;
    juce::Array<TransitionDefinition> transitions;
    juce::Array<LaneDefinition> lanes;
};

struct ControlScriptDefinition
{
    juce::String language = "lua";
    juce::String code;
};

struct MarkovProject
{
    double bpm = 112.0;
    ControlScriptDefinition control;
    juce::Array<MarkovStateDefinition> states;
};

struct LaneRuntime
{
    LaneDefinition definition;
    std::unique_ptr<AudioProgram> program;
    juce::String compileError;
    float meterPeak = 0.0f;
    bool prepared = false;
};

struct LaneStatus
{
    int stateIndex = -1;
    juce::String stateId;
    juce::String laneName;
    juce::String language;
    juce::String status;
    juce::String detail;
    float meterPeak = 0.0f;
};

class MarkovEngine
{
public:
    MarkovEngine() = default;
    MarkovEngine (MarkovEngine&&) noexcept = default;
    MarkovEngine& operator= (MarkovEngine&&) noexcept = default;
    MarkovEngine (const MarkovEngine&) = delete;
    MarkovEngine& operator= (const MarkovEngine&) = delete;

    bool loadFromScript (const juce::String& script, const AudioLanguageRegistry& registry, juce::String& error);

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setPlaying (bool shouldPlay);
    bool isPlaying() const { return playing; }
    void serviceBackgroundTasks();
    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

    const MarkovProject& getProject() const { return project; }
    int getCurrentStateIndex() const { return currentStateIndex; }
    juce::String getCurrentStateName() const;
    juce::String getStatusText() const;
    juce::Array<LaneStatus> getLaneStatuses (int stateIndex) const;
    juce::Array<AudioParameterInfo> getLaneParameters (int stateIndex, const juce::String& laneName) const;
    bool setLaneParameter (int stateIndex, const juce::String& laneName, const juce::String& parameterId, float value);
    bool setTransitionWeight (int stateIndex, int transitionIndex, double weight);

    static juce::String makeDemoScript();
    static bool scriptToJson (const juce::String& script, juce::var& root, juce::String& error);
    static juce::String jsonToReadableScript (const juce::var& root);

private:
    bool parseProject (const juce::var& root, MarkovProject& result, juce::String& error) const;
    void rebuildPrograms (const AudioLanguageRegistry& registry);
    void applyStoredLaneParams (LaneRuntime& lane);
    bool isSuperColliderLane (const LaneRuntime& lane) const;
    void ensureLanePrepared (LaneRuntime& lane);
    void restartCurrentSuperColliderLanes();
    void scheduleNextState();
    void warmSuperColliderState (int stateIndex);
    void advanceState();
    int chooseWeightedTransition (const MarkovStateDefinition& state);
    int chooseControlTransition (const MarkovStateDefinition& state);
    ControlRequest makeControlRequest (const MarkovStateDefinition& state, bool includeTransitions) const;
    void runStateEnterControl();
    void applyParameterChanges (const std::vector<ControlParameterChange>& changes);
    int findStateIndex (const juce::String& id) const;
    void renderAllStateLanes (int numSamples, int outputChannels);
    void renderStateLanesToBuffer (int stateIndex, juce::AudioBuffer<float>& output, int numSamples);
    void mixRenderedState (int stateIndex,
                           juce::AudioBuffer<float>& destination,
                           int destinationStartSample,
                           int sourceStartSample,
                           int numSamples,
                           float startGain,
                           float endGain);
    void mixPreviousTail (juce::AudioBuffer<float>& destination,
                          int destinationStartSample,
                          int sourceStartSample,
                          int numSamples);

    MarkovProject project;
    std::vector<std::vector<LaneRuntime>> stateLanes;
    std::vector<int> stateVisitCounts;
    std::vector<ControlParameterChange> pendingParameterChanges;
    ControlScriptRunner controlRunner;
    juce::String lastControlError;
    juce::Random random;
    double sampleRate = 0.0;
    int maxBlockSize = 0;
    int numChannels = 2;
    int currentStateIndex = 0;
    int previousStateIndex = -1;
    int scheduledNextStateIndex = -1;
    juce::int64 samplesUntilAdvance = 0;
    int fadeSamplesRemaining = 0;
    int fadeTotalSamples = 0;
    int tailSamplesRemaining = 0;
    int tailTotalSamples = 0;
    bool playing = false;
    juce::AudioBuffer<float> laneScratch;
    std::vector<std::unique_ptr<juce::AudioBuffer<float>>> stateScratch;
};
