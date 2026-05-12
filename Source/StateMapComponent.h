#pragma once

#include "MarkovEngine.h"
#include <functional>
#include <vector>

class StateMapComponent final : public juce::Component
{
public:
    std::function<void (int)> onStateClicked;
    std::function<void (int, int)> onTransitionDoubleClicked;

    void setProject (const MarkovProject& projectToShow,
                     int currentIndexToShow,
                     int selectedIndexToShow);
    void setTransitionWeight (int stateIndex, int transitionIndex, double weight);

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    struct TransitionLabelHit
    {
        juce::Rectangle<float> bounds;
        int stateIndex = -1;
        int transitionIndex = -1;
    };

    struct PendingTransitionLabel
    {
        juce::Point<float> position;
        double weight = 0.0;
        bool active = false;
        int stateIndex = -1;
        int transitionIndex = -1;
    };

    int findStateIndex (const juce::String& id) const;
    void layoutNodes (juce::Rectangle<float> area);
    void drawTransitions (juce::Graphics& g);
    void drawTransitionLabels (juce::Graphics& g);
    void drawStateNodes (juce::Graphics& g);
    juce::Rectangle<float> makeTransitionLabelBounds (juce::Point<float> position, double weight) const;
    juce::Rectangle<float> placeTransitionLabel (juce::Point<float> position, double weight) const;
    juce::Rectangle<float> drawTransitionLabel (juce::Graphics& g,
                                                juce::Rectangle<float> labelArea,
                                                double weight,
                                                bool active);

    juce::Point<float> getGraphCentre() const;
    static float lengthOf (juce::Point<float> point);
    static juce::Point<float> normalised (juce::Point<float> point);

    MarkovProject project;
    int currentIndex = -1;
    int selectedIndex = -1;
    std::vector<juce::Rectangle<float>> nodeBounds;
    std::vector<TransitionLabelHit> transitionLabelHits;
    std::vector<PendingTransitionLabel> pendingTransitionLabels;
};
