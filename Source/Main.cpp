#include "MarkovEngine.h"
#include "StateMapComponent.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

class MainComponent final : public juce::AudioAppComponent,
                            private juce::Button::Listener,
                            private juce::ComboBox::Listener,
                            private juce::Slider::Listener,
                            private juce::KeyListener,
                            private juce::Timer
{
    friend class MarkovStudioApplication;

public:
    MainComponent()
        : laneCodeEditor (laneCodeDocument, &laneCodeTokeniser),
          scriptEditor (scriptDocument, &scriptTokeniser)
    {
        configureWindow();
        configureMap();
        configureStateControls();
        configureLaneControls();
        configurePerformanceControls();
        configureProjectEditors();
        configureButtons();
        loadInitialProject();
    }

    ~MainComponent() override
    {
        shutdownAudio();
    }

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        const juce::ScopedLock lock (audioLock);
        currentSampleRate = sampleRate;
        currentBlockSize = samplesPerBlockExpected;
        currentOutputChannels = 2;
        engine.prepare (sampleRate, samplesPerBlockExpected, 2);
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        const juce::ScopedLock lock (audioLock);
        if (bufferToFill.buffer != nullptr)
            engine.render (*bufferToFill.buffer, bufferToFill.startSample, bufferToFill.numSamples);
    }

    void releaseResources() override {}

    using juce::Component::keyPressed;

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (editMode
            && key.getKeyCode() == juce::KeyPress::returnKey
            && key.getModifiers().isCommandDown())
        {
            if (laneCodeEditor.hasKeyboardFocus (true))
                applySelectedLaneEdits();
            else
                applyScript();

            return true;
        }

        if (key == juce::KeyPress::escapeKey && editMode)
        {
            showMapView();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0d10));

        auto bounds = getLocalBounds().reduced (18);
        auto header = bounds.removeFromTop (54);
        juce::ignoreUnused (header);

        if (editMode)
        {
            auto left = bounds.removeFromLeft (360);
            g.setColour (juce::Colour (0xff171b20));
            g.fillRoundedRectangle (left.toFloat(), 8.0f);

            g.setColour (juce::Colour (0xff20262d));
            g.fillRoundedRectangle (bounds.withTrimmedLeft (14).toFloat(), 8.0f);
        }
        else
        {
            g.setColour (juce::Colour (0xff171b20));
            g.fillRoundedRectangle (bounds.toFloat(), 8.0f);
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (18);
        auto header = bounds.removeFromTop (54);

        title.setBounds (header.removeFromLeft (360));
        transportButton.setBounds (header.removeFromRight (104).reduced (0, 8));
        editModeButton.setBounds (header.removeFromRight (86).reduced (8, 8));
        saveProjectButton.setBounds (header.removeFromRight (70).reduced (8, 8));
        openProjectButton.setBounds (header.removeFromRight (70).reduced (8, 8));

        if (editMode)
        {
            exportAudioButton.setBounds (header.removeFromRight (86).reduced (8, 8));
            snapshotProjectButton.setBounds (header.removeFromRight (96).reduced (8, 8));
            addLaneButton.setBounds (header.removeFromRight (104).reduced (8, 8));
            addStateButton.setBounds (header.removeFromRight (104).reduced (8, 8));
            applyButton.setBounds (header.removeFromRight (120).reduced (8, 8));
            demoButton.setBounds (header.removeFromRight (104).reduced (8, 8));
        }

        if (! editMode)
        {
            auto simple = bounds.reduced (18);
            auto controlRow = simple.removeFromBottom (42);
            simple.removeFromBottom (12);
            stateMap.setBounds (simple);

            stateSelector.setBounds (controlRow.removeFromLeft (300));
            controlRow.removeFromLeft (16);
            laneSelector.setBounds (controlRow.removeFromLeft (300));
            controlRow.removeFromLeft (16);
            parameterSelector.setBounds (controlRow.removeFromLeft (240));
            controlRow.removeFromLeft (16);
            parameterSlider.setBounds (controlRow);
            updatePresentationMode();
            return;
        }

        auto left = bounds.removeFromLeft (360).reduced (14);
        status.setBounds (left.removeFromTop (70));
        left.removeFromTop (10);
        mapLabel.setBounds (left.removeFromTop (22));
        left.removeFromTop (4);
        stateMap.setBounds (left.removeFromTop (236));
        left.removeFromTop (10);
        stateSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        stateTemplateSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        auto stateEditRow = left.removeFromTop (32);
        stateIdEditor.setBounds (stateEditRow.removeFromLeft (132));
        stateEditRow.removeFromLeft (8);
        stateSectionEditor.setBounds (stateEditRow.removeFromLeft (102));
        stateEditRow.removeFromLeft (8);
        applyStateButton.setBounds (stateEditRow);
        left.removeFromTop (8);
        auto stateButtons = left.removeFromTop (32);
        duplicateStateButton.setBounds (stateButtons.removeFromLeft (162));
        stateButtons.removeFromLeft (8);
        deleteStateButton.setBounds (stateButtons);
        left.removeFromTop (8);
        laneSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        newLaneLanguageSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        laneTemplateSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        parameterSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        parameterSlider.setBounds (left.removeFromTop (34));
        left.removeFromTop (12);
        transitionSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        transitionWeightSlider.setBounds (left.removeFromTop (34));
        left.removeFromTop (8);
        transitionTargetSelector.setBounds (left.removeFromTop (32));
        left.removeFromTop (8);
        auto transitionButtons = left.removeFromTop (32);
        addTransitionButton.setBounds (transitionButtons.removeFromLeft (154));
        transitionButtons.removeFromLeft (8);
        deleteTransitionButton.setBounds (transitionButtons);
        left.removeFromTop (12);
        durationSlider.setBounds (left.removeFromTop (34));
        left.removeFromTop (12);
        tempoSlider.setBounds (left.removeFromTop (34));
        left.removeFromTop (10);
        inspectorLabel.setBounds (left.removeFromTop (22));
        left.removeFromTop (4);
        laneInspector.setBounds (left);

        auto editorArea = bounds.withTrimmedLeft (14).reduced (14);
        auto laneEditArea = editorArea.removeFromTop (juce::jmax (260, editorArea.getHeight() / 2));
        laneCodeLabel.setBounds (laneEditArea.removeFromTop (24));
        laneEditArea.removeFromTop (6);
        auto laneEditHeader = laneEditArea.removeFromTop (34);
        laneNameEditor.setBounds (laneEditHeader.removeFromLeft (190));
        laneEditHeader.removeFromLeft (8);
        laneLanguageSelector.setBounds (laneEditHeader.removeFromLeft (150));
        laneEditHeader.removeFromLeft (8);
        laneGainSlider.setBounds (laneEditHeader.removeFromLeft (150));
        laneEditHeader.removeFromLeft (8);
        laneMuteButton.setBounds (laneEditHeader.removeFromLeft (78));
        laneEditHeader.removeFromLeft (8);
        applyLaneButton.setBounds (laneEditHeader.removeFromLeft (104));
        laneEditHeader.removeFromLeft (8);
        rerenderLaneButton.setBounds (laneEditHeader.removeFromLeft (92));
        laneEditHeader.removeFromLeft (8);
        duplicateLaneButton.setBounds (laneEditHeader.removeFromLeft (92));
        laneEditHeader.removeFromLeft (8);
        deleteLaneButton.setBounds (laneEditHeader.removeFromLeft (82));
        laneEditArea.removeFromTop (8);
        laneCodeEditor.setBounds (laneEditArea);
        editorArea.removeFromTop (14);
        songScriptLabel.setBounds (editorArea.removeFromTop (24));
        editorArea.removeFromTop (6);
        scriptEditor.setBounds (editorArea);
        updatePresentationMode();
    }

private:
    void configureWindow()
    {
        setWantsKeyboardFocus (true);
        addKeyListener (this);

        title.setText ("Markov Music by matd.space", juce::dontSendNotification);
        title.setJustificationType (juce::Justification::centredLeft);
        title.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        addAndMakeVisible (title);

        configureTextEditor (status, {}, true, true, false);

        setSize (1320, 760);
        setAudioChannels (0, 2);
    }

    void configureMap()
    {
        stateMap.onStateClicked = [this] (int stateIndex)
        {
            revealStateCodeFromMap (stateIndex);
        };

        stateMap.onTransitionDoubleClicked = [this] (int stateIndex, int transitionIndex)
        {
            revealTransitionFromMap (stateIndex, transitionIndex);
        };

        addAndMakeVisible (stateMap);
    }

    void configureStateControls()
    {
        configureComboBox (stateSelector, "Choose part", true);

        configureComboBox (stateTemplateSelector, "New part type");
        addComboItems (stateTemplateSelector, { "Verse", "Chorus", "Bridge", "Intro", "Outro", "Breakdown" });
        stateTemplateSelector.setText ("Verse", juce::dontSendNotification);

        configureTextEditor (stateIdEditor, "Part name");
        configureTextEditor (stateSectionEditor, "Label");
    }

    void configureLaneControls()
    {
        auto languageIds = languageRegistry.getLanguageIds();

        configureComboBox (laneSelector, "Choose instrument", true);
        configureComboBox (newLaneLanguageSelector, "Instrument language");
        configureComboBox (laneLanguageSelector, "Language");

        addComboItems (newLaneLanguageSelector, languageIds);
        addComboItems (laneLanguageSelector, languageIds);
        newLaneLanguageSelector.setText ("minitone", juce::dontSendNotification);

        configureComboBox (laneTemplateSelector, "Instrument preset");
        addComboItems (laneTemplateSelector, { "Minitone Bass", "Minitone Pulse", "Minitone Texture",
                                               "Faust Oscillator", "Faust Filter", "Csound Drone",
                                               "Csound Pluck", "Cmajor Sine", "ChucK Sine",
                                               "ChucK Pulse", "SuperCollider Synth" });
        laneTemplateSelector.setText ("Minitone Bass", juce::dontSendNotification);

        configureTextEditor (laneNameEditor, "Instrument name");
        configureSlider (laneGainSlider, 0.0, 1.5, 0.01);
        configureCodeEditor (laneCodeEditor);
    }

    void configurePerformanceControls()
    {
        configureComboBox (parameterSelector, "Sound control", true);
        configureSlider (parameterSlider);

        configureComboBox (transitionSelector, "Next choice", true);
        configureComboBox (transitionTargetSelector, "Can go to");
        configureSlider (transitionWeightSlider, 0.0, 1.0, 0.01);
        configureSlider (durationSlider, 0.25, 64.0, 0.25);
        configureSlider (tempoSlider, 20.0, 300.0, 1.0);
    }

    void configureProjectEditors()
    {
        configureTextEditor (laneInspector, {}, true, true, false);
        configureCodeEditor (scriptEditor);
        configureSectionLabel (mapLabel, "Map");
        configureSectionLabel (inspectorLabel, "Runtime");
        configureSectionLabel (laneCodeLabel, "Lane Code");
        configureSectionLabel (songScriptLabel, "Song Code");
        setScriptText (MarkovEngine::makeDemoScript());
    }

    void configureButtons()
    {
        for (auto* button : std::initializer_list<juce::Button*> { &applyButton, &demoButton, &transportButton,
                                                                    &addStateButton, &addLaneButton, &applyLaneButton,
                                                                    &duplicateLaneButton, &deleteLaneButton, &laneMuteButton,
                                                                    &applyStateButton, &addTransitionButton,
                                                                    &deleteTransitionButton, &openProjectButton,
                                                                    &saveProjectButton, &snapshotProjectButton,
                                                                    &exportAudioButton, &duplicateStateButton,
                                                                    &deleteStateButton, &rerenderLaneButton,
                                                                    &editModeButton })
        {
            button->addListener (this);
            addAndMakeVisible (*button);
        }

        applyButton.setButtonText ("Apply Song");
        demoButton.setButtonText ("Demo Song");
        transportButton.setButtonText ("Start");
        editModeButton.setButtonText ("Edit");
        addStateButton.setButtonText ("Add Part");
        addLaneButton.setButtonText ("Add Instrument");
        applyLaneButton.setButtonText ("Apply Sound");
        duplicateLaneButton.setButtonText ("Duplicate");
        deleteLaneButton.setButtonText ("Delete");
        laneMuteButton.setButtonText ("Mute");
        applyStateButton.setButtonText ("Apply Part");
        addTransitionButton.setButtonText ("Add Choice");
        deleteTransitionButton.setButtonText ("Delete Choice");
        openProjectButton.setButtonText ("Open");
        saveProjectButton.setButtonText ("Save");
        snapshotProjectButton.setButtonText ("Snapshot");
        exportAudioButton.setButtonText ("Export");
        duplicateStateButton.setButtonText ("Duplicate Part");
        deleteStateButton.setButtonText ("Delete Part");
        rerenderLaneButton.setButtonText ("Refresh Sound");
    }

    void loadInitialProject()
    {
        juce::String error;
        {
            const juce::ScopedLock lock (audioLock);
            engine.loadFromScript (getScriptText(), languageRegistry, error);
        }
        refreshProjectView();
        startTimerHz (8);
        updatePresentationMode();
        refreshStatus();
    }

    void configureComboBox (juce::ComboBox& comboBox,
                            const juce::String& placeholder,
                            bool listensForChanges = false)
    {
        if (listensForChanges)
            comboBox.addListener (this);

        comboBox.setTextWhenNothingSelected (placeholder);
        addAndMakeVisible (comboBox);
    }

    void configureSlider (juce::Slider& slider,
                          double minimum = 0.0,
                          double maximum = 1.0,
                          double interval = 0.01)
    {
        slider.addListener (this);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 24);
        slider.setRange (minimum, maximum, interval);
        addAndMakeVisible (slider);
    }

    void configureTextEditor (juce::TextEditor& editor,
                              const juce::String& placeholder = {},
                              bool multiLine = false,
                              bool readOnly = false,
                              bool escapeReturnsToMap = false)
    {
        editor.setMultiLine (multiLine);
        editor.setReadOnly (readOnly);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101317));
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff252b31));
        editor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffd9e3ea));

        if (placeholder.isNotEmpty())
            editor.setTextToShowWhenEmpty (placeholder, juce::Colour (0xff7f8994));

        if (multiLine)
        {
            editor.setReturnKeyStartsNewLine (true);
            editor.setTabKeyUsedAsCharacter (true);
            editor.setFont (juce::FontOptions (15.0f));
        }

        if (escapeReturnsToMap)
            editor.addKeyListener (this);

        addAndMakeVisible (editor);
    }

    void configureCodeEditor (juce::CodeEditorComponent& editor)
    {
        editor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain)));
        editor.setTabSize (2, true);
        editor.setLineNumbersShown (true);
        editor.setScrollbarThickness (10);
        editor.setColour (juce::CodeEditorComponent::backgroundColourId, juce::Colour (0xff0d1116));
        editor.setColour (juce::CodeEditorComponent::defaultTextColourId, juce::Colour (0xffd9e3ea));
        editor.setColour (juce::CodeEditorComponent::highlightColourId, juce::Colour (0x663a7fa2));
        editor.setColour (juce::CodeEditorComponent::lineNumberBackgroundId, juce::Colour (0xff111820));
        editor.setColour (juce::CodeEditorComponent::lineNumberTextId, juce::Colour (0xff6f7c86));
        editor.addKeyListener (this);

        juce::CodeEditorComponent::ColourScheme scheme;
        scheme.set ("Error", juce::Colour (0xffff6d6d));
        scheme.set ("Comment", juce::Colour (0xff71808c));
        scheme.set ("Keyword", juce::Colour (0xff88c0d0));
        scheme.set ("Operator", juce::Colour (0xffd8dee9));
        scheme.set ("Identifier", juce::Colour (0xffd9e3ea));
        scheme.set ("Integer", juce::Colour (0xffeacb64));
        scheme.set ("Float", juce::Colour (0xffeacb64));
        scheme.set ("String", juce::Colour (0xffa3be8c));
        scheme.set ("Bracket", juce::Colour (0xffd8dee9));
        scheme.set ("Punctuation", juce::Colour (0xffaeb9c3));
        scheme.set ("Preprocessor Text", juce::Colour (0xffb48ead));
        editor.setColourScheme (scheme);

        addAndMakeVisible (editor);
    }

    juce::String getScriptText() const
    {
        return scriptDocument.getAllContent();
    }

    void setScriptText (const juce::String& text)
    {
        scriptEditor.loadContent (text);
    }

    juce::String getLaneCodeText() const
    {
        return laneCodeDocument.getAllContent();
    }

    void setLaneCodeText (const juce::String& text)
    {
        laneCodeEditor.loadContent (text);
    }

    void configureSectionLabel (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        label.setJustificationType (juce::Justification::centredLeft);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffaeb9c3));
        addAndMakeVisible (label);
    }

    static void addComboItems (juce::ComboBox& comboBox, std::initializer_list<const char*> items)
    {
        for (auto* item : items)
            comboBox.addItem (item, comboBox.getNumItems() + 1);
    }

    static void addComboItems (juce::ComboBox& comboBox, const juce::StringArray& items)
    {
        for (auto& item : items)
            comboBox.addItem (item, comboBox.getNumItems() + 1);
    }

    void updatePresentationMode()
    {
        editModeButton.setButtonText (editMode ? "Play" : "Edit");

        for (auto* component : std::initializer_list<juce::Component*> {
                 &stateTemplateSelector, &stateIdEditor, &stateSectionEditor,
                 &newLaneLanguageSelector, &laneTemplateSelector, &laneLanguageSelector,
                 &laneNameEditor, &laneGainSlider, &laneMuteButton, &laneCodeEditor,
                 &transitionSelector, &transitionTargetSelector, &transitionWeightSlider,
                 &durationSlider, &tempoSlider, &scriptEditor,
                 &mapLabel, &inspectorLabel, &laneCodeLabel, &songScriptLabel,
                 &applyButton, &demoButton, &addStateButton, &addLaneButton,
                 &applyLaneButton, &duplicateLaneButton, &deleteLaneButton,
                 &applyStateButton, &addTransitionButton, &deleteTransitionButton,
                 &snapshotProjectButton, &exportAudioButton, &duplicateStateButton,
                 &deleteStateButton, &rerenderLaneButton })
        {
            component->setVisible (editMode);
        }

        stateSelector.setVisible (true);
        laneSelector.setVisible (true);
        parameterSelector.setVisible (true);
        parameterSlider.setVisible (true);
        status.setVisible (editMode || compiling);
        laneInspector.setVisible (editMode);
        stateMap.setVisible (true);
        openProjectButton.setVisible (true);
        saveProjectButton.setVisible (true);
        transportButton.setVisible (true);
        editModeButton.setVisible (true);
        title.setVisible (true);

        status.setFont (juce::FontOptions (editMode ? 14.0f : 22.0f));
        laneInspector.setFont (juce::FontOptions (editMode ? 14.0f : 18.0f));
    }

    void buttonClicked (juce::Button* button) override
    {
        if (button == &demoButton)
        {
            setScriptText (MarkovEngine::makeDemoScript());
            applyScript();
        }
        else if (button == &applyButton)
        {
            applyScript();
        }
        else if (button == &transportButton)
        {
            {
                const juce::ScopedLock lock (audioLock);
                engine.setPlaying (! engine.isPlaying());
            }
            transportButton.setButtonText (engine.isPlaying() ? "Stop" : "Start");
            refreshStatus();
        }
        else if (button == &editModeButton)
        {
            if (editMode)
                showMapView();
            else
                showEditView();
        }
        else if (button == &addStateButton)
        {
            addStateToScript();
        }
        else if (button == &duplicateStateButton)
        {
            duplicateSelectedState();
        }
        else if (button == &deleteStateButton)
        {
            deleteSelectedState();
        }
        else if (button == &addLaneButton)
        {
            addLaneToSelectedState();
        }
        else if (button == &applyLaneButton)
        {
            applySelectedLaneEdits();
        }
        else if (button == &rerenderLaneButton)
        {
            rerenderSelectedLane();
        }
        else if (button == &duplicateLaneButton)
        {
            duplicateSelectedLane();
        }
        else if (button == &deleteLaneButton)
        {
            deleteSelectedLane();
        }
        else if (button == &applyStateButton)
        {
            applySelectedStateEdits();
        }
        else if (button == &addTransitionButton)
        {
            addTransitionToSelectedState();
        }
        else if (button == &deleteTransitionButton)
        {
            deleteSelectedTransition();
        }
        else if (button == &openProjectButton)
        {
            openProject();
        }
        else if (button == &saveProjectButton)
        {
            saveProject();
        }
        else if (button == &snapshotProjectButton)
        {
            saveProjectSnapshot();
        }
        else if (button == &exportAudioButton)
        {
            exportAudioSnapshot();
        }
    }

    void comboBoxChanged (juce::ComboBox* comboBox) override
    {
        if (comboBox == &stateSelector)
        {
            refreshStateMap();
            refreshSelectedStateEditor();
            refreshLaneSelector();
            refreshTransitionSelector();
            refreshTransitionTargetSelector();
            refreshDurationSlider();
            refreshSelectedLaneEditor();
            refreshLaneInspector();
        }
        else if (comboBox == &laneSelector)
        {
            refreshParameterSelector();
            refreshSelectedLaneEditor();
            refreshLaneInspector();
        }
        else if (comboBox == &parameterSelector)
        {
            refreshParameterSlider();
        }
        else if (comboBox == &transitionSelector)
        {
            refreshTransitionSlider();
        }
    }

    void sliderValueChanged (juce::Slider* slider) override
    {
        if (updatingParameterControls)
            return;

        if (slider == &transitionWeightSlider)
        {
            updateSelectedTransitionWeight ((float) slider->getValue());
            return;
        }

        if (slider == &durationSlider)
        {
            updateSelectedStateDuration ((float) slider->getValue());
            return;
        }

        if (slider == &tempoSlider)
        {
            updateTempo ((float) slider->getValue());
            return;
        }

        if (slider != &parameterSlider)
            return;

        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneName = laneSelector.getText();
        auto parameterId = selectedParameterId;

        if (stateIndex < 0 || laneName.isEmpty() || parameterId.isEmpty())
            return;

        auto changed = false;
        {
            const juce::ScopedLock lock (audioLock);
            changed = engine.setLaneParameter (stateIndex, laneName, parameterId, (float) slider->getValue());
        }

        if (changed)
            persistParameterToScript (stateIndex, laneName, parameterId, (float) slider->getValue());

        refreshLaneInspector();
    }

    void timerCallback() override
    {
        {
            const juce::ScopedLock lock (audioLock);
            engine.serviceBackgroundTasks();
        }

        refreshStateMap();
        refreshStatus();
        refreshLaneInspector();
    }

    void applyScript()
    {
        loadScriptInBackground (getScriptText(),
                                "Preparing song...",
                                stateSelector.getSelectedId(),
                                laneSelector.getSelectedId(),
                                transitionSelector.getSelectedId());
    }

    void revealStateCodeFromMap (int stateIndex)
    {
        auto& project = engine.getProject();
        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        stateSelector.setSelectedId (stateIndex + 1, juce::sendNotificationSync);

        if (! project.states[stateIndex].lanes.isEmpty())
            laneSelector.setSelectedId (juce::jmax (1, laneSelector.getSelectedId()), juce::sendNotificationSync);

        if (! editMode)
        {
            showEditView();
        }

        refreshSelectedLaneEditor();
        laneCodeEditor.grabKeyboardFocus();
        laneCodeEditor.moveCaretToTop (false);
    }

    void revealTransitionFromMap (int stateIndex, int transitionIndex)
    {
        auto& project = engine.getProject();
        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        if (transitionIndex < 0 || transitionIndex >= project.states[stateIndex].transitions.size())
            return;

        stateSelector.setSelectedId (stateIndex + 1, juce::sendNotificationSync);
        transitionSelector.setSelectedId (transitionIndex + 1, juce::sendNotificationSync);
        refreshTransitionSlider();
        editTransitionWeightFromMap (stateIndex, transitionIndex);
    }

    void editTransitionWeightFromMap (int stateIndex, int transitionIndex)
    {
        auto& project = engine.getProject();
        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        auto state = project.states[stateIndex];
        if (transitionIndex < 0 || transitionIndex >= state.transitions.size())
            return;

        auto transition = state.transitions[transitionIndex];
        juce::AlertWindow editor ("Transition chance",
                                  state.section + " can go to " + transition.targetState,
                                  juce::AlertWindow::NoIcon);

        editor.addTextEditor ("chance",
                              juce::String (transition.weight * 100.0, 0),
                              "Probability (%)");
        editor.addButton ("Update", 1, juce::KeyPress (juce::KeyPress::returnKey));
        editor.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        if (editor.runModalLoop() != 1)
            return;

        auto percent = editor.getTextEditorContents ("chance").retainCharacters ("0123456789.").getDoubleValue();
        updateMapTransitionWeight (stateIndex, transitionIndex, (float) juce::jlimit (0.0, 999.0, percent) / 100.0f);
    }

    void updateMapTransitionWeight (int stateIndex, int transitionIndex, float value)
    {
        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex < 0 || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto transitionsVar = stateObject->getProperty ("transitions");
        if (! transitionsVar.isArray() || transitionIndex < 0 || transitionIndex >= transitionsVar.getArray()->size())
            return;

        if (auto* transitionObject = transitionsVar.getArray()->getReference (transitionIndex).getDynamicObject())
            transitionObject->setProperty ("weight", value);
        else
            return;

        showProjectSource (root);
        {
            const juce::ScopedLock lock (audioLock);
            engine.setTransitionWeight (stateIndex, transitionIndex, value);
        }

        pendingMapWeightState = stateIndex;
        pendingMapWeightTransition = transitionIndex;
        pendingMapWeightValue = value;
        stateMap.setTransitionWeight (stateIndex, transitionIndex, value);
        transitionWeightSlider.setValue (value, juce::dontSendNotification);
        refreshStateMap();
    }

    void showMapView()
    {
        editMode = false;
        updatePresentationMode();
        resized();
        repaint();
        grabKeyboardFocus();
    }

    void showEditView()
    {
        editMode = true;
        updatePresentationMode();
        resized();
        repaint();
    }

    bool applyScriptText (const juce::String& text, const juce::String& successMessage = {})
    {
        juce::var root;
        juce::String error;
        auto visibleText = text;
        if (MarkovEngine::scriptToJson (text, root, error))
            visibleText = MarkovEngine::jsonToReadableScript (root);

        setScriptText (visibleText);
        loadScriptInBackground (visibleText,
                                successMessage.isNotEmpty() ? successMessage : "Preparing song...",
                                stateSelector.getSelectedId(),
                                laneSelector.getSelectedId(),
                                transitionSelector.getSelectedId());
        return true;
    }

    void loadScriptInBackground (juce::String scriptText,
                                 juce::String statusMessage,
                                 int stateIdToRestore,
                                 int laneIdToRestore,
                                 int transitionIdToRestore)
    {
        auto revision = ++compileRevision;
        compiling = true;
        updatePresentationMode();
        status.setText (statusMessage + "\n\nGetting the sounds ready.", juce::dontSendNotification);

        auto sampleRate = currentSampleRate;
        auto blockSize = currentBlockSize;
        auto outputChannels = currentOutputChannels;
        auto wasPlaying = engine.isPlaying();
        juce::Component::SafePointer<MainComponent> safeThis (this);

        std::thread ([safeThis,
                      revision,
                      scriptToCompile = std::move (scriptText),
                      compileMessage = std::move (statusMessage),
                      sampleRate,
                      blockSize,
                      outputChannels,
                      wasPlaying,
                      stateIdToRestore,
                      laneIdToRestore,
                      transitionIdToRestore]()
        {
            if (safeThis == nullptr)
                return;

            auto compiledEngine = std::make_unique<MarkovEngine>();
            if (sampleRate > 0.0)
                compiledEngine->prepare (sampleRate, blockSize, outputChannels);

            juce::String error;
            auto ok = compiledEngine->loadFromScript (scriptToCompile, safeThis->languageRegistry, error);
            if (ok && wasPlaying)
                compiledEngine->setPlaying (true);

            juce::MessageManager::callAsync ([safeThis,
                                              revision,
                                              ok,
                                              error,
                                              compileMessage,
                                              stateIdToRestore,
                                              laneIdToRestore,
                                              transitionIdToRestore,
                                              engineToInstall = std::move (compiledEngine)]() mutable
            {
                if (safeThis == nullptr || revision != safeThis->compileRevision.load())
                    return;

                safeThis->compiling = false;
                safeThis->updatePresentationMode();

                if (! ok)
                {
                    safeThis->status.setVisible (true);
                    safeThis->status.setText ("This song needs a small fix:\n" + error, juce::dontSendNotification);
                    return;
                }

                {
                    const juce::ScopedLock lock (safeThis->audioLock);
                    safeThis->engine = std::move (*engineToInstall);
                }

                safeThis->refreshProjectView();
                if (stateIdToRestore > 0)
                    safeThis->stateSelector.setSelectedId (stateIdToRestore, juce::dontSendNotification);
                safeThis->refreshLaneSelector();
                if (laneIdToRestore > 0)
                    safeThis->laneSelector.setSelectedId (laneIdToRestore, juce::dontSendNotification);
                safeThis->refreshParameterSelector();
                safeThis->refreshTransitionSelector();
                if (transitionIdToRestore > 0)
                    safeThis->transitionSelector.setSelectedId (transitionIdToRestore, juce::dontSendNotification);
                safeThis->refreshTransitionSlider();
                safeThis->refreshDurationSlider();
                safeThis->refreshTempoSlider();
                safeThis->refreshSelectedLaneEditor();
                safeThis->refreshLaneInspector();
                safeThis->refreshStatus();

                if (compileMessage.isNotEmpty())
                    safeThis->status.setText (compileMessage + "\n\n" + safeThis->status.getText(), juce::dontSendNotification);
            });
        }).detach();
    }

    void openProject()
    {
        juce::FileChooser chooser ("Open Markov project",
                                   currentProjectFile.existsAsFile() ? currentProjectFile
                                                                     : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                   "*.markov.json;*.json");

        if (! chooser.browseForFileToOpen())
            return;

        auto file = chooser.getResult();
        auto text = file.loadFileAsString();
        if (text.trim().isEmpty())
        {
            status.setText ("That song file is empty.", juce::dontSendNotification);
            return;
        }

        if (applyScriptText (text, "Opened " + file.getFileName()))
            currentProjectFile = file;
    }

    void saveProject()
    {
        if (! currentProjectFile.existsAsFile())
        {
            juce::FileChooser chooser ("Save Markov project",
                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                           .getChildFile ("markov-project.markov.json"),
                                       "*.markov.json;*.json");

            if (! chooser.browseForFileToSave (true))
                return;

            currentProjectFile = chooser.getResult();
        }

        writeProjectFile (currentProjectFile, "Saved ");
    }

    void saveProjectSnapshot()
    {
        auto folder = currentProjectFile.existsAsFile()
                    ? currentProjectFile.getParentDirectory()
                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        auto timestamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        auto baseName = currentProjectFile.existsAsFile()
                      ? currentProjectFile.getFileNameWithoutExtension().upToLastOccurrenceOf (".markov", false, false)
                      : "markov-project";
        auto snapshot = folder.getChildFile (baseName + "-" + timestamp + ".markov.json");
        writeProjectFile (snapshot, "Snapshot saved ");
    }

    void writeProjectFile (const juce::File& file, const juce::String& prefix)
    {
        juce::var root;
        juce::String error;
        if (! MarkovEngine::scriptToJson (getScriptText(), root, error))
        {
            status.setText ("The song was not saved:\n" + error, juce::dontSendNotification);
            return;
        }

        if (! file.replaceWithText (juce::JSON::toString (root, true)))
        {
            status.setText ("I could not save that file.", juce::dontSendNotification);
            return;
        }

        status.setText (prefix + file.getFileName() + "\n\n" + status.getText(), juce::dontSendNotification);
    }

    juce::var parseVisibleProject()
    {
        juce::var root;
        juce::String error;
        if (! MarkovEngine::scriptToJson (getScriptText(), root, error))
            status.setText ("That edit needs a small fix:\n" + error, juce::dontSendNotification);

        return root;
    }

    void showProjectSource (const juce::var& root)
    {
        setScriptText (MarkovEngine::jsonToReadableScript (root));
    }

    void exportAudioSnapshot()
    {
        auto folder = currentProjectFile.existsAsFile()
                    ? currentProjectFile.getParentDirectory()
                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        auto timestamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        auto defaultFile = folder.getChildFile ("markov-export-" + timestamp + ".wav");
        juce::FileChooser chooser ("Export audio snapshot", defaultFile, "*.wav");

        if (! chooser.browseForFileToSave (true))
            return;

        auto file = chooser.getResult();
        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension (".wav");

        juce::String error;
        if (! renderScriptToWav (getScriptText(), file, languageRegistry, 60.0, error))
        {
            status.setText ("The audio export did not finish:\n" + error, juce::dontSendNotification);
            return;
        }

        status.setText ("Exported " + file.getFileName() + "\n\n" + status.getText(), juce::dontSendNotification);
    }

    void refreshStatus()
    {
        if (compiling)
            return;

        if (editMode)
        {
            status.setText ((engine.isPlaying() ? "Playing\n" : "Stopped\n") + engine.getStatusText()
                                + "\n\nSound languages: " + languageRegistry.getLanguageIds().joinIntoString (", "),
                            juce::dontSendNotification);
            return;
        }

        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneCount = stateIndex >= 0 && stateIndex < project.states.size()
                       ? project.states[stateIndex].lanes.size()
                       : 0;

        juce::StringArray lines;
        lines.add (engine.isPlaying() ? "Playing" : "Stopped");
        lines.add (engine.getCurrentStateName());
        lines.add (juce::String (laneCount) + " instruments  " + juce::String (project.bpm, 0) + " BPM");
        status.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
    }

    void refreshStateMap()
    {
        stateMap.setProject (engine.getProject(),
                             engine.getCurrentStateIndex(),
                             stateSelector.getSelectedId() - 1);

        if (pendingMapWeightState >= 0)
            stateMap.setTransitionWeight (pendingMapWeightState,
                                          pendingMapWeightTransition,
                                          pendingMapWeightValue);
    }

    void refreshProjectView()
    {
        stateSelector.clear (juce::dontSendNotification);

        auto& project = engine.getProject();
        for (int i = 0; i < project.states.size(); ++i)
        {
            auto state = project.states[i];
            stateSelector.addItem (state.section + " / " + state.id, i + 1);
        }

        if (project.states.size() > 0)
            stateSelector.setSelectedId (1, juce::dontSendNotification);

        refreshSelectedStateEditor();
        refreshTransitionTargetSelector();
        refreshLaneSelector();
        refreshTransitionSelector();
        refreshDurationSlider();
        refreshTempoSlider();
        refreshSelectedLaneEditor();
        refreshLaneInspector();
        refreshStateMap();
    }

    void refreshTempoSlider()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        tempoSlider.setValue (engine.getProject().bpm, juce::dontSendNotification);
    }

    void refreshLaneSelector()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        laneSelector.clear (juce::dontSendNotification);

        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;

        if (stateIndex >= 0 && stateIndex < project.states.size())
        {
            auto state = project.states[stateIndex];
            for (int i = 0; i < state.lanes.size(); ++i)
                laneSelector.addItem (state.lanes[i].name, i + 1);

            if (! state.lanes.isEmpty())
                laneSelector.setSelectedId (1, juce::dontSendNotification);
        }

        refreshParameterSelector();
        refreshSelectedLaneEditor();
    }

    void refreshSelectedLaneEditor()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        laneNameEditor.clear();
        setLaneCodeText ({});
        laneLanguageSelector.setText ({}, juce::dontSendNotification);
        laneGainSlider.setValue (0.0, juce::dontSendNotification);
        laneMuteButton.setToggleState (false, juce::dontSendNotification);

        laneNameEditor.setEnabled (false);
        laneCodeEditor.setEnabled (false);
        laneLanguageSelector.setEnabled (false);
        laneGainSlider.setEnabled (false);
        laneMuteButton.setEnabled (false);
        applyLaneButton.setEnabled (false);
        rerenderLaneButton.setEnabled (false);
        duplicateLaneButton.setEnabled (false);
        deleteLaneButton.setEnabled (false);

        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneIndex = laneSelector.getSelectedId() - 1;

        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        auto state = project.states[stateIndex];
        if (laneIndex < 0 || laneIndex >= state.lanes.size())
            return;

        auto lane = state.lanes[laneIndex];
        laneNameEditor.setText (lane.name, juce::dontSendNotification);
        laneLanguageSelector.setText (lane.language, juce::dontSendNotification);
        laneGainSlider.setValue (lane.gain, juce::dontSendNotification);
        laneMuteButton.setToggleState (lane.muted, juce::dontSendNotification);
        setLaneCodeText (lane.code);

        laneNameEditor.setEnabled (true);
        laneCodeEditor.setEnabled (true);
        laneLanguageSelector.setEnabled (true);
        laneGainSlider.setEnabled (true);
        laneMuteButton.setEnabled (true);
        applyLaneButton.setEnabled (true);
        rerenderLaneButton.setEnabled (true);
        duplicateLaneButton.setEnabled (true);
        deleteLaneButton.setEnabled (true);
    }

    void refreshTransitionSelector()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        transitionSelector.clear (juce::dontSendNotification);
        selectedTransitionIndex = -1;

        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;

        if (stateIndex >= 0 && stateIndex < project.states.size())
        {
            auto state = project.states[stateIndex];
            for (int i = 0; i < state.transitions.size(); ++i)
                transitionSelector.addItem (state.transitions[i].targetState, i + 1);

            if (! state.transitions.isEmpty())
            {
                transitionSelector.setSelectedId (1, juce::dontSendNotification);
                selectedTransitionIndex = 0;
            }
        }

        refreshTransitionSlider();
    }

    void refreshTransitionSlider()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto transitionIndex = transitionSelector.getSelectedId() - 1;
        selectedTransitionIndex = transitionIndex;

        transitionWeightSlider.setEnabled (false);
        transitionWeightSlider.setValue (0.0, juce::dontSendNotification);
        deleteTransitionButton.setEnabled (false);

        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        auto state = project.states[stateIndex];
        if (transitionIndex < 0 || transitionIndex >= state.transitions.size())
            return;

        transitionWeightSlider.setValue (state.transitions[transitionIndex].weight, juce::dontSendNotification);
        transitionWeightSlider.setEnabled (true);
        deleteTransitionButton.setEnabled (true);
    }

    void refreshDurationSlider()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;

        durationSlider.setEnabled (false);
        durationSlider.setValue (0.25, juce::dontSendNotification);

        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        durationSlider.setValue (project.states[stateIndex].durationBeats, juce::dontSendNotification);
        durationSlider.setEnabled (true);
    }

    void refreshParameterSelector()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        parameterSelector.clear (juce::dontSendNotification);
        selectedParameterId.clear();

        auto parameters = getSelectedLaneParameters();
        for (int i = 0; i < parameters.size(); ++i)
            parameterSelector.addItem (parameters[i].label, i + 1);

        if (! parameters.isEmpty())
        {
            parameterSelector.setSelectedId (1, juce::dontSendNotification);
            selectedParameterId = parameters[0].label;
        }

        refreshParameterSlider();
    }

    juce::Array<AudioParameterInfo> getSelectedLaneParameters() const
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneName = laneSelector.getText();

        if (stateIndex < 0 || laneName.isEmpty())
            return {};

        return engine.getLaneParameters (stateIndex, laneName);
    }

    void refreshParameterSlider()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        auto parameters = getSelectedLaneParameters();
        auto parameterIndex = parameterSelector.getSelectedId() - 1;

        parameterSlider.setEnabled (false);
        selectedParameterId.clear();

        if (parameterIndex < 0 || parameterIndex >= parameters.size())
        {
            parameterSlider.setRange (0.0, 1.0, 0.01);
            parameterSlider.setValue (0.0, juce::dontSendNotification);
            return;
        }

        auto parameter = parameters[parameterIndex];
        auto interval = parameter.step > 0.0f ? (double) parameter.step : 0.0;
        parameterSlider.setRange ((double) parameter.minimumValue, (double) parameter.maximumValue, interval);
        parameterSlider.setValue ((double) parameter.currentValue, juce::dontSendNotification);
        parameterSlider.setEnabled (true);
        selectedParameterId = parameter.label;
    }

    void refreshLaneInspector()
    {
        auto& project = engine.getProject();
        auto index = stateSelector.getSelectedId() - 1;

        if (index < 0 || index >= project.states.size())
        {
            laneInspector.clear();
            return;
        }

        auto state = project.states[index];
        juce::StringArray lines;

        if (! editMode)
        {
            auto laneStatuses = engine.getLaneStatuses (index);
            auto selectedLane = laneSelector.getText();

            lines.add ("Playing now");
            for (auto& laneStatus : laneStatuses)
            {
                auto meter = juce::jlimit (0.0f, 1.0f, laneStatus.meterPeak);
                auto bars = juce::jlimit (0, 14, (int) std::round (meter * 14.0f));
                juce::String meterText;
                for (int i = 0; i < 14; ++i)
                    meterText += i < bars ? "|" : ".";

                auto marker = laneStatus.laneName == selectedLane ? "> " : "  ";
                auto line = marker + laneStatus.laneName + "  " + meterText;
                if (laneStatus.status != "live")
                    line += "  " + readableLaneStatus (laneStatus.status);
                lines.add (line);
            }

            auto parameters = getSelectedLaneParameters();
            if (! parameters.isEmpty())
            {
                lines.add ("");
                lines.add ("Selected sound");
                auto selectedParameter = parameterSelector.getText();
                for (auto& parameter : parameters)
                    if (parameter.label == selectedParameter || selectedParameter.isEmpty())
                    {
                        lines.add (laneSelector.getText() + "  " + parameter.label + " "
                                   + juce::String (parameter.currentValue, 2));
                        break;
                    }
            }

            laneInspector.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
            return;
        }

        lines.add (state.section + " / " + state.id);
        lines.add ("Length: " + juce::String (state.durationBeats, 2) + " beats");
        lines.add ("Can go next to:");

        for (auto& transition : state.transitions)
            lines.add ("  " + transition.targetState + "  " + juce::String (transition.weight * 100.0, 0) + "%");

        lines.add ("");
        lines.add ("Instruments:");
        auto laneStatuses = engine.getLaneStatuses (index);
        for (auto& lane : state.lanes)
        {
            lines.add ("  " + lane.name + " [" + lane.language + "]");
            lines.add ("    gain " + juce::String (lane.gain, 2) + (lane.muted ? " muted" : ""));

            for (auto& laneStatus : laneStatuses)
            {
                if (laneStatus.laneName == lane.name)
                {
                    auto meter = juce::jlimit (0.0f, 1.0f, laneStatus.meterPeak);
                    auto bars = juce::jlimit (0, 20, (int) std::round (meter * 20.0f));
                    juce::String meterText;
                    for (int i = 0; i < 20; ++i)
                        meterText += i < bars ? "|" : ".";
                    lines.add ("    " + readableLaneStatus (laneStatus.status) + "  " + meterText + "  " + juce::String (meter, 2));
                    if (laneStatus.detail.isNotEmpty())
                        lines.add ("    " + laneStatus.detail);
                    break;
                }
            }

            auto parameters = engine.getLaneParameters (index, lane.name);
            if (! parameters.isEmpty())
            {
                lines.add ("    controls:");
                for (auto& parameter : parameters)
                {
                    auto range = " [" + juce::String (parameter.minimumValue, 2)
                               + "..." + juce::String (parameter.maximumValue, 2) + "]";
                    lines.add ("      " + parameter.label + " = " + juce::String (parameter.currentValue, 3) + range);
                }
            }
            else if (lane.language == "faust")
            {
                lines.add ("    controls will appear after Faust is ready");
            }
        }

        laneInspector.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
    }

    static juce::String readableLaneStatus (const juce::String& statusText)
    {
        if (statusText == "live")
            return "ready";
        if (statusText == "placeholder")
            return "not connected yet";
        if (statusText == "error")
            return "needs attention";
        if (statusText == "silent")
            return "silent";

        return statusText;
    }

    void refreshSelectedStateEditor()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        stateIdEditor.clear();
        stateSectionEditor.clear();
        stateIdEditor.setEnabled (false);
        stateSectionEditor.setEnabled (false);
        applyStateButton.setEnabled (false);
        duplicateStateButton.setEnabled (false);
        deleteStateButton.setEnabled (false);

        auto& project = engine.getProject();
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0 || stateIndex >= project.states.size())
            return;

        auto state = project.states[stateIndex];
        stateIdEditor.setText (state.id, juce::dontSendNotification);
        stateSectionEditor.setText (state.section, juce::dontSendNotification);
        stateIdEditor.setEnabled (true);
        stateSectionEditor.setEnabled (true);
        applyStateButton.setEnabled (true);
        duplicateStateButton.setEnabled (true);
        deleteStateButton.setEnabled (project.states.size() > 1);
    }

    void refreshTransitionTargetSelector()
    {
        const juce::ScopedValueSetter<bool> guard (updatingParameterControls, true);
        transitionTargetSelector.clear (juce::dontSendNotification);

        auto& project = engine.getProject();
        for (int i = 0; i < project.states.size(); ++i)
            transitionTargetSelector.addItem (project.states[i].id, i + 1);

        auto stateIndex = stateSelector.getSelectedId();
        if (stateIndex > 0 && stateIndex <= project.states.size())
            transitionTargetSelector.setSelectedId (stateIndex, juce::dontSendNotification);

        transitionTargetSelector.setEnabled (project.states.size() > 0);
        addTransitionButton.setEnabled (project.states.size() > 0);
    }

    void applySelectedStateEdits()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* states = statesVar.getArray();
        auto* stateObject = states->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto oldId = stateObject->getProperty ("id").toString();
        juce::StringArray existingIds;
        for (int i = 0; i < states->size(); ++i)
            if (auto* existingState = states->getReference (i).getDynamicObject())
                existingIds.add (existingState->getProperty ("id").toString());

        auto newId = stateIdEditor.getText().trim().replace (" ", "_");
        if (newId.isEmpty())
            newId = "section";
        newId = makeUniqueStateId (newId, existingIds, stateIndex);

        auto section = stateSectionEditor.getText().trim();
        if (section.isEmpty())
            section = "Section";

        stateObject->setProperty ("id", newId);
        stateObject->setProperty ("section", section);

        if (newId != oldId)
        {
            for (auto& stateVar : *states)
            {
                auto* anyStateObject = stateVar.getDynamicObject();
                if (anyStateObject == nullptr)
                    continue;

                auto transitionsVar = anyStateObject->getProperty ("transitions");
                if (! transitionsVar.isArray())
                    continue;

                for (auto& transitionVar : *transitionsVar.getArray())
                    if (auto* transitionObject = transitionVar.getDynamicObject())
                        if (transitionObject->getProperty ("to").toString() == oldId)
                            transitionObject->setProperty ("to", newId);
            }
        }

        reloadScriptAndSelect (root, stateIndex, laneSelector.getSelectedId() - 1, transitionSelector.getSelectedId() - 1);
    }

    void addTransitionToSelectedState()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto target = transitionTargetSelector.getText().trim();
        if (target.isEmpty())
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto transitionsVar = stateObject->getProperty ("transitions");
        if (! transitionsVar.isArray())
        {
            transitionsVar = juce::Array<juce::var>();
            stateObject->setProperty ("transitions", transitionsVar);
        }

        auto* transitions = transitionsVar.getArray();
        if (transitions == nullptr)
            return;

        auto* transition = new juce::DynamicObject();
        transition->setProperty ("to", target);
        transition->setProperty ("weight", 1.0);
        transitions->add (transition);

        reloadScriptAndSelect (root, stateIndex, laneSelector.getSelectedId() - 1, transitions->size() - 1);
    }

    void deleteSelectedTransition()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto transitionIndex = transitionSelector.getSelectedId() - 1;
        if (stateIndex < 0 || transitionIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto transitionsVar = stateObject->getProperty ("transitions");
        if (! transitionsVar.isArray() || transitionIndex >= transitionsVar.getArray()->size())
            return;

        auto* transitions = transitionsVar.getArray();
        transitions->remove (transitionIndex);
        auto nextTransitionIndex = transitions->isEmpty() ? -1
                                : juce::jlimit (0, transitions->size() - 1, transitionIndex);
        reloadScriptAndSelect (root, stateIndex, laneSelector.getSelectedId() - 1, nextTransitionIndex);
    }

    void persistParameterToScript (int stateIndex,
                                   const juce::String& laneName,
                                   const juce::String& parameterId,
                                   float value)
    {
        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();

        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex < 0 || stateIndex >= statesVar.getArray()->size())
            return;

        auto stateVar = statesVar.getArray()->getReference (stateIndex);
        auto* stateObject = stateVar.getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto lanesVar = stateObject->getProperty ("lanes");
        if (! lanesVar.isArray())
            return;

        for (auto& laneVar : *lanesVar.getArray())
        {
            auto* laneObject = laneVar.getDynamicObject();
            if (laneObject == nullptr)
                continue;

            if (laneObject->getProperty ("name").toString() != laneName)
                continue;

            if (parameterId.equalsIgnoreCase ("gain"))
                laneObject->setProperty ("gain", value);

            auto paramsVar = laneObject->getProperty ("params");
            if (! paramsVar.isObject())
            {
                paramsVar = new juce::DynamicObject();
                laneObject->setProperty ("params", paramsVar);
            }

            if (auto* paramsObject = paramsVar.getDynamicObject())
                paramsObject->setProperty (parameterId, value);

            showProjectSource (root);
            return;
        }
    }

    void updateSelectedTransitionWeight (float value)
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto transitionIndex = selectedTransitionIndex;

        if (stateIndex < 0 || transitionIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto transitionsVar = stateObject->getProperty ("transitions");
        if (! transitionsVar.isArray() || transitionIndex >= transitionsVar.getArray()->size())
            return;

        auto* transitionObject = transitionsVar.getArray()->getReference (transitionIndex).getDynamicObject();
        if (transitionObject == nullptr)
            return;

        transitionObject->setProperty ("weight", value);
        showProjectSource (root);
        {
            const juce::ScopedLock lock (audioLock);
            engine.setTransitionWeight (stateIndex, transitionIndex, value);
        }

        pendingMapWeightState = stateIndex;
        pendingMapWeightTransition = transitionIndex;
        pendingMapWeightValue = value;
        stateMap.setTransitionWeight (stateIndex, transitionIndex, value);
        refreshStateMap();
    }

    void updateSelectedStateDuration (float value)
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        stateObject->setProperty ("durationBeats", value);
        showProjectSource (root);
        loadScriptInBackground (getScriptText(), "Updating length...", stateIndex + 1, laneSelector.getSelectedId(), transitionSelector.getSelectedId());
    }

    void updateTempo (float value)
    {
        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        rootObject->setProperty ("bpm", value);
        showProjectSource (root);

        auto selectedState = stateSelector.getSelectedId();
        auto selectedLane = laneSelector.getSelectedId();
        auto selectedTransition = transitionSelector.getSelectedId();
        loadScriptInBackground (getScriptText(), "Updating tempo...", selectedState, selectedLane, selectedTransition);
    }

    void addStateToScript()
    {
        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray())
            return;

        auto* states = statesVar.getArray();
        auto newIndex = states->size() + 1;
        auto section = stateTemplateSelector.getText().trim();
        if (section.isEmpty())
            section = "Section";

        auto newId = makeStateIdBase (section) + "_" + juce::String (newIndex);

        juce::StringArray existingIds;
        for (auto& stateVar : *states)
            if (auto* stateObject = stateVar.getDynamicObject())
                existingIds.add (stateObject->getProperty ("id").toString());

        while (existingIds.contains (newId))
        {
            ++newIndex;
            newId = makeStateIdBase (section) + "_" + juce::String (newIndex);
        }

        auto fallbackTarget = states->isEmpty() ? newId
                            : states->getReference (0).getDynamicObject()->getProperty ("id").toString();

        auto* transition = new juce::DynamicObject();
        transition->setProperty ("to", fallbackTarget);
        transition->setProperty ("weight", 1.0);
        juce::Array<juce::var> transitions;
        transitions.add (transition);

        auto* lane = new juce::DynamicObject();
        lane->setProperty ("name", "Sketch");
        lane->setProperty ("language", "minitone");
        lane->setProperty ("gain", 0.35);
        lane->setProperty ("code", "wave=tri freq=440 pulse=1 tone=0.65 pan=0 bpm=112");
        juce::Array<juce::var> lanes;
        lanes.add (lane);

        auto* state = new juce::DynamicObject();
        state->setProperty ("id", newId);
        state->setProperty ("section", section);
        state->setProperty ("durationBeats", makeStateTemplateDuration (section));
        state->setProperty ("transitions", transitions);
        state->setProperty ("lanes", lanes);
        states->add (state);

        showProjectSource (root);
        loadScriptInBackground (getScriptText(), "Adding part...", states->size(), 1, 1);
    }

    void duplicateSelectedState()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* states = statesVar.getArray();
        auto clone = juce::JSON::parse (juce::JSON::toString (states->getReference (stateIndex), true));
        auto* cloneObject = clone.getDynamicObject();
        if (cloneObject == nullptr)
            return;

        juce::StringArray existingIds;
        for (auto& stateVar : *states)
            if (auto* stateObject = stateVar.getDynamicObject())
                existingIds.add (stateObject->getProperty ("id").toString());

        auto copiedId = cloneObject->getProperty ("id").toString() + "_copy";
        cloneObject->setProperty ("id", makeUniqueStateId (copiedId, existingIds, -1));
        states->insert (stateIndex + 1, clone);

        reloadScriptAndSelect (root, stateIndex + 1, 0);
    }

    void deleteSelectedState()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* states = statesVar.getArray();
        if (states->size() <= 1)
            return;

        auto deletedId = states->getReference (stateIndex).getDynamicObject() != nullptr
                       ? states->getReference (stateIndex).getDynamicObject()->getProperty ("id").toString()
                       : juce::String();

        states->remove (stateIndex);
        auto fallbackId = states->getReference (juce::jlimit (0, states->size() - 1, stateIndex)).getDynamicObject()
                        ->getProperty ("id").toString();

        for (auto& stateVar : *states)
        {
            auto* stateObject = stateVar.getDynamicObject();
            if (stateObject == nullptr)
                continue;

            auto transitionsVar = stateObject->getProperty ("transitions");
            if (! transitionsVar.isArray())
                continue;

            for (auto& transitionVar : *transitionsVar.getArray())
                if (auto* transitionObject = transitionVar.getDynamicObject())
                    if (transitionObject->getProperty ("to").toString() == deletedId)
                        transitionObject->setProperty ("to", fallbackId);
        }

        auto nextStateIndex = juce::jlimit (0, states->size() - 1, stateIndex);
        reloadScriptAndSelect (root, nextStateIndex, 0);
    }

    void addLaneToSelectedState()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        if (stateIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto lanesVar = stateObject->getProperty ("lanes");
        if (! lanesVar.isArray())
        {
            lanesVar = juce::Array<juce::var>();
            stateObject->setProperty ("lanes", lanesVar);
        }

        auto* lanes = lanesVar.getArray();
        if (lanes == nullptr)
            return;

        juce::StringArray existingNames;
        for (auto& laneVar : *lanes)
            if (auto* laneObject = laneVar.getDynamicObject())
                existingNames.add (laneObject->getProperty ("name").toString());

        auto language = newLaneLanguageSelector.getText().trim().toLowerCase();
        auto laneTemplate = laneTemplateSelector.getText().trim();
        if (laneTemplate.isNotEmpty())
            language = languageForLaneTemplate (laneTemplate);
        if (language.isEmpty())
            language = "minitone";

        auto newIndex = lanes->size() + 1;
        auto baseName = laneTemplate.isNotEmpty() ? laneTemplate : makeLaneNameBase (language);
        auto newName = baseName + " " + juce::String (newIndex);
        while (existingNames.contains (newName))
        {
            ++newIndex;
            newName = baseName + " " + juce::String (newIndex);
        }

        auto frequency = 220 + (newIndex * 55);

        auto* lane = new juce::DynamicObject();
        lane->setProperty ("name", newName);
        lane->setProperty ("language", language);
        lane->setProperty ("gain", makeLaneTemplateGain (laneTemplate, language));
        lane->setProperty ("code", makeLaneTemplate (language, frequency, laneTemplate));

        if (language == "faust")
        {
            auto* params = new juce::DynamicObject();
            params->setProperty ("freq", frequency);
            params->setProperty ("tonegain", 0.2);
            lane->setProperty ("params", params);
        }
        else if (language == "csound" || language == "cmajor" || language == "chuck" || language == "supercollider")
        {
            auto* params = new juce::DynamicObject();
            params->setProperty ("freq", frequency);
            params->setProperty ("amp", makeLaneTemplateGain (laneTemplate, language) * 0.5);
            params->setProperty ("cutoff", 1400);
            params->setProperty ("duration", laneTemplate.containsIgnoreCase ("pluck") ? 2 : 8);
            lane->setProperty ("params", params);
        }

        lanes->add (lane);

        showProjectSource (root);
        loadScriptInBackground (getScriptText(), "Adding instrument...", stateIndex + 1, lanes->size(), transitionSelector.getSelectedId());
    }

    void applySelectedLaneEdits()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneIndex = laneSelector.getSelectedId() - 1;
        if (stateIndex < 0 || laneIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto lanesVar = stateObject->getProperty ("lanes");
        if (! lanesVar.isArray() || laneIndex >= lanesVar.getArray()->size())
            return;

        auto* lanes = lanesVar.getArray();
        auto* laneObject = lanes->getReference (laneIndex).getDynamicObject();
        if (laneObject == nullptr)
            return;

        juce::StringArray existingNames;
        for (int i = 0; i < lanes->size(); ++i)
            if (auto* existingLane = lanes->getReference (i).getDynamicObject())
                existingNames.add (existingLane->getProperty ("name").toString());

        auto language = laneLanguageSelector.getText().trim().toLowerCase();
        if (language.isEmpty())
            language = laneObject->getProperty ("language").toString().trim().toLowerCase();
        if (language.isEmpty())
            language = "minitone";

        auto name = laneNameEditor.getText().trim();
        if (name.isEmpty())
            name = makeLaneNameBase (language);
        name = makeUniqueLaneName (name, existingNames, laneIndex);

        laneObject->setProperty ("name", name);
        laneObject->setProperty ("language", language);
        laneObject->setProperty ("gain", laneGainSlider.getValue());
        laneObject->setProperty ("muted", laneMuteButton.getToggleState());
        laneObject->setProperty ("code", getLaneCodeText());

        if (language == "faust" && ! laneObject->getProperty ("params").isObject())
        {
            auto* params = new juce::DynamicObject();
            params->setProperty ("freq", 440);
            params->setProperty ("tonegain", 0.2);
            laneObject->setProperty ("params", params);
        }

        reloadScriptAndSelect (root, stateIndex, laneIndex);
    }

    void duplicateSelectedLane()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneIndex = laneSelector.getSelectedId() - 1;
        if (stateIndex < 0 || laneIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto lanesVar = stateObject->getProperty ("lanes");
        if (! lanesVar.isArray() || laneIndex >= lanesVar.getArray()->size())
            return;

        auto* lanes = lanesVar.getArray();
        auto clone = juce::JSON::parse (juce::JSON::toString (lanes->getReference (laneIndex), true));
        auto* cloneObject = clone.getDynamicObject();
        if (cloneObject == nullptr)
            return;

        juce::StringArray existingNames;
        for (auto& laneVar : *lanes)
            if (auto* laneObject = laneVar.getDynamicObject())
                existingNames.add (laneObject->getProperty ("name").toString());

        auto copiedName = cloneObject->getProperty ("name").toString() + " Copy";
        cloneObject->setProperty ("name", makeUniqueLaneName (copiedName, existingNames, -1));
        lanes->insert (laneIndex + 1, clone);

        reloadScriptAndSelect (root, stateIndex, laneIndex + 1);
    }

    void rerenderSelectedLane()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneIndex = laneSelector.getSelectedId() - 1;
        if (stateIndex < 0 || laneIndex < 0)
            return;

        loadScriptInBackground (getScriptText(), "Refreshing sound...", stateIndex + 1, laneIndex + 1, transitionSelector.getSelectedId());
    }

    void deleteSelectedLane()
    {
        auto stateIndex = stateSelector.getSelectedId() - 1;
        auto laneIndex = laneSelector.getSelectedId() - 1;
        if (stateIndex < 0 || laneIndex < 0)
            return;

        auto root = parseVisibleProject();
        auto* rootObject = root.getDynamicObject();
        if (rootObject == nullptr)
            return;

        auto statesVar = rootObject->getProperty ("states");
        if (! statesVar.isArray() || stateIndex >= statesVar.getArray()->size())
            return;

        auto* stateObject = statesVar.getArray()->getReference (stateIndex).getDynamicObject();
        if (stateObject == nullptr)
            return;

        auto lanesVar = stateObject->getProperty ("lanes");
        if (! lanesVar.isArray() || laneIndex >= lanesVar.getArray()->size())
            return;

        auto* lanes = lanesVar.getArray();
        lanes->remove (laneIndex);
        auto nextLaneIndex = juce::jlimit (0, juce::jmax (0, lanes->size() - 1), laneIndex);
        reloadScriptAndSelect (root, stateIndex, lanes->isEmpty() ? -1 : nextLaneIndex);
    }

    void reloadScriptAndSelect (const juce::var& root, int stateIndex, int laneIndex, int transitionIndex = -1)
    {
        setScriptText (MarkovEngine::jsonToReadableScript (root));
        loadScriptInBackground (getScriptText(),
                                "Preparing changes...",
                                stateIndex + 1,
                                laneIndex >= 0 ? laneIndex + 1 : 0,
                                transitionIndex >= 0 ? transitionIndex + 1 : 0);
    }

    static juce::String makeUniqueLaneName (const juce::String& preferred,
                                            const juce::StringArray& existingNames,
                                            int allowedExistingIndex)
    {
        auto name = preferred.trim();
        if (name.isEmpty())
            name = "Lane";

        auto candidate = name;
        auto suffix = 2;

        for (;;)
        {
            auto duplicateIndex = existingNames.indexOf (candidate);
            if (duplicateIndex < 0 || duplicateIndex == allowedExistingIndex)
                return candidate;

            candidate = name + " " + juce::String (suffix++);
        }
    }

    static juce::String makeUniqueStateId (const juce::String& preferred,
                                           const juce::StringArray& existingIds,
                                           int allowedExistingIndex)
    {
        auto id = preferred.trim();
        if (id.isEmpty())
            id = "section";

        auto candidate = id;
        auto suffix = 2;

        for (;;)
        {
            auto duplicateIndex = existingIds.indexOf (candidate);
            if (duplicateIndex < 0 || duplicateIndex == allowedExistingIndex)
                return candidate;

            candidate = id + "_" + juce::String (suffix++);
        }
    }

    static juce::String makeStateIdBase (const juce::String& section)
    {
        auto id = section.trim().toLowerCase().replace (" ", "_");
        id = id.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_");
        return id.isEmpty() ? "section" : id;
    }

    static double makeStateTemplateDuration (const juce::String& section)
    {
        auto text = section.toLowerCase();
        if (text == "intro" || text == "outro") return 8.0;
        if (text == "bridge" || text == "breakdown") return 8.0;
        return 16.0;
    }

    static juce::String makeLaneNameBase (const juce::String& language)
    {
        if (language == "minitone")      return "Sketch";
        if (language == "faust")         return "Faust";
        if (language == "cmajor")        return "Cmajor";
        if (language == "csound")        return "Csound";
        if (language == "chuck")         return "ChucK";
        if (language == "rtcmix")        return "RTcmix";
        if (language == "supercollider") return "SuperCollider";
        return "Lane";
    }

    static juce::String languageForLaneTemplate (const juce::String& laneTemplate)
    {
        auto text = laneTemplate.toLowerCase();
        if (text.startsWith ("faust"))         return "faust";
        if (text.startsWith ("csound"))        return "csound";
        if (text.startsWith ("cmajor"))        return "cmajor";
        if (text.startsWith ("chuck"))         return "chuck";
        if (text.startsWith ("supercollider")) return "supercollider";
        return "minitone";
    }

    static double makeLaneTemplateGain (const juce::String& laneTemplate, const juce::String& language)
    {
        auto text = laneTemplate.toLowerCase();
        if (language == "faust") return 0.25;
        if (language == "csound") return text.contains ("drone") ? 0.22 : 0.35;
        if (language == "chuck") return text.contains ("pulse") ? 0.45 : 0.35;
        if (text.contains ("bass")) return 0.55;
        if (text.contains ("pulse")) return 0.75;
        return 0.3;
    }

    static juce::String makeLaneTemplate (const juce::String& language, int frequency, const juce::String& laneTemplate = {})
    {
        auto templateName = laneTemplate.toLowerCase();

        if (language == "faust")
        {
            if (templateName.contains ("filter"))
                return "import(\"stdfaust.lib\"); freq = hslider(\"freq\", "
                     + juce::String (frequency)
                     + ", 80, 2000, 1); cutoff = hslider(\"cutoff\", 1200, 100, 8000, 1); tonegain = hslider(\"tonegain\", 0.2, 0, 1, 0.01); process = os.osc(freq) : fi.lowpass(3, cutoff) * tonegain;";

            return "import(\"stdfaust.lib\"); freq = hslider(\"freq\", "
                 + juce::String (frequency)
                 + ", 80, 2000, 1); tonegain = hslider(\"tonegain\", 0.2, 0, 1, 0.01); process = os.osc(freq) * tonegain;";
        }

        if (language == "cmajor")
            return "processor MarkovLane [[main]]\n{\n  input value float freq;\n  input value float amp;\n  output stream float out;\n  float phase;\n\n  void main()\n  {\n    loop\n    {\n      out <- sin (phase) * amp;\n      phase = wrap (phase + freq * float (processor.period) * float (twoPi), float (twoPi));\n      advance();\n    }\n  }\n}";

        if (language == "csound")
        {
            if (templateName.contains ("drone"))
                return "instr 1\n  kAmp chnget \"amp\"\n  kFreq chnget \"freq\"\n  kCutoff chnget \"cutoff\"\n  a1 oscili kAmp, kFreq, 1\n  a2 oscili kAmp, kFreq * 1.5, 1\n  aMix butterlp a1 + a2, kCutoff\n  outs aMix, aMix\nendin";

            if (templateName.contains ("pluck"))
                return "instr 1\n  kAmp chnget \"amp\"\n  kFreq chnget \"freq\"\n  aEnv linseg 0, 0.01, 1, p3 - 0.01, 0\n  a1 pluck aEnv * kAmp, kFreq, kFreq, 0, 1\n  outs a1, a1\nendin";

            return "instr 1\n  kAmp chnget \"amp\"\n  kFreq chnget \"freq\"\n  a1 oscili kAmp, kFreq, 1\n  outs a1, a1\nendin";
        }

        if (language == "chuck")
        {
            if (templateName.contains ("pulse"))
                return "global float markov_freq;\nglobal float markov_amp;\nSinOsc osc => ADSR env => dac;\nenv.set(5::ms, 80::ms, 0.0, 20::ms);\nwhile (true)\n{\n  markov_freq => osc.freq;\n  markov_amp => osc.gain;\n  env.keyOn(); 80::ms => now;\n  env.keyOff(); 170::ms => now;\n}";

            return "global float markov_freq;\nglobal float markov_amp;\nSinOsc osc => dac;\nwhile (true)\n{\n  markov_freq => osc.freq;\n  markov_amp => osc.gain;\n  1::samp => now;\n}";
        }

        if (language == "rtcmix")
            return "WAVETABLE(0, 3.5, 20000, " + juce::String (frequency) + ", 0.1)";

        if (language == "supercollider")
            return "RLPF.ar(Saw.ar(freq) * amp, cutoff, 0.25)";

        if (templateName.contains ("bass"))
            return "wave=saw freq=110 pulse=2 tone=0.35 pan=0 bpm=112";

        if (templateName.contains ("pulse"))
            return "wave=sine freq=58 pulse=1 tone=0.20 pan=-0.05 bpm=112";

        return "wave=tri freq=" + juce::String (frequency) + " pulse=4 tone=0.75 pan=0.25 bpm=112";
    }

    static bool renderScriptToWav (const juce::String& scriptText,
                                   const juce::File& file,
                                   const AudioLanguageRegistry& registry,
                                   double seconds,
                                   juce::String& error)
    {
        constexpr double exportSampleRate = 48000.0;
        constexpr int exportChannels = 2;
        constexpr int blockSize = 512;

        auto totalSamples = (int64_t) std::round (exportSampleRate * juce::jmax (0.1, seconds));

        MarkovEngine exportEngine;
        if (! exportEngine.loadFromScript (scriptText, registry, error))
            return false;

        exportEngine.prepare (exportSampleRate, blockSize, exportChannels);
        exportEngine.setPlaying (true);

        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        if (stream == nullptr)
        {
            error = "Could not write " + file.getFullPathName();
            return false;
        }

        juce::WavAudioFormat wavFormat;
        auto writerOptions = juce::AudioFormatWriterOptions()
                                 .withSampleRate (exportSampleRate)
                                 .withNumChannels (exportChannels)
                                 .withBitsPerSample (24);
        auto writer = wavFormat.createWriterFor (stream, writerOptions);
        if (writer == nullptr)
        {
            error = "Could not create WAV writer.";
            return false;
        }

        juce::AudioBuffer<float> buffer (exportChannels, blockSize);
        for (int64_t rendered = 0; rendered < totalSamples;)
        {
            auto samplesThisBlock = (int) juce::jmin ((int64_t) blockSize, totalSamples - rendered);
            buffer.clear();
            exportEngine.render (buffer, 0, samplesThisBlock);
            writer->writeFromAudioSampleBuffer (buffer, 0, samplesThisBlock);
            rendered += samplesThisBlock;
        }

        return true;
    }

    AudioLanguageRegistry languageRegistry;
    MarkovEngine engine;
    juce::CriticalSection audioLock;
    juce::Label title;
    juce::Label mapLabel;
    juce::Label inspectorLabel;
    juce::Label laneCodeLabel;
    juce::Label songScriptLabel;
    juce::TextEditor status;
    StateMapComponent stateMap;
    juce::ComboBox stateSelector;
    juce::ComboBox stateTemplateSelector;
    juce::TextEditor stateIdEditor;
    juce::TextEditor stateSectionEditor;
    juce::ComboBox laneSelector;
    juce::ComboBox newLaneLanguageSelector;
    juce::ComboBox laneTemplateSelector;
    juce::TextEditor laneNameEditor;
    juce::ComboBox laneLanguageSelector;
    juce::Slider laneGainSlider;
    juce::ToggleButton laneMuteButton;
    juce::CodeDocument laneCodeDocument;
    juce::CPlusPlusCodeTokeniser laneCodeTokeniser;
    juce::CodeEditorComponent laneCodeEditor;
    juce::ComboBox parameterSelector;
    juce::Slider parameterSlider;
    juce::ComboBox transitionSelector;
    juce::ComboBox transitionTargetSelector;
    juce::Slider transitionWeightSlider;
    juce::Slider durationSlider;
    juce::Slider tempoSlider;
    juce::TextEditor laneInspector;
    juce::CodeDocument scriptDocument;
    juce::CPlusPlusCodeTokeniser scriptTokeniser;
    juce::CodeEditorComponent scriptEditor;
    juce::TextButton applyButton;
    juce::TextButton demoButton;
    juce::TextButton transportButton;
    juce::TextButton addStateButton;
    juce::TextButton duplicateStateButton;
    juce::TextButton deleteStateButton;
    juce::TextButton addLaneButton;
    juce::TextButton applyStateButton;
    juce::TextButton addTransitionButton;
    juce::TextButton deleteTransitionButton;
    juce::TextButton openProjectButton;
    juce::TextButton saveProjectButton;
    juce::TextButton snapshotProjectButton;
    juce::TextButton exportAudioButton;
    juce::TextButton applyLaneButton;
    juce::TextButton rerenderLaneButton;
    juce::TextButton duplicateLaneButton;
    juce::TextButton deleteLaneButton;
    juce::TextButton editModeButton;
    bool updatingParameterControls = false;
    bool editMode = false;
    juce::String selectedParameterId;
    int selectedTransitionIndex = -1;
    int pendingMapWeightState = -1;
    int pendingMapWeightTransition = -1;
    double pendingMapWeightValue = 0.0;
    juce::File currentProjectFile;
    std::atomic<int> compileRevision { 0 };
    std::atomic<bool> compiling { false };
    double currentSampleRate = 0.0;
    int currentBlockSize = 512;
    int currentOutputChannels = 2;
};

class MarkovStudioApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Markov Music by matd.space"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--faust-smoke"))
        {
            runFaustSmokeTest();
            return;
        }

        if (commandLine.contains ("--csound-smoke"))
        {
            runCsoundSmokeTest();
            return;
        }

        if (commandLine.contains ("--export-smoke"))
        {
            runExportSmokeTest();
            return;
        }

        if (commandLine.contains ("--cmajor-smoke"))
        {
            runCmajorSmokeTest();
            return;
        }

        if (commandLine.contains ("--chuck-smoke"))
        {
            runChuckSmokeTest();
            return;
        }

        if (commandLine.contains ("--supercollider-smoke"))
        {
            runSuperColliderSmokeTest();
            return;
        }

        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name),
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    static void runFaustSmokeTest()
    {
        AudioLanguageRegistry registry;
        LaneDefinition lane;
        lane.name = "Faust smoke";
        lane.language = "faust";
        lane.gain = 0.5f;
        lane.code = "import(\"stdfaust.lib\"); freq = hslider(\"freq\", 440, 80, 2000, 1); tonegain = hslider(\"tonegain\", 0.1, 0, 1, 0.01); process = os.osc(freq) * tonegain;";

        juce::String error;
        auto program = registry.compile (lane, error);

        if (program == nullptr)
        {
            std::cout << "Faust smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        program->prepare (48000.0, 512, 2);
        program->reset();
        program->setParameter ("freq", 660.0f);
        program->setParameter ("tonegain", 0.2f);
        program->render (buffer, 0, 512);

        auto peak = buffer.getMagnitude (0, 512);
        std::cout << "Faust smoke passed. Peak: " << peak << std::endl;
        std::exit (peak > 0.0f ? 0 : 2);
    }

    static void runCsoundSmokeTest()
    {
        AudioLanguageRegistry registry;
        LaneDefinition lane;
        lane.name = "Csound smoke";
        lane.language = "csound";
        lane.gain = 0.5f;
        lane.code = "instr 1\n  kAmp chnget \"amp\"\n  kFreq chnget \"freq\"\n  a1 oscili kAmp, kFreq, 1\n  outs a1, a1\nendin";
        lane.params.set ("amp", "0.2");
        lane.params.set ("freq", "330");
        lane.params.set ("duration", "1");

        juce::String error;
        auto program = registry.compile (lane, error);

        if (program == nullptr)
        {
            std::cout << "Csound smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioBuffer<float> buffer (2, 48000);
        buffer.clear();
        program->prepare (48000.0, 512, 2);
        program->reset();
        program->setParameter ("freq", 440.0f);
        program->render (buffer, 0, 48000);

        auto peak = juce::jmax (buffer.getMagnitude (0, 48000), buffer.getMagnitude (1, 48000));
        std::cout << "Csound smoke passed. Peak: " << peak << std::endl;
        std::exit (peak > 0.0f ? 0 : 2);
    }

    static void runCmajorSmokeTest()
    {
        AudioLanguageRegistry registry;
        LaneDefinition lane;
        lane.name = "Cmajor smoke";
        lane.language = "cmajor";
        lane.gain = 0.5f;
        lane.params.set ("freq", "440");
        lane.params.set ("amp", "0.2");
        lane.code = "processor MarkovLane [[main]]\n"
                    "{\n"
                    "  input value float freq;\n"
                    "  input value float amp;\n"
                    "  output stream float out;\n"
                    "  float phase;\n"
                    "  void main()\n"
                    "  {\n"
                    "    loop\n"
                    "    {\n"
                    "      out <- sin (phase) * amp;\n"
                    "      phase = wrap (phase + freq * float (processor.period) * float (twoPi), float (twoPi));\n"
                    "      advance();\n"
                    "    }\n"
                    "  }\n"
                    "}";

        juce::String error;
        auto program = registry.compile (lane, error);

        if (program == nullptr)
        {
            std::cout << "Cmajor smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioBuffer<float> buffer (2, 48000);
        buffer.clear();
        program->prepare (48000.0, 512, 2);
        program->reset();
        program->setParameter ("freq", 660.0f);
        program->render (buffer, 0, 48000);

        auto peak = juce::jmax (buffer.getMagnitude (0, 48000), buffer.getMagnitude (1, 48000));
        std::cout << "Cmajor smoke passed. Peak: " << peak << std::endl;
        std::exit (peak > 0.0f ? 0 : 2);
    }

    static void runChuckSmokeTest()
    {
        AudioLanguageRegistry registry;
        LaneDefinition lane;
        lane.name = "ChucK smoke";
        lane.language = "chuck";
        lane.gain = 0.5f;
        lane.params.set ("freq", "440");
        lane.params.set ("amp", "0.2");
        lane.code = "global float markov_freq;\n"
                    "global float markov_amp;\n"
                    "SinOsc osc => dac;\n"
                    "while (true)\n"
                    "{\n"
                    "  markov_freq => osc.freq;\n"
                    "  markov_amp => osc.gain;\n"
                    "  1::samp => now;\n"
                    "}";

        juce::String error;
        auto program = registry.compile (lane, error);

        if (program == nullptr)
        {
            std::cout << "ChucK smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioBuffer<float> buffer (2, 48000);
        buffer.clear();
        program->prepare (48000.0, 512, 2);
        program->reset();
        program->setParameter ("freq", 660.0f);
        program->render (buffer, 0, 24000);
        program->setParameter ("freq", 330.0f);
        program->setParameter ("amp", 0.1f);
        program->render (buffer, 24000, 24000);

        auto peak = juce::jmax (buffer.getMagnitude (0, 48000), buffer.getMagnitude (1, 48000));
        std::cout << "ChucK smoke passed. Peak: " << peak << "\n"
                  << program->describe() << std::endl;
        std::exit (peak > 0.0f ? 0 : 2);
    }

    static void runSuperColliderSmokeTest()
    {
        AudioLanguageRegistry registry;
        LaneDefinition lane;
        lane.name = "SuperCollider smoke";
        lane.language = "supercollider";
        lane.gain = 0.5f;
        lane.params.set ("freq", "440");
        lane.params.set ("amp", "0.2");
        lane.params.set ("duration", "1");
        lane.code = "SinOsc.ar(freq) * amp";

        juce::String error;
        auto program = registry.compile (lane, error);

        if (program == nullptr)
        {
            std::cout << "SuperCollider smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioBuffer<float> buffer (2, 48000);
        buffer.clear();
        program->prepare (48000.0, 512, 2);
        program->reset();
        program->setParameter ("freq", 660.0f);
        for (int offset = 0; offset < 48000; offset += 512)
            program->render (buffer, offset, juce::jmin (512, 48000 - offset));

        auto peak = juce::jmax (buffer.getMagnitude (0, 48000), buffer.getMagnitude (1, 48000));
        std::cout << (peak > 0.0f ? "SuperCollider smoke passed. Peak: "
                                  : "SuperCollider smoke failed: silent. Peak: ")
                  << peak << "\n"
                  << program->describe() << std::endl;
        program.reset();
        std::exit (peak > 0.0f ? 0 : 2);
    }

    static void runExportSmokeTest()
    {
        AudioLanguageRegistry registry;
        auto output = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("markov-export-smoke.wav");
        output.deleteFile();

        juce::String error;
        if (! MainComponent::renderScriptToWav (MarkovEngine::makeDemoScript(), output, registry, 18.0, error))
        {
            std::cout << "Export smoke failed:\n" << error << std::endl;
            std::exit (1);
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (output));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            std::cout << "Export smoke failed: could not read " << output.getFullPathName() << std::endl;
            std::exit (2);
        }

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);

        auto peak = buffer.getMagnitude (0, buffer.getNumSamples());
        auto passed = peak > 0.0001f;
        std::cout << (passed ? "Export smoke passed. File: " : "Export smoke failed: silent file: ")
                  << output.getFullPathName()
                  << " Peak: " << peak << std::endl;
        std::exit (passed ? 0 : 3);
    }

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MarkovStudioApplication)
