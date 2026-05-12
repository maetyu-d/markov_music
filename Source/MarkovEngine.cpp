#include "MarkovEngine.h"
#include <cmath>

namespace
{
static constexpr double transitionFadeSeconds = 0.09;
static constexpr double transitionTailSeconds = 1.8;
static constexpr float transitionTailStartGain = 0.42f;

static juce::String readString (const juce::DynamicObject* object, const juce::Identifier& key, const juce::String& fallback = {})
{
    if (object == nullptr || ! object->hasProperty (key))
        return fallback;

    return object->getProperty (key).toString();
}

static double readDouble (const juce::DynamicObject* object, const juce::Identifier& key, double fallback)
{
    if (object == nullptr || ! object->hasProperty (key))
        return fallback;

    return (double) object->getProperty (key);
}

static juce::String unquoteToken (juce::String token)
{
    token = token.trim();
    if (token.length() >= 2 && token.startsWithChar ('"') && token.endsWithChar ('"'))
        return token.substring (1, token.length() - 1);

    return token;
}

static juce::String quoteToken (const juce::String& text)
{
    return "\"" + text.replace ("\\", "\\\\").replace ("\"", "\\\"") + "\"";
}

static juce::StringArray tokenizeLine (const juce::String& line)
{
    juce::StringArray tokens;
    tokens.addTokens (line, true);
    tokens.trim();
    tokens.removeEmptyStrings();
    return tokens;
}

static void setParamsFromTokens (juce::DynamicObject& object, const juce::StringArray& tokens, int firstToken)
{
    juce::DynamicObject::Ptr params;

    for (int i = firstToken; i + 1 < tokens.size(); i += 2)
    {
        auto key = tokens[i];
        if (key == "gain")
            object.setProperty ("gain", tokens[i + 1].getDoubleValue());
        else
        {
            if (params == nullptr)
                params = new juce::DynamicObject();

            params->setProperty (key, tokens[i + 1].getDoubleValue());
        }
    }

    if (params != nullptr)
        object.setProperty ("params", juce::var (params.get()));
}

static juce::String withoutIndent (const juce::String& line)
{
    return line.startsWith ("    ") ? line.substring (4)
         : line.startsWith ("  ") ? line.substring (2)
                                  : line;
}

static int makeTransitionFadeSamples (double sampleRate)
{
    return juce::jmax (1, (int) std::round (sampleRate * transitionFadeSeconds));
}

static int makeTransitionTailSamples (double sampleRate)
{
    return juce::jmax (1, (int) std::round (sampleRate * transitionTailSeconds));
}
}

bool MarkovEngine::loadFromScript (const juce::String& script, const AudioLanguageRegistry& registry, juce::String& error)
{
    juce::var parsed;
    MarkovProject nextProject;

    if (! scriptToJson (script, parsed, error))
        return false;

    if (! parseProject (parsed, nextProject, error))
        return false;

    project = std::move (nextProject);
    rebuildPrograms (registry);
    stateVisitCounts.assign ((size_t) project.states.size(), 0);
    pendingParameterChanges.clear();
    lastControlError.clear();
    previousStateIndex = -1;
    fadeSamplesRemaining = 0;
    fadeTotalSamples = makeTransitionFadeSamples (sampleRate);
    tailSamplesRemaining = 0;
    tailTotalSamples = makeTransitionTailSamples (sampleRate);

    if (sampleRate > 0.0)
        for (auto& lanes : stateLanes)
            for (auto& lane : lanes)
                if (lane.program != nullptr)
                    ensureLanePrepared (lane);

    reset();
    return true;
}

void MarkovEngine::prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse)
{
    sampleRate = sampleRateToUse;
    maxBlockSize = maxBlockSizeToUse;
    numChannels = numChannelsToUse;
    fadeTotalSamples = makeTransitionFadeSamples (sampleRate);
    tailTotalSamples = makeTransitionTailSamples (sampleRate);
    laneScratch.setSize (juce::jmax (1, numChannels), juce::jmax (1, maxBlockSize), false, false, true);

    for (auto& lanes : stateLanes)
        for (auto& lane : lanes)
            if (lane.program != nullptr)
                ensureLanePrepared (lane);

    reset();
}

void MarkovEngine::reset()
{
    currentStateIndex = 0;
    previousStateIndex = -1;
    scheduledNextStateIndex = -1;
    fadeSamplesRemaining = 0;
    tailSamplesRemaining = 0;
    pendingParameterChanges.clear();
    lastControlError.clear();

    if (project.states.isEmpty())
    {
        samplesUntilAdvance = 0;
        return;
    }

    auto beats = project.states[currentStateIndex].durationBeats;
    samplesUntilAdvance = (juce::int64) std::round ((60.0 / project.bpm) * beats * sampleRate);

    if (! stateVisitCounts.empty())
        ++stateVisitCounts[(size_t) currentStateIndex];

    for (int stateIndex = 0; stateIndex < (int) stateLanes.size(); ++stateIndex)
        for (auto& lane : stateLanes[(size_t) stateIndex])
            if (lane.program != nullptr)
            {
                ensureLanePrepared (lane);
                if (lane.prepared)
                {
                    if (stateIndex == currentStateIndex || ! isSuperColliderLane (lane))
                    {
                        lane.program->reset();
                        applyStoredLaneParams (lane);
                    }
                }
            }

    runStateEnterControl();
    scheduleNextState();
}

void MarkovEngine::setPlaying (bool shouldPlay)
{
    if (shouldPlay && ! playing)
    {
        restartCurrentSuperColliderLanes();
        warmSuperColliderState (scheduledNextStateIndex);
    }

    playing = shouldPlay;
    if (playing && samplesUntilAdvance <= 0)
        reset();
}

void MarkovEngine::serviceBackgroundTasks()
{
    if (! playing)
        return;

    warmSuperColliderState (scheduledNextStateIndex);
}

bool MarkovEngine::isSuperColliderLane (const LaneRuntime& lane) const
{
    return lane.definition.language.equalsIgnoreCase ("supercollider");
}

void MarkovEngine::ensureLanePrepared (LaneRuntime& lane)
{
    if (lane.program == nullptr || lane.prepared || sampleRate <= 0.0)
        return;

    lane.program->prepare (sampleRate, maxBlockSize, numChannels);
    lane.program->reset();
    applyStoredLaneParams (lane);
    lane.prepared = true;
}

void MarkovEngine::restartCurrentSuperColliderLanes()
{
    if (currentStateIndex < 0 || currentStateIndex >= (int) stateLanes.size())
        return;

    for (auto& lane : stateLanes[(size_t) currentStateIndex])
        if (isSuperColliderLane (lane) && lane.program != nullptr)
        {
            if (lane.prepared)
            {
                lane.program->reset();
                applyStoredLaneParams (lane);
                continue;
            }

            ensureLanePrepared (lane);
        }
}

void MarkovEngine::scheduleNextState()
{
    scheduledNextStateIndex = -1;

    if (currentStateIndex < 0 || currentStateIndex >= project.states.size())
        return;

    auto state = project.states[currentStateIndex];
    auto nextStateIndex = chooseControlTransition (state);

    if (nextStateIndex < 0)
        nextStateIndex = chooseWeightedTransition (state);

    scheduledNextStateIndex = juce::jlimit (0, project.states.size() - 1, nextStateIndex);
}

void MarkovEngine::warmSuperColliderState (int stateIndex)
{
    if (stateIndex < 0 || stateIndex >= (int) stateLanes.size())
        return;

    for (auto& lane : stateLanes[(size_t) stateIndex])
        if (isSuperColliderLane (lane))
            ensureLanePrepared (lane);
}

void MarkovEngine::render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    buffer.clear (startSample, numSamples);

    if (! playing || currentStateIndex < 0 || currentStateIndex >= project.states.size())
        return;

    int rendered = 0;
    while (rendered < numSamples)
    {
        if (samplesUntilAdvance <= 0)
            advanceState();

        auto block = (int) juce::jmin ((juce::int64) (numSamples - rendered), samplesUntilAdvance);
        renderAllStateLanes (block, buffer.getNumChannels());

        if (fadeSamplesRemaining > 0 && previousStateIndex >= 0 && previousStateIndex < (int) stateLanes.size())
        {
            auto fadeBlock = juce::jmin (block, fadeSamplesRemaining);
            auto fadeStart = fadeTotalSamples > 0
                           ? 1.0f - (float) fadeSamplesRemaining / (float) fadeTotalSamples
                           : 1.0f;
            auto fadeEnd = fadeTotalSamples > 0
                         ? 1.0f - (float) (fadeSamplesRemaining - fadeBlock) / (float) fadeTotalSamples
                         : 1.0f;
            auto fadeInStart = std::sin (fadeStart * juce::MathConstants<float>::halfPi);
            auto fadeInEnd = std::sin (fadeEnd * juce::MathConstants<float>::halfPi);
            auto fadeOutStart = transitionTailStartGain
                               + (1.0f - transitionTailStartGain) * std::cos (fadeStart * juce::MathConstants<float>::halfPi);
            auto fadeOutEnd = transitionTailStartGain
                             + (1.0f - transitionTailStartGain) * std::cos (fadeEnd * juce::MathConstants<float>::halfPi);

            mixRenderedState (previousStateIndex, buffer, startSample + rendered, 0, fadeBlock, fadeOutStart, fadeOutEnd);
            mixRenderedState (currentStateIndex, buffer, startSample + rendered, 0, fadeBlock, fadeInStart, fadeInEnd);

            fadeSamplesRemaining -= fadeBlock;

            if (fadeBlock < block)
            {
                mixRenderedState (currentStateIndex, buffer, startSample + rendered + fadeBlock, fadeBlock, block - fadeBlock, 1.0f, 1.0f);
                mixPreviousTail (buffer, startSample + rendered + fadeBlock, fadeBlock, block - fadeBlock);
            }
        }
        else
        {
            mixRenderedState (currentStateIndex, buffer, startSample + rendered, 0, block, 1.0f, 1.0f);
            mixPreviousTail (buffer, startSample + rendered, 0, block);
        }

        rendered += block;
        samplesUntilAdvance -= block;
    }
}

void MarkovEngine::renderAllStateLanes (int numSamples, int outputChannels)
{
    if (numSamples <= 0)
        return;

    while (stateScratch.size() < stateLanes.size())
        stateScratch.push_back (std::make_unique<juce::AudioBuffer<float>>());

    for (int stateIndex = 0; stateIndex < (int) stateLanes.size(); ++stateIndex)
    {
        auto& scratch = *stateScratch[(size_t) stateIndex];
        if (scratch.getNumChannels() != outputChannels || scratch.getNumSamples() < numSamples)
            scratch.setSize (outputChannels, numSamples, false, false, true);

        scratch.clear (0, numSamples);

        if (stateIndex != currentStateIndex && stateIndex != previousStateIndex)
        {
            bool hasOnlyDeferredLanes = true;
            for (auto& lane : stateLanes[(size_t) stateIndex])
                if (! isSuperColliderLane (lane))
                    hasOnlyDeferredLanes = false;

            if (hasOnlyDeferredLanes)
                continue;
        }

        renderStateLanesToBuffer (stateIndex, scratch, numSamples);
    }
}

void MarkovEngine::renderStateLanesToBuffer (int stateIndex, juce::AudioBuffer<float>& output, int numSamples)
{
    if (stateIndex < 0 || stateIndex >= (int) stateLanes.size() || numSamples <= 0)
        return;

    if (laneScratch.getNumChannels() != juce::jmax (1, numChannels)
        || laneScratch.getNumSamples() < numSamples)
        laneScratch.setSize (juce::jmax (1, numChannels), numSamples, false, false, true);

    for (auto& lane : stateLanes[(size_t) stateIndex])
    {
        if (lane.program == nullptr)
            continue;

        if (isSuperColliderLane (lane))
        {
            if (stateIndex != currentStateIndex && stateIndex != previousStateIndex)
                continue;

            ensureLanePrepared (lane);
        }

        if (! lane.prepared)
            continue;

        laneScratch.clear (0, numSamples);
        lane.program->render (laneScratch, 0, numSamples);

        auto peak = laneScratch.getMagnitude (0, numSamples);
        lane.meterPeak = juce::jmax (peak, lane.meterPeak * 0.86f);

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
        {
            auto sourceChannel = channel % laneScratch.getNumChannels();
            output.addFrom (channel, 0, laneScratch, sourceChannel, 0, numSamples);
        }
    }
}

void MarkovEngine::mixRenderedState (int stateIndex,
                                     juce::AudioBuffer<float>& destination,
                                     int destinationStartSample,
                                     int sourceStartSample,
                                     int numSamples,
                                     float startGain,
                                     float endGain)
{
    if (stateIndex < 0 || stateIndex >= (int) stateScratch.size() || numSamples <= 0)
        return;

    auto* source = stateScratch[(size_t) stateIndex].get();
    if (source == nullptr)
        return;

    for (int channel = 0; channel < destination.getNumChannels(); ++channel)
    {
        auto sourceChannel = channel % source->getNumChannels();
        destination.addFromWithRamp (channel,
                                     destinationStartSample,
                                     source->getReadPointer (sourceChannel, sourceStartSample),
                                     numSamples,
                                     startGain,
                                     endGain);
    }
}

void MarkovEngine::mixPreviousTail (juce::AudioBuffer<float>& destination,
                                    int destinationStartSample,
                                    int sourceStartSample,
                                    int numSamples)
{
    if (previousStateIndex < 0 || tailSamplesRemaining <= 0 || numSamples <= 0)
        return;

    auto tailBlock = juce::jmin (numSamples, tailSamplesRemaining);
    auto startGain = transitionTailStartGain * ((float) tailSamplesRemaining / (float) juce::jmax (1, tailTotalSamples));
    auto endGain = transitionTailStartGain * ((float) (tailSamplesRemaining - tailBlock) / (float) juce::jmax (1, tailTotalSamples));

    mixRenderedState (previousStateIndex,
                      destination,
                      destinationStartSample,
                      sourceStartSample,
                      tailBlock,
                      startGain,
                      endGain);

    tailSamplesRemaining -= tailBlock;
    if (tailSamplesRemaining <= 0 && fadeSamplesRemaining <= 0)
        previousStateIndex = -1;
}

juce::String MarkovEngine::getCurrentStateName() const
{
    if (currentStateIndex < 0 || currentStateIndex >= project.states.size())
        return "No state";

    auto state = project.states[currentStateIndex];
    return state.section + " / " + state.id;
}

juce::String MarkovEngine::getStatusText() const
{
    juce::StringArray lines;
    lines.add ("Tempo: " + juce::String (project.bpm, 1) + " BPM");
    lines.add ("Song control: " + project.control.language);
    if (lastControlError.isNotEmpty())
        lines.add ("Song control note: " + lastControlError);
    lines.add ("Now: " + getCurrentStateName());
    lines.add ("Parts: " + juce::String (project.states.size()));

    if (currentStateIndex < 0 || currentStateIndex >= (int) stateLanes.size())
        return lines.joinIntoString ("\n");

    for (auto& lane : stateLanes[(size_t) currentStateIndex])
    {
        auto text = lane.definition.name + " [" + lane.definition.language + "]";
        if (lane.program != nullptr)
            text += " - ready - " + lane.program->describe() + " level " + juce::String (lane.meterPeak, 2);
        else if (lane.compileError.isNotEmpty())
            text += " - needs attention - " + lane.compileError;
        lines.add (text);
    }

    return lines.joinIntoString ("\n");
}

juce::Array<LaneStatus> MarkovEngine::getLaneStatuses (int stateIndex) const
{
    juce::Array<LaneStatus> result;

    if (stateIndex < 0 || stateIndex >= (int) stateLanes.size() || stateIndex >= project.states.size())
        return result;

    auto state = project.states[stateIndex];
    for (auto& lane : stateLanes[(size_t) stateIndex])
    {
        LaneStatus status;
        status.stateIndex = stateIndex;
        status.stateId = state.id;
        status.laneName = lane.definition.name;
        status.language = lane.definition.language;
        status.meterPeak = lane.meterPeak;

        if (lane.program != nullptr)
        {
            status.status = "live";
            status.detail = lane.program->describe();
        }
        else if (lane.compileError.isNotEmpty())
        {
            status.status = lane.definition.language == "rtcmix" || lane.definition.language == "supercollider"
                          ? "placeholder"
                          : "error";
            status.detail = lane.compileError;
        }
        else
        {
            status.status = "silent";
        }

        result.add (status);
    }

    return result;
}

juce::Array<AudioParameterInfo> MarkovEngine::getLaneParameters (int stateIndex, const juce::String& laneName) const
{
    if (stateIndex < 0 || stateIndex >= (int) stateLanes.size())
        return {};

    for (auto& lane : stateLanes[(size_t) stateIndex])
        if (lane.definition.name == laneName && lane.program != nullptr)
            return lane.program->getParameters();

    return {};
}

bool MarkovEngine::setLaneParameter (int stateIndex, const juce::String& laneName, const juce::String& parameterId, float value)
{
    if (stateIndex < 0 || stateIndex >= (int) stateLanes.size())
        return false;

    for (auto& lane : stateLanes[(size_t) stateIndex])
    {
        if (lane.definition.name == laneName && lane.program != nullptr)
        {
            auto changed = lane.program->setParameter (parameterId, value);
            if (changed && parameterId.equalsIgnoreCase ("gain"))
                lane.definition.gain = value;
            if (changed)
                lane.definition.params.set (parameterId, juce::String (value, 6));
            return changed;
        }
    }

    return false;
}

bool MarkovEngine::setTransitionWeight (int stateIndex, int transitionIndex, double weight)
{
    if (stateIndex < 0 || stateIndex >= project.states.size())
        return false;

    auto& transitions = project.states.getReference (stateIndex).transitions;
    if (transitionIndex < 0 || transitionIndex >= transitions.size())
        return false;

    transitions.getReference (transitionIndex).weight = juce::jmax (0.0, weight);
    return true;
}

bool MarkovEngine::parseProject (const juce::var& root, MarkovProject& result, juce::String& error) const
{
    auto* object = root.getDynamicObject();
    if (object == nullptr)
    {
        error = "Start with a tempo and at least one part.";
        return false;
    }

    result.bpm = juce::jlimit (20.0, 300.0, readDouble (object, "bpm", 112.0));

    if (auto controlVar = object->getProperty ("control"); controlVar.isObject())
    {
        auto* controlObject = controlVar.getDynamicObject();
        result.control.language = readString (controlObject, "language", "lua").toLowerCase();
        result.control.code = readString (controlObject, "code");

        if (result.control.language != "lua" && result.control.language != "python")
        {
            error = "Control can use Lua or Python.";
            return false;
        }
    }

    auto statesVar = object->getProperty ("states");

    if (! statesVar.isArray() || statesVar.getArray()->isEmpty())
    {
        error = "Add at least one part to the song.";
        return false;
    }

    for (auto& stateVar : *statesVar.getArray())
    {
        auto* stateObject = stateVar.getDynamicObject();
        if (stateObject == nullptr)
            continue;

        MarkovStateDefinition state;
        state.id = readString (stateObject, "id", "state" + juce::String (result.states.size() + 1));
        state.section = readString (stateObject, "section", "section");
        state.durationBeats = juce::jmax (0.25, readDouble (stateObject, "durationBeats", 16.0));

        if (auto transitions = stateObject->getProperty ("transitions"); transitions.isArray())
        {
            for (auto& transitionVar : *transitions.getArray())
            {
                auto* transitionObject = transitionVar.getDynamicObject();
                if (transitionObject == nullptr)
                    continue;

                TransitionDefinition transition;
                transition.targetState = readString (transitionObject, "to");
                transition.weight = juce::jmax (0.0, readDouble (transitionObject, "weight", 1.0));

                if (transition.targetState.isNotEmpty() && transition.weight > 0.0)
                    state.transitions.add (transition);
            }
        }

        if (auto lanesVar = stateObject->getProperty ("lanes"); lanesVar.isArray())
        {
            for (auto& laneVar : *lanesVar.getArray())
            {
                auto* laneObject = laneVar.getDynamicObject();
                if (laneObject == nullptr)
                    continue;

                LaneDefinition lane;
                lane.name = readString (laneObject, "name", "lane" + juce::String (state.lanes.size() + 1));
                lane.language = readString (laneObject, "language", "minitone").toLowerCase();
                lane.code = readString (laneObject, "code");
                lane.gain = (float) juce::jlimit (0.0, 1.5, readDouble (laneObject, "gain", 0.7));
                lane.muted = (bool) laneObject->getProperty ("muted");

                if (auto paramsVar = laneObject->getProperty ("params"); paramsVar.isObject())
                {
                    auto* paramsObject = paramsVar.getDynamicObject();
                    for (auto& property : paramsObject->getProperties())
                        lane.params.set (property.name.toString(), property.value.toString());
                }

                state.lanes.add (lane);
            }
        }

        result.states.add (state);
    }

    for (auto& state : result.states)
    {
        for (auto& transition : state.transitions)
        {
            auto found = false;
            for (auto& target : result.states)
                found = found || target.id == transition.targetState;

            if (! found)
            {
                error = "The part '" + state.id + "' points to '" + transition.targetState
                      + "', but that part does not exist yet.";
                return false;
            }
        }
    }

    return ! result.states.isEmpty();
}

void MarkovEngine::rebuildPrograms (const AudioLanguageRegistry& registry)
{
    stateLanes.clear();
    stateScratch.clear();
    stateLanes.reserve ((size_t) project.states.size());
    stateScratch.reserve ((size_t) project.states.size());

    for (auto& state : project.states)
    {
        std::vector<LaneRuntime> lanes;
        lanes.reserve ((size_t) state.lanes.size());

        for (auto& laneDefinition : state.lanes)
        {
            LaneRuntime runtime;
            runtime.definition = laneDefinition;
            runtime.program = registry.compile (laneDefinition, runtime.compileError);
            applyStoredLaneParams (runtime);
            lanes.push_back (std::move (runtime));
        }

        stateLanes.push_back (std::move (lanes));
        stateScratch.push_back (std::make_unique<juce::AudioBuffer<float>>());
    }
}

void MarkovEngine::applyStoredLaneParams (LaneRuntime& lane)
{
    if (lane.program == nullptr)
        return;

    for (auto& key : lane.definition.params.getAllKeys())
        lane.program->setParameter (key, lane.definition.params[key].getFloatValue());
}

void MarkovEngine::advanceState()
{
    if (project.states.isEmpty())
        return;

    auto nextStateIndex = scheduledNextStateIndex;
    if (nextStateIndex < 0)
    {
        auto state = project.states[currentStateIndex];
        nextStateIndex = chooseControlTransition (state);

        if (nextStateIndex < 0)
            nextStateIndex = chooseWeightedTransition (state);
    }

    previousStateIndex = currentStateIndex;
    currentStateIndex = juce::jlimit (0, project.states.size() - 1, nextStateIndex);
    scheduledNextStateIndex = -1;
    fadeSamplesRemaining = previousStateIndex != currentStateIndex ? fadeTotalSamples : 0;
    tailSamplesRemaining = previousStateIndex != currentStateIndex ? tailTotalSamples : 0;
    samplesUntilAdvance = (juce::int64) std::round ((60.0 / project.bpm) * project.states[currentStateIndex].durationBeats * sampleRate);

    if (currentStateIndex < (int) stateVisitCounts.size())
        ++stateVisitCounts[(size_t) currentStateIndex];

    applyParameterChanges (pendingParameterChanges);
    pendingParameterChanges.clear();
    runStateEnterControl();
    scheduleNextState();
}

int MarkovEngine::chooseWeightedTransition (const MarkovStateDefinition& state)
{
    auto totalWeight = 0.0;
    for (auto& transition : state.transitions)
        totalWeight += transition.weight;

    if (totalWeight <= 0.0)
        return (currentStateIndex + 1) % project.states.size();

    auto pick = random.nextDouble() * totalWeight;
    for (auto& transition : state.transitions)
    {
        pick -= transition.weight;
        if (pick <= 0.0)
            return findStateIndex (transition.targetState);
    }

    return (currentStateIndex + 1) % project.states.size();
}

int MarkovEngine::chooseControlTransition (const MarkovStateDefinition& state)
{
    lastControlError.clear();
    pendingParameterChanges.clear();

    if (project.control.code.trim().isEmpty() || state.transitions.isEmpty())
        return -1;

    auto request = makeControlRequest (state, true);
    auto result = controlRunner.chooseNextState (request);
    lastControlError = result.error;
    pendingParameterChanges = std::move (result.parameterChanges);

    if (result.nextStateId.isNotEmpty())
        return findStateIndex (result.nextStateId);

    return -1;
}

ControlRequest MarkovEngine::makeControlRequest (const MarkovStateDefinition& state, bool includeTransitions) const
{
    ControlRequest request;
    request.language = project.control.language;
    request.code = project.control.code;
    request.currentStateId = state.id;
    request.currentSection = state.section;
    request.visitCount = currentStateIndex < (int) stateVisitCounts.size() ? stateVisitCounts[(size_t) currentStateIndex] : 0;

    if (! includeTransitions)
        return request;

    for (auto& transition : state.transitions)
    {
        auto targetIndex = findStateIndex (transition.targetState);
        if (targetIndex >= 0 && targetIndex < project.states.size())
        {
            auto target = project.states[targetIndex];
            ControlChoice choice;
            choice.id = target.id;
            choice.section = target.section;
            choice.weight = transition.weight;
            request.choices.add (choice);
        }
    }

    return request;
}

void MarkovEngine::runStateEnterControl()
{
    if (project.control.code.trim().isEmpty()
        || currentStateIndex < 0
        || currentStateIndex >= project.states.size())
        return;

    auto request = makeControlRequest (project.states[currentStateIndex], false);
    auto result = controlRunner.enterState (request);

    if (result.error.isNotEmpty())
        lastControlError = result.error;

    applyParameterChanges (result.parameterChanges);
}

void MarkovEngine::applyParameterChanges (const std::vector<ControlParameterChange>& changes)
{
    if (changes.empty() || currentStateIndex < 0 || currentStateIndex >= (int) stateLanes.size())
        return;

    auto& lanes = stateLanes[(size_t) currentStateIndex];

    for (auto& change : changes)
    {
        for (auto& lane : lanes)
        {
            if (lane.definition.name == change.laneName && lane.program != nullptr)
            {
                auto changed = lane.program->setParameter (change.parameterId, change.value);
                if (changed)
                {
                    if (change.parameterId.equalsIgnoreCase ("gain"))
                        lane.definition.gain = change.value;
                    lane.definition.params.set (change.parameterId, juce::String (change.value, 6));
                }
            }
        }
    }
}

int MarkovEngine::findStateIndex (const juce::String& id) const
{
    for (int i = 0; i < project.states.size(); ++i)
        if (project.states[i].id == id)
            return i;

    return 0;
}

juce::String MarkovEngine::makeDemoScript()
{
    return R"TEXT(tempo 132

state glass_room "Glass Room" 16
  next acid_turn 42
  next blue_fold 25
  next metal_break 18
  next sleep_drift 15
  lane "Glass room" supercollider gain 0.78
    param freq 69
    param amp 0.30
    param cutoff 2200
    param duration 8
    code:
      ({
        var tick = Impulse.ar(8);
        var soft = Impulse.ar(16);
        var notes = Demand.ar(tick, 0, Dseq([0, 0, 3, 7, 10, 7, 3, -2], inf));
        var melody = Demand.ar(Impulse.ar(2), 0, Dseq([12, 10, 7, 3, 5, 3, 0, -2], inf));
        var kick = SinOsc.ar(46 + (Decay2.ar(tick, 0.001, 0.07) * 95)) * Decay2.ar(tick, 0.002, 0.11) * 0.72;
        var rim = BPF.ar(WhiteNoise.ar, 2100, 0.5) * Decay2.ar(Impulse.ar(4, 0.5), 0.002, 0.045) * 0.20;
        var hat = HPF.ar(WhiteNoise.ar, 6500) * Decay2.ar(soft, 0.001, 0.018) * 0.08;
        var bass = RLPF.ar(Pulse.ar(freq * (2 ** (notes / 12)), 0.38), cutoff, 0.22) * Decay2.ar(tick, 0.004, 0.18) * 0.36;
        var bell = SinOsc.ar(freq * 8 * (2 ** (melody / 12))) * Decay2.ar(Impulse.ar(2), 0.01, 0.55) * 0.13;
        Pan2.ar((kick + rim + hat + bass + bell) * amp, SinOsc.kr(0.05) * 0.18)
      }.value)
    endcode

state acid_turn "Acid Turn" 12
  next metal_break 45
  next glass_room 25
  next blue_fold 20
  next sleep_drift 10
  lane "Acid turn" supercollider gain 0.80
    param freq 82
    param amp 0.31
    param cutoff 1900
    param duration 8
    code:
      ({
        var kickTrig = Impulse.ar(8);
        var snareTrig = Impulse.ar(4, 0.5);
        var hatTrig = Impulse.ar(16);
        var step = Demand.ar(kickTrig, 0, Dseq([0, 0, 12, 3, 7, 5, 3, -2], inf));
        var accent = Demand.ar(kickTrig, 0, Dseq([1, 0.35, 0.8, 0.45, 1, 0.5, 0.7, 0.4], inf));
        var kick = SinOsc.ar(51 + (Decay2.ar(kickTrig, 0.001, 0.06) * 115)) * Decay2.ar(kickTrig, 0.001, 0.095) * 0.78;
        var snare = BPF.ar(WhiteNoise.ar, 1700, 0.5) * Decay2.ar(snareTrig, 0.003, 0.07) * 0.30;
        var hats = HPF.ar(WhiteNoise.ar, 7200) * Decay2.ar(hatTrig, 0.001, 0.014) * 0.075;
        var acid = RLPF.ar(Pulse.ar(freq * (2 ** (step / 12)), 0.42),
                           cutoff + (Decay2.ar(kickTrig, 0.002, 0.15) * 2800 * accent),
                           0.15) * 0.34;
        Pan2.ar((kick + snare + hats + acid) * amp, SinOsc.kr(0.08) * 0.12)
      }.value)
    endcode

state metal_break "Metal Break" 12
  next acid_turn 38
  next blue_fold 27
  next sleep_drift 20
  next glass_room 15
  lane "Metal break" supercollider gain 0.76
    param freq 104
    param amp 0.29
    param cutoff 3000
    param duration 8
    code:
      ({
        var kickTrig = TDuty.ar(Dseq([0.5, 0.25, 0.25, 0.5, 0.75, 0.25], inf), 0, 1);
        var hitTrig = Impulse.ar(6);
        var hatTrig = Impulse.ar(18);
        var step = Demand.ar(kickTrig, 0, Dseq([0, 7, 10, 3, 0, -2, 3, 7], inf));
        var tune = Demand.ar(Impulse.ar(2), 0, Dseq([0, 3, 10, 7, 5, 3, -2, 0], inf));
        var kick = SinOsc.ar(55 + (Decay2.ar(kickTrig, 0.001, 0.07) * 135)) * Decay2.ar(kickTrig, 0.002, 0.105) * 0.72;
        var knock = (BPF.ar(WhiteNoise.ar, 950, 0.42) + SinOsc.ar(180)) * Decay2.ar(hitTrig, 0.002, 0.055) * 0.30;
        var hats = HPF.ar(WhiteNoise.ar, 7600) * Decay2.ar(hatTrig, 0.001, 0.012) * 0.07;
        var bass = RLPF.ar(Saw.ar(freq * (2 ** (step / 12))), cutoff, 0.20) * Decay2.ar(kickTrig, 0.004, 0.18) * 0.32;
        var lead = SinOsc.ar(freq * 4 * (2 ** (tune / 12))) * Decay2.ar(Impulse.ar(2), 0.008, 0.42) * 0.11;
        Pan2.ar((kick + knock + hats + bass + lead) * amp, SinOsc.kr(0.11) * 0.16)
      }.value)
    endcode

state blue_fold "Blue Fold" 16
  next sleep_drift 36
  next acid_turn 28
  next glass_room 24
  next metal_break 12
  lane "Blue fold" supercollider gain 0.74
    param freq 92
    param amp 0.30
    param cutoff 2100
    param duration 8
    code:
      ({
        var kickTrig = Impulse.ar(8);
        var clapTrig = Impulse.ar(4, 0.5);
        var hatTrig = Impulse.ar(12);
        var step = Demand.ar(kickTrig, 0, Dseq([0, 0, 5, 3, 10, 7, 3, -2], inf));
        var fall = Demand.ar(Impulse.ar(1.5), 0, Dseq([15, 14, 10, 7, 3, 0, -2, 3], inf));
        var kick = SinOsc.ar(45 + (Decay2.ar(kickTrig, 0.001, 0.06) * 105)) * Decay2.ar(kickTrig, 0.002, 0.09) * 0.68;
        var clap = BPF.ar(WhiteNoise.ar, 1450, 0.55) * Decay2.ar(clapTrig, 0.004, 0.055) * 0.24;
        var hats = HPF.ar(WhiteNoise.ar, 5700) * Decay2.ar(hatTrig, 0.001, 0.018) * 0.075;
        var bass = RLPF.ar(Pulse.ar(freq * (2 ** (step / 12)), 0.35), cutoff, 0.22) * Decay2.ar(kickTrig, 0.004, 0.19) * 0.31;
        var glass = SinOsc.ar(freq * 6 * (2 ** (fall / 12))) * Decay2.ar(Impulse.ar(1.5), 0.012, 0.75) * 0.12;
        Pan2.ar((kick + clap + hats + bass + glass) * amp, SinOsc.kr(0.04) * 0.22)
      }.value)
    endcode

state sleep_drift "Sleep Drift" 20
  next glass_room 44
  next blue_fold 26
  next acid_turn 18
  next metal_break 12
  lane "Sleep drift" supercollider gain 0.66
    param freq 69
    param amp 0.27
    param cutoff 1600
    param duration 8
    code:
      ({
        var tick = Impulse.ar(6);
        var ghost = Impulse.ar(11);
        var chord = RLPF.ar(Saw.ar([freq * 0.5, freq * 0.75, freq * 1.5]) * 0.08, cutoff, 0.28).sum;
        var pulse = SinOsc.ar(freq * 0.5) * 0.13;
        var dust = HPF.ar(WhiteNoise.ar, 5200) * Decay2.ar(tick, 0.001, 0.028) * 0.07;
        var ghostHat = HPF.ar(WhiteNoise.ar, 8200) * Decay2.ar(ghost, 0.001, 0.014) * 0.04;
        var tune = Demand.ar(Impulse.ar(1.25), 0, Dseq([0, 3, 7, 10, 14, 10, 7, 3], inf));
        var chime = SinOsc.ar(freq * 9 * (2 ** (tune / 12))) * Decay2.ar(Impulse.ar(1.25), 0.015, 0.9) * 0.10;
        Pan2.ar((pulse + chord + dust + ghostHat + chime) * amp, SinOsc.kr(0.03) * 0.25)
      }.value)
    endcode
)TEXT";
}

bool MarkovEngine::scriptToJson (const juce::String& script, juce::var& root, juce::String& error)
{
    root = juce::JSON::parse (script);
    if (root.isObject())
        return true;

    auto* project = new juce::DynamicObject();
    project->setProperty ("bpm", 112.0);

    auto* control = new juce::DynamicObject();
    control->setProperty ("language", "lua");
    control->setProperty ("code", "");
    project->setProperty ("control", juce::var (control));

    juce::Array<juce::var> states;
    juce::DynamicObject* currentState = nullptr;
    juce::DynamicObject* currentLane = nullptr;
    juce::String blockTarget;
    juce::String blockText;

    auto finishCodeBlock = [&]()
    {
        if (blockTarget == "lane" && currentLane != nullptr)
            currentLane->setProperty ("code", blockText.trimEnd());
        else if (blockTarget == "control")
            control->setProperty ("code", blockText.trimEnd());

        blockTarget.clear();
        blockText.clear();
    };

    auto lines = juce::StringArray::fromLines (script);
    for (auto rawLine : lines)
    {
        auto line = rawLine.trim();
        if (line.isEmpty() || line.startsWithChar ('#'))
            continue;

        if (blockTarget.isNotEmpty())
        {
            if (line == "endcode")
                finishCodeBlock();
            else
                blockText += withoutIndent (rawLine) + "\n";

            continue;
        }

        if (line == "code:")
        {
            if (currentLane == nullptr)
            {
                error = "Put sound code inside an instrument.";
                return false;
            }

            blockTarget = "lane";
            continue;
        }

        auto tokens = tokenizeLine (line);
        if (tokens.isEmpty())
            continue;

        auto command = tokens[0].toLowerCase();

        if (command == "tempo" && tokens.size() >= 2)
        {
            project->setProperty ("bpm", tokens[1].getDoubleValue());
        }
        else if (command == "control" && tokens.size() >= 2)
        {
            control->setProperty ("language", tokens[1].toLowerCase());
            blockTarget = "control";
        }
        else if (command == "state" && tokens.size() >= 4)
        {
            currentState = new juce::DynamicObject();
            currentLane = nullptr;
            currentState->setProperty ("id", unquoteToken (tokens[1]));
            currentState->setProperty ("section", unquoteToken (tokens[2]));
            currentState->setProperty ("durationBeats", tokens[3].getDoubleValue());
            currentState->setProperty ("transitions", juce::Array<juce::var>());
            currentState->setProperty ("lanes", juce::Array<juce::var>());
            states.add (currentState);
        }
        else if (command == "next" && tokens.size() >= 3 && currentState != nullptr)
        {
            auto transitionsVar = currentState->getProperty ("transitions");
            auto* transitions = transitionsVar.getArray();
            auto* transition = new juce::DynamicObject();
            transition->setProperty ("to", unquoteToken (tokens[1]));
            transition->setProperty ("weight", tokens[2].getDoubleValue() / 100.0);
            transitions->add (transition);
            currentState->setProperty ("transitions", transitionsVar);
        }
        else if (command == "lane" && tokens.size() >= 4 && currentState != nullptr)
        {
            auto lanesVar = currentState->getProperty ("lanes");
            auto* lanes = lanesVar.getArray();
            currentLane = new juce::DynamicObject();
            currentLane->setProperty ("name", unquoteToken (tokens[1]));
            currentLane->setProperty ("language", tokens[2].toLowerCase());
            currentLane->setProperty ("code", "");
            setParamsFromTokens (*currentLane, tokens, 3);
            lanes->add (currentLane);
            currentState->setProperty ("lanes", lanesVar);
        }
        else if (command == "code" && currentLane != nullptr)
        {
            currentLane->setProperty ("code", line.fromFirstOccurrenceOf ("code", false, false).trimStart());
        }
        else if (command == "param" && tokens.size() >= 3 && currentLane != nullptr)
        {
            juce::DynamicObject::Ptr params = currentLane->getProperty ("params").isObject()
                                            ? currentLane->getProperty ("params").getDynamicObject()
                                            : new juce::DynamicObject();
            params->setProperty (tokens[1], tokens[2].getDoubleValue());
            currentLane->setProperty ("params", juce::var (params.get()));
        }
        else
        {
            error = "I could not understand this line:\n" + rawLine;
            return false;
        }
    }

    if (blockTarget.isNotEmpty())
        finishCodeBlock();

    project->setProperty ("states", states);
    root = juce::var (project);
    return true;
}

juce::String MarkovEngine::jsonToReadableScript (const juce::var& root)
{
    auto* project = root.getDynamicObject();
    if (project == nullptr)
        return {};

    juce::StringArray lines;
    lines.add ("tempo " + juce::String (readDouble (project, "bpm", 112.0), 0));

    if (auto controlVar = project->getProperty ("control"); controlVar.isObject())
    {
        auto* control = controlVar.getDynamicObject();
        auto controlCode = readString (control, "code");
        if (controlCode.trim().isNotEmpty())
        {
            lines.add ({});
            lines.add ("control " + readString (control, "language", "lua"));
            for (auto controlLine : juce::StringArray::fromLines (controlCode))
                lines.add ("  " + controlLine);
            lines.add ("endcode");
        }
    }

    auto statesVar = project->getProperty ("states");
    if (! statesVar.isArray())
        return lines.joinIntoString ("\n");

    for (auto& stateVar : *statesVar.getArray())
    {
        auto* state = stateVar.getDynamicObject();
        if (state == nullptr)
            continue;

        lines.add ({});
        lines.add ("state " + readString (state, "id") + " "
                   + quoteToken (readString (state, "section", "Section")) + " "
                   + juce::String (readDouble (state, "durationBeats", 16.0), 2));

        if (auto transitions = state->getProperty ("transitions"); transitions.isArray())
            for (auto& transitionVar : *transitions.getArray())
                if (auto* transition = transitionVar.getDynamicObject())
                    lines.add ("  next " + readString (transition, "to") + " "
                               + juce::String (readDouble (transition, "weight", 1.0) * 100.0, 0));

        if (auto lanes = state->getProperty ("lanes"); lanes.isArray())
        {
            for (auto& laneVar : *lanes.getArray())
            {
                auto* lane = laneVar.getDynamicObject();
                if (lane == nullptr)
                    continue;

                lines.add ("  lane " + quoteToken (readString (lane, "name", "Lane")) + " "
                           + readString (lane, "language", "minitone") + " gain "
                           + juce::String (readDouble (lane, "gain", 0.7), 2));

                if (auto paramsVar = lane->getProperty ("params"); paramsVar.isObject())
                    for (auto& property : paramsVar.getDynamicObject()->getProperties())
                        lines.add ("    param " + property.name.toString() + " " + property.value.toString());

                auto code = readString (lane, "code");
                if (! code.containsChar ('\n'))
                {
                    lines.add ("    code " + code);
                }
                else
                {
                    lines.add ("    code:");
                    for (auto codeLine : juce::StringArray::fromLines (code))
                        lines.add ("      " + codeLine);
                    lines.add ("    endcode");
                }
            }
        }
    }

    return lines.joinIntoString ("\n");
}
