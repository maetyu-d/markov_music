#include "StateMapComponent.h"
#include <cmath>

void StateMapComponent::setProject (const MarkovProject& projectToShow,
                                    int currentIndexToShow,
                                    int selectedIndexToShow)
{
    project = projectToShow;
    currentIndex = currentIndexToShow;
    selectedIndex = selectedIndexToShow;
    repaint();
}

void StateMapComponent::setTransitionWeight (int stateIndex, int transitionIndex, double weight)
{
    if (stateIndex < 0 || stateIndex >= project.states.size())
        return;

    auto& transitions = project.states.getReference (stateIndex).transitions;
    if (transitionIndex < 0 || transitionIndex >= transitions.size())
        return;

    transitions.getReference (transitionIndex).weight = weight;
    repaint();
}

void StateMapComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0f1317));
    nodeBounds.clear();
    transitionLabelHits.clear();
    pendingTransitionLabels.clear();

    auto area = getLocalBounds().toFloat().reduced (22.0f, 18.0f);
    if (project.states.isEmpty() || area.isEmpty())
        return;

    layoutNodes (area);
    drawTransitions (g);
    drawStateNodes (g);
    drawTransitionLabels (g);
}

void StateMapComponent::mouseUp (const juce::MouseEvent& event)
{
    if (event.getNumberOfClicks() >= 2)
    {
        for (auto& hit : transitionLabelHits)
        {
            if (hit.bounds.expanded (5.0f).contains (event.position))
            {
                if (onTransitionDoubleClicked)
                    onTransitionDoubleClicked (hit.stateIndex, hit.transitionIndex);

                return;
            }
        }
    }

    for (int i = 0; i < (int) nodeBounds.size(); ++i)
    {
        if (nodeBounds[(size_t) i].contains (event.position))
        {
            if (onStateClicked)
                onStateClicked (i);

            return;
        }
    }
}

int StateMapComponent::findStateIndex (const juce::String& id) const
{
    for (int i = 0; i < project.states.size(); ++i)
        if (project.states[i].id == id)
            return i;

    return -1;
}

void StateMapComponent::layoutNodes (juce::Rectangle<float> area)
{
    auto count = project.states.size();
    auto radius = juce::jlimit (42.0f, 78.0f, juce::jmin (area.getWidth(), area.getHeight()) / 6.0f);
    auto centre = area.getCentre();
    auto orbitX = juce::jmax (1.0f, area.getWidth() * 0.5f - radius - 16.0f);
    auto orbitY = juce::jmax (1.0f, area.getHeight() * 0.5f - radius - 16.0f);

    for (int i = 0; i < count; ++i)
    {
        auto angle = -juce::MathConstants<float>::halfPi
                   + (juce::MathConstants<float>::twoPi * (float) i / (float) juce::jmax (1, count));
        auto x = count > 1 ? centre.x + std::cos (angle) * orbitX - radius
                           : centre.x - radius;
        auto y = count > 1 ? centre.y + std::sin (angle) * orbitY - radius
                           : centre.y - radius;

        nodeBounds.push_back ({ x, y, radius * 2.0f, radius * 2.0f });
    }
}

void StateMapComponent::drawTransitions (juce::Graphics& g)
{
    auto graphCentre = getGraphCentre();

    for (int from = 0; from < project.states.size(); ++from)
    {
        for (int transitionIndex = 0; transitionIndex < project.states[from].transitions.size(); ++transitionIndex)
        {
            auto transition = project.states[from].transitions[transitionIndex];
            auto to = findStateIndex (transition.targetState);
            if (to < 0 || to >= (int) nodeBounds.size())
                continue;

            auto alpha = (float) juce::jlimit (0.16, 0.82, transition.weight);
            auto active = from == currentIndex;
            g.setColour ((active ? juce::Colour (0xffe8c15a) : juce::Colour (0xff7f8994)).withAlpha (alpha));

            juce::Path path;
            juce::Point<float> labelPosition;
            if (from == to)
            {
                auto node = nodeBounds[(size_t) from];
                auto loop = node.expanded (18.0f, 28.0f).translated (0.0f, -node.getHeight() * 0.32f);
                path.addEllipse (loop);
                labelPosition = loop.getCentre().translated (0.0f, -loop.getHeight() * 0.5f);
            }
            else
            {
                auto startCentre = nodeBounds[(size_t) from].getCentre();
                auto endCentre = nodeBounds[(size_t) to].getCentre();
                auto chord = endCentre - startCentre;
                auto chordLength = lengthOf (chord);
                auto line = normalised (chord);
                auto start = startCentre + line * (nodeBounds[(size_t) from].getWidth() * 0.5f + 4.0f);
                auto end = endCentre - line * (nodeBounds[(size_t) to].getWidth() * 0.5f + 4.0f);
                auto mid = (start + end) * 0.5f;
                auto away = normalised (mid - graphCentre);
                auto centreDistance = lengthOf (mid - graphCentre);

                if (centreDistance < 110.0f)
                {
                    auto startRadial = normalised (startCentre - graphCentre);
                    auto endRadial = normalised (endCentre - graphCentre);
                    auto turn = startRadial.x * endRadial.y - startRadial.y * endRadial.x;
                    auto side = std::abs (turn) > 0.01f ? (turn > 0.0f ? 1.0f : -1.0f)
                                                        : (from < to ? 1.0f : -1.0f);
                    away = { -line.y * side, line.x * side };
                }

                auto bend = centreDistance < 110.0f
                          ? juce::jlimit (230.0f, 360.0f, chordLength * 0.95f)
                          : juce::jlimit (72.0f, 190.0f, chordLength * 0.25f);
                auto control = mid + away * bend;

                path.startNewSubPath (start);
                path.quadraticTo (control, end);

                auto labelT = 0.5f;
                auto oneMinusT = 1.0f - labelT;
                labelPosition = start * (oneMinusT * oneMinusT)
                              + control * (2.0f * oneMinusT * labelT)
                              + end * (labelT * labelT);

                auto orderedLine = from < to ? line : -line;
                auto labelSide = from < to ? 1.0f : -1.0f;
                auto labelOffset = juce::Point<float> (-orderedLine.y, orderedLine.x) * (16.0f * labelSide);
                labelPosition += labelOffset;
            }

            g.strokePath (path, juce::PathStrokeType (active ? 3.0f : 1.8f));
            pendingTransitionLabels.push_back ({ labelPosition, transition.weight, active, from, transitionIndex });
        }
    }
}

void StateMapComponent::drawTransitionLabels (juce::Graphics& g)
{
    for (auto& label : pendingTransitionLabels)
    {
        auto labelBounds = placeTransitionLabel (label.position, label.weight);
        auto drawnBounds = drawTransitionLabel (g, labelBounds, label.weight, label.active);
        transitionLabelHits.push_back ({ drawnBounds, label.stateIndex, label.transitionIndex });
    }
}

juce::Rectangle<float> StateMapComponent::makeTransitionLabelBounds (juce::Point<float> position, double weight) const
{
    auto percent = juce::jlimit (0, 999, (int) std::round (weight * 100.0));
    auto label = juce::String (percent) + "%";
    auto textWidth = juce::jlimit (34.0f, 52.0f, (float) label.length() * 9.5f + 16.0f);
    return juce::Rectangle<float> (textWidth, 22.0f).withCentre (position).toNearestInt().toFloat();
}

juce::Rectangle<float> StateMapComponent::placeTransitionLabel (juce::Point<float> position, double weight) const
{
    auto bounds = makeTransitionLabelBounds (position, weight);
    auto mapArea = getLocalBounds().toFloat().reduced (10.0f);
    auto centre = bounds.getCentre();
    auto graphCentre = getGraphCentre();

    for (int pass = 0; pass < 14; ++pass)
    {
        bool moved = false;

        for (auto& node : nodeBounds)
        {
            auto avoid = node.expanded (7.0f);
            if (bounds.intersects (avoid))
            {
                auto direction = normalised (centre - avoid.getCentre());
                if (direction == juce::Point<float>())
                    direction = normalised (centre - graphCentre);
                if (direction == juce::Point<float>())
                    direction = { 0.0f, -1.0f };

                centre += direction * 10.0f;
                bounds = makeTransitionLabelBounds (centre, weight);
                moved = true;
            }
        }

        for (auto& hit : transitionLabelHits)
        {
            if (bounds.expanded (2.0f).intersects (hit.bounds))
            {
                auto direction = normalised (centre - hit.bounds.getCentre());
                if (direction == juce::Point<float>())
                    direction = { 0.0f, centre.y < graphCentre.y ? -1.0f : 1.0f };

                centre += direction * 8.0f;
                bounds = makeTransitionLabelBounds (centre, weight);
                moved = true;
            }
        }

        auto clampedX = juce::jlimit (mapArea.getX() + bounds.getWidth() * 0.5f,
                                      mapArea.getRight() - bounds.getWidth() * 0.5f,
                                      centre.x);
        auto clampedY = juce::jlimit (mapArea.getY() + bounds.getHeight() * 0.5f,
                                      mapArea.getBottom() - bounds.getHeight() * 0.5f,
                                      centre.y);
        if (std::abs (clampedX - centre.x) > 0.01f || std::abs (clampedY - centre.y) > 0.01f)
        {
            centre = { clampedX, clampedY };
            bounds = makeTransitionLabelBounds (centre, weight);
            moved = true;
        }

        if (! moved)
            break;
    }

    return bounds;
}

juce::Rectangle<float> StateMapComponent::drawTransitionLabel (juce::Graphics& g,
                                                               juce::Rectangle<float> labelArea,
                                                               double weight,
                                                               bool active)
{
    auto percent = juce::jlimit (0, 999, (int) std::round (weight * 100.0));
    auto label = juce::String (percent) + "%";
    g.setColour (juce::Colour (0xff0f1317).withAlpha (active ? 0.92f : 0.82f));
    g.fillRoundedRectangle (labelArea, 4.0f);
    g.setColour ((active ? juce::Colour (0xffe8c15a) : juce::Colour (0xffaeb9c2)).withAlpha (0.95f));
    g.setFont (juce::FontOptions (12.0f, active ? juce::Font::bold : juce::Font::plain));
    g.drawFittedText (label, labelArea.toNearestInt(), juce::Justification::centred, 1);
    return labelArea;
}

void StateMapComponent::drawStateNodes (juce::Graphics& g)
{
    for (int i = 0; i < project.states.size(); ++i)
    {
        auto node = nodeBounds[(size_t) i];
        auto isCurrent = i == currentIndex;
        auto isSelected = i == selectedIndex;

        auto fill = isCurrent ? juce::Colour (0xffe8c15a)
                  : isSelected ? juce::Colour (0xff2f4755)
                               : juce::Colour (0xff20262d);
        auto text = isCurrent ? juce::Colour (0xff0b0d10) : juce::Colour (0xffd9e3ea);

        g.setColour (fill);
        g.fillEllipse (node);
        g.setColour (isSelected ? juce::Colour (0xff9ec8d8) : juce::Colour (0xff38424b));
        g.drawEllipse (node, isCurrent ? 3.0f : (isSelected ? 2.0f : 1.2f));

        auto labelArea = node.reduced (10.0f, 11.0f).toNearestInt();
        g.setColour (text);
        g.setFont (juce::FontOptions (juce::jlimit (13.0f, 19.0f, node.getWidth() * 0.20f),
                                      isCurrent ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText (project.states[i].section,
                          labelArea.removeFromTop (labelArea.getHeight() / 2),
                          juce::Justification::centred,
                          1);

        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 13.0f, node.getWidth() * 0.13f)));
        g.drawFittedText (project.states[i].id, labelArea, juce::Justification::centred, 1);
    }
}

juce::Point<float> StateMapComponent::getGraphCentre() const
{
    if (nodeBounds.empty())
        return getLocalBounds().toFloat().getCentre();

    juce::Point<float> total;
    for (auto& node : nodeBounds)
        total += node.getCentre();

    return total / (float) nodeBounds.size();
}

float StateMapComponent::lengthOf (juce::Point<float> point)
{
    return std::sqrt (point.x * point.x + point.y * point.y);
}

juce::Point<float> StateMapComponent::normalised (juce::Point<float> point)
{
    auto length = lengthOf (point);
    if (length <= 0.0001f)
        return {};

    return point / length;
}
