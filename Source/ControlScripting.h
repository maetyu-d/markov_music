#pragma once

#include <juce_core/juce_core.h>
#include <vector>

struct ControlChoice
{
    juce::String id;
    juce::String section;
    double weight = 1.0;
};

struct ControlRequest
{
    juce::String language;
    juce::String code;
    juce::String currentStateId;
    juce::String currentSection;
    int visitCount = 0;
    juce::Array<ControlChoice> choices;
};

struct ControlParameterChange
{
    juce::String laneName;
    juce::String parameterId;
    float value = 0.0f;
};

struct ControlResult
{
    juce::String nextStateId;
    juce::String error;
    std::vector<ControlParameterChange> parameterChanges;
};

class ControlScriptRunner
{
public:
    ControlResult chooseNextState (const ControlRequest& request) const;
    ControlResult enterState (const ControlRequest& request) const;

private:
    ControlResult runLua (const ControlRequest& request, const juce::String& functionName, bool allowReturnedState) const;
    ControlResult runPythonPlaceholder (const ControlRequest& request) const;
};
