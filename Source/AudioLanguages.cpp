#include "AudioLanguages.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#if MARKOV_HAS_CHUCK_CORE
#include "chuck.h"
#include "chuck_globals.h"
#endif

namespace
{
static std::atomic<int> nextSuperColliderAuSlot { 0 };

static float getFloatToken (const juce::StringPairArray& tokens, const juce::String& key, float fallback)
{
    return tokens.getValue (key, juce::String (fallback)).getFloatValue();
}

static juce::String getStringToken (const juce::StringPairArray& tokens, const juce::String& key, const juce::String& fallback)
{
    auto value = tokens.getValue (key, {});
    return value.isEmpty() ? fallback : value;
}

static juce::StringPairArray parseTinyCode (const juce::String& code)
{
    juce::StringPairArray tokens;
    auto normalised = code.replaceCharacters ("\n\r,;", "    ");
    juce::StringArray parts;
    parts.addTokens (normalised, " \t", "\"'");

    for (auto part : parts)
    {
        auto split = part.indexOfChar ('=');
        if (split > 0)
            tokens.set (part.substring (0, split).trim().toLowerCase(),
                        part.substring (split + 1).trim().unquoted());
    }

    return tokens;
}

static juce::File findExecutableOnPath (const juce::String& executableName)
{
#if defined(MARKOV_BUNDLED_FAUST_BIN)
    if (executableName == "faust")
    {
        auto bundledFaust = juce::File (MARKOV_BUNDLED_FAUST_BIN);
        if (bundledFaust.existsAsFile())
            return bundledFaust;
    }
#endif

#if defined(MARKOV_BUNDLED_CSOUND_BIN)
    if (executableName == "csound")
    {
        auto bundledCsound = juce::File (MARKOV_BUNDLED_CSOUND_BIN);
        if (bundledCsound.existsAsFile())
            return bundledCsound;
    }
#endif

#if defined(MARKOV_BUNDLED_CMAJ_BIN)
    if (executableName == "cmaj")
    {
        auto bundledCmaj = juce::File (MARKOV_BUNDLED_CMAJ_BIN);
        if (bundledCmaj.existsAsFile())
            return bundledCmaj;
    }
#endif

#if defined(MARKOV_BUNDLED_CHUCK_BIN)
    if (executableName == "chuck")
    {
        auto bundledChuck = juce::File (MARKOV_BUNDLED_CHUCK_BIN);
        if (bundledChuck.existsAsFile())
            return bundledChuck;
    }
#endif

    if (executableName == "sclang")
    {
        for (auto path : { "/Applications/SuperCollider.app/Contents/MacOS/sclang" })
        {
            auto candidate = juce::File (path);
            if (candidate.existsAsFile())
                return candidate;
        }
    }

    if (auto* path = std::getenv ("PATH"))
    {
        juce::StringArray paths;
        paths.addTokens (juce::String::fromUTF8 (path), ":", {});

        for (auto pathEntry : paths)
        {
            auto candidate = juce::File (pathEntry).getChildFile (executableName);
            if (candidate.existsAsFile())
                return candidate;
        }
    }

    for (auto location : { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" })
    {
        auto candidate = juce::File (location).getChildFile (executableName);
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

static bool shouldUseSuperColliderAu()
{
    if (auto* value = std::getenv ("MARKOV_SUPERCOLLIDER_AU"))
        return juce::String::fromUTF8 (value).trim() == "1";

    return false;
}

struct RenderedAudioCacheEntry
{
    juce::String key;
    juce::AudioBuffer<float> audio;
};

static constexpr size_t renderedAudioCacheMaxBytes = 256u * 1024u * 1024u;

static juce::CriticalSection& getRenderedAudioCacheLock()
{
    static juce::CriticalSection lock;
    return lock;
}

static std::vector<RenderedAudioCacheEntry>& getRenderedAudioCache()
{
    static std::vector<RenderedAudioCacheEntry> cache;
    return cache;
}

static juce::String makeRenderedAudioCacheKey (const juce::String& language,
                                               const LaneDefinition& lane,
                                               const juce::File& executable,
                                               double sampleRate,
                                               int numChannels)
{
    juce::String key = language + "\n"
                     + executable.getFullPathName() + "\n"
                     + lane.code + "\n"
                     + juce::String ((int) std::round (sampleRate)) + "\n"
                     + juce::String (numChannels) + "\n";

    auto paramKeys = lane.params.getAllKeys();
    paramKeys.sort (true);

    for (auto paramKey : paramKeys)
        key += paramKey + "=" + lane.params.getValue (paramKey, {}) + "\n";

    return key;
}

static bool loadRenderedAudioFromCache (const juce::String& key, juce::AudioBuffer<float>& destination)
{
    const juce::ScopedLock scopedLock (getRenderedAudioCacheLock());
    auto& cache = getRenderedAudioCache();
    for (auto& entry : cache)
    {
        if (entry.key == key)
        {
            destination.makeCopyOf (entry.audio);
            return true;
        }
    }

    return false;
}

static size_t getRenderedAudioCacheEntryBytes (const RenderedAudioCacheEntry& entry)
{
    return (size_t) juce::jmax (0, entry.audio.getNumChannels())
         * (size_t) juce::jmax (0, entry.audio.getNumSamples())
         * sizeof (float);
}

static size_t getRenderedAudioCacheBytes (const std::vector<RenderedAudioCacheEntry>& cache)
{
    size_t bytes = 0;
    for (auto& entry : cache)
        bytes += getRenderedAudioCacheEntryBytes (entry);

    return bytes;
}

static void storeRenderedAudioInCache (const juce::String& key, const juce::AudioBuffer<float>& audio)
{
    const juce::ScopedLock scopedLock (getRenderedAudioCacheLock());
    auto& cache = getRenderedAudioCache();
    for (auto& entry : cache)
    {
        if (entry.key == key)
        {
            entry.audio.makeCopyOf (audio);
            return;
        }
    }

    cache.push_back ({ key, {} });
    cache.back().audio.makeCopyOf (audio);

    while (! cache.empty()
           && (cache.size() > 48 || getRenderedAudioCacheBytes (cache) > renderedAudioCacheMaxBytes))
        cache.erase (cache.begin());
}

static juce::File findBundledCsoundLibrary()
{
#if defined(MARKOV_BUNDLED_CSOUND_LIB)
    auto bundled = juce::File (MARKOV_BUNDLED_CSOUND_LIB);
    if (bundled.existsAsFile())
        return bundled;
#endif

#if defined(MARKOV_BUNDLED_CSOUND_BIN)
    auto bin = juce::File (MARKOV_BUNDLED_CSOUND_BIN);
    auto bundledFromBin = bin.getParentDirectory().getParentDirectory()
                             .getChildFile ("Library/Frameworks/CsoundLib64.framework/Versions/6.0/CsoundLib64");
    if (bundledFromBin.existsAsFile())
        return bundledFromBin;
#endif

    for (auto path : { "/Library/Frameworks/CsoundLib64.framework/CsoundLib64",
                       "/opt/homebrew/Frameworks/CsoundLib64.framework/CsoundLib64",
                       "/usr/local/Frameworks/CsoundLib64.framework/CsoundLib64" })
    {
        auto candidate = juce::File (path);
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

static juce::String runProcess (const juce::StringArray& args, int& exitCode, int timeoutMs = 120000)
{
    juce::ChildProcess process;
    exitCode = -1;

    if (! process.start (args))
        return "Could not start process: " + args.joinIntoString (" ");

    juce::MemoryOutputStream output;
    char buffer[4096];
    auto startTime = juce::Time::getMillisecondCounterHiRes();

    while (process.isRunning())
    {
        auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer));
        if (bytesRead > 0)
            output.write (buffer, (size_t) bytesRead);
        else
            std::this_thread::sleep_for (std::chrono::milliseconds (5));

        if (juce::Time::getMillisecondCounterHiRes() - startTime > (double) timeoutMs)
        {
            process.kill();
            exitCode = -2;
            return output.toString() + "\nProcess timed out: " + args.joinIntoString (" ");
        }
    }

    for (;;)
    {
        auto bytesRead = process.readProcessOutput (buffer, (int) sizeof (buffer));
        if (bytesRead <= 0)
            break;

        output.write (buffer, (size_t) bytesRead);
    }

    exitCode = (int) process.getExitCode();
    return output.toString();
}

static juce::String cIncludePath (const juce::File& file)
{
    return file.getFullPathName().replace ("\\", "\\\\").replace ("\"", "\\\"");
}

static juce::String scStringLiteral (juce::String text)
{
    return "\"" + text.replace ("\\", "\\\\").replace ("\"", "\\\"") + "\"";
}

static juce::String normaliseParameterId (const juce::String& parameterId)
{
    auto id = parameterId.fromLastOccurrenceOf ("/", false, false).trim().toLowerCase();
    return id.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_-. ");
}

static juce::String expandCodeParameters (juce::String code, const juce::StringPairArray& params)
{
    for (auto& key : params.getAllKeys())
    {
        auto value = params.getValue (key, {});
        code = code.replace ("{{" + key + "}}", value);
        code = code.replace ("$" + key, value);
    }

    return code;
}

static void addParameterInfo (juce::Array<AudioParameterInfo>& result,
                              const juce::String& id,
                              float value,
                              float min,
                              float max,
                              float step)
{
    AudioParameterInfo info;
    info.id = id;
    info.label = id;
    info.currentValue = value;
    info.defaultValue = value;
    info.minimumValue = min;
    info.maximumValue = max;
    info.step = step;
    result.add (info);
}

static juce::Array<AudioParameterInfo> makeRenderedLaneParameters (const LaneDefinition& lane)
{
    juce::Array<AudioParameterInfo> result;
    addParameterInfo (result, "gain", lane.gain, 0.0f, 1.5f, 0.01f);
    addParameterInfo (result, "freq", lane.params.getValue ("freq", "330").getFloatValue(), 20.0f, 5000.0f, 1.0f);
    addParameterInfo (result, "amp", lane.params.getValue ("amp", "0.15").getFloatValue(), 0.0f, 1.0f, 0.01f);
    addParameterInfo (result, "cutoff", lane.params.getValue ("cutoff", "1400").getFloatValue(), 40.0f, 12000.0f, 1.0f);
    addParameterInfo (result, "duration", lane.params.getValue ("duration", "8").getFloatValue(), 0.25f, 32.0f, 0.25f);
    return result;
}

class MiniToneProgram final : public AudioProgram
{
public:
    explicit MiniToneProgram (LaneDefinition laneToUse)
        : lane (std::move (laneToUse))
    {
        auto tokens = parseTinyCode (lane.code);
        waveform = getStringToken (tokens, "wave", "sine").toLowerCase();
        baseFrequency = juce::jlimit (20.0f, 12000.0f, getFloatToken (tokens, "freq", 220.0f));
        pulseBeats = juce::jmax (0.125f, getFloatToken (tokens, "pulse", 1.0f));
        tone = juce::jlimit (0.0f, 1.0f, getFloatToken (tokens, "tone", 0.45f));
        pan = juce::jlimit (-1.0f, 1.0f, getFloatToken (tokens, "pan", 0.0f));
        bpm = juce::jlimit (20.0f, 300.0f, getFloatToken (tokens, "bpm", 112.0f));
    }

    void prepare (double sampleRateToUse, int, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        numChannels = numChannelsToUse;
        phase = 0.0;
        pulsePhase = 0.0;
        filterState.clear();
        filterState.resize ((size_t) juce::jmax (1, numChannels));
    }

    void reset() override
    {
        phase = 0.0;
        pulsePhase = 0.0;
        std::fill (filterState.begin(), filterState.end(), 0.0f);
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.toLowerCase();

        if (id == "gain")
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
        else if (id == "freq" || id == "frequency")
            baseFrequency = juce::jlimit (20.0f, 12000.0f, value);
        else if (id == "tone")
            tone = juce::jlimit (0.0f, 1.0f, value);
        else if (id == "pan")
            pan = juce::jlimit (-1.0f, 1.0f, value);
        else if (id == "pulse")
            pulseBeats = juce::jmax (0.125f, value);
        else if (id == "bpm")
            bpm = juce::jlimit (20.0f, 300.0f, value);
        else
            return false;

        return true;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        juce::Array<AudioParameterInfo> result;

        auto add = [&] (const juce::String& id, float value, float min, float max, float step)
        {
            AudioParameterInfo info;
            info.id = id;
            info.label = id;
            info.currentValue = value;
            info.defaultValue = value;
            info.minimumValue = min;
            info.maximumValue = max;
            info.step = step;
            result.add (info);
        };

        add ("gain", lane.gain, 0.0f, 1.5f, 0.01f);
        add ("freq", baseFrequency, 20.0f, 12000.0f, 1.0f);
        add ("tone", tone, 0.0f, 1.0f, 0.01f);
        add ("pan", pan, -1.0f, 1.0f, 0.01f);
        add ("pulse", pulseBeats, 0.125f, 16.0f, 0.125f);
        add ("bpm", bpm, 20.0f, 300.0f, 1.0f);
        return result;
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (sampleRate <= 0.0 || lane.muted)
            return;

        auto secondsPerPulse = (60.0 / (double) bpm) * (double) pulseBeats;
        auto pulseIncrement = 1.0 / (secondsPerPulse * sampleRate);
        auto phaseIncrement = juce::MathConstants<double>::twoPi * (double) baseFrequency / sampleRate;
        auto cutoff = 180.0f + tone * 7800.0f;
        auto filterAlpha = juce::jlimit (0.001f, 0.6f, cutoff / (float) sampleRate);
        auto leftGain = lane.gain * std::sqrt (0.5f * (1.0f - pan));
        auto rightGain = lane.gain * std::sqrt (0.5f * (1.0f + pan));

        for (int sample = 0; sample < numSamples; ++sample)
        {
            auto env = pulsePhase < 0.08 ? (float) (pulsePhase / 0.08)
                     : pulsePhase < 0.65 ? (float) std::exp (-4.0 * (pulsePhase - 0.08))
                                          : 0.0f;

            float raw = 0.0f;
            if (waveform == "saw")
                raw = (float) ((phase / juce::MathConstants<double>::twoPi) * 2.0 - 1.0);
            else if (waveform == "square")
                raw = phase < juce::MathConstants<double>::pi ? 1.0f : -1.0f;
            else if (waveform == "tri")
                raw = (float) (2.0 / juce::MathConstants<double>::pi * std::asin (std::sin (phase)));
            else
                raw = (float) std::sin (phase);

            auto value = raw * env;
            phase += phaseIncrement;
            pulsePhase += pulseIncrement;

            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            if (pulsePhase >= 1.0)
                pulsePhase -= 1.0;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto gain = channel == 0 ? leftGain : rightGain;
                auto& z = filterState[(size_t) channel % filterState.size()];
                z += filterAlpha * (value - z);
                buffer.addSample (channel, startSample + sample, z * gain);
            }
        }
    }

    juce::String describe() const override
    {
        return "MiniTone " + waveform + " " + juce::String (baseFrequency, 1) + " Hz"
             + " gain " + juce::String (lane.gain, 2);
    }

private:
    LaneDefinition lane;
    juce::String waveform;
    float baseFrequency = 220.0f;
    float pulseBeats = 1.0f;
    float tone = 0.45f;
    float pan = 0.0f;
    float bpm = 112.0f;
    double sampleRate = 0.0;
    int numChannels = 2;
    double phase = 0.0;
    double pulsePhase = 0.0;
    std::vector<float> filterState;
};

class MiniToneHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "minitone"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
        error.clear();
        return std::make_unique<MiniToneProgram> (lane);
    }
};

class CsoundLiveProgram final : public AudioProgram
{
public:
    CsoundLiveProgram (LaneDefinition laneToUse,
                       std::shared_ptr<juce::DynamicLibrary> libraryToUse,
                       juce::String libraryPathToUse,
                       juce::String& error)
        : lane (std::move (laneToUse)),
          library (std::move (libraryToUse)),
          libraryPath (std::move (libraryPathToUse))
    {
        loadApi (error);
    }

    ~CsoundLiveProgram() override
    {
        if (csound != nullptr && destroy != nullptr)
            destroy (csound);
    }

    void prepare (double sampleRateToUse, int, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        numChannels = juce::jmax (1, numChannelsToUse);
        restartEngine();
    }

    void reset() override
    {
        restartEngine();
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
            return false;

        lane.params.set (id, juce::String (value, 6));
        if (csound != nullptr && setControlChannel != nullptr)
        {
            const juce::ScopedLock lock (csoundLock);
            setControlChannel (csound, id.toRawUTF8(), (CsoundFloat) value);
        }

        return true;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (lane.muted || csound == nullptr || finished)
            return;

        const juce::ScopedLock lock (csoundLock);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (spout == nullptr || blockCursor >= ksmps)
            {
                if (performKsmps (csound) != 0)
                {
                    finished = true;
                    return;
                }

                spout = getSpout (csound);
                blockCursor = 0;
            }

            if (spout == nullptr)
                return;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto sourceChannel = channel % csoundOutputChannels;
                auto value = (float) (spout[(size_t) blockCursor * (size_t) csoundOutputChannels + (size_t) sourceChannel]
                                      / zeroDbfs)
                           * lane.gain;
                buffer.addSample (channel, startSample + sample, value);
            }

            ++blockCursor;
        }
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return "Csound live error: " + lastError;

        return juce::String ("Csound live engine")
             + " gain " + juce::String (lane.gain, 2);
    }

private:
    using CsoundFloat = double;
    using CreateFn = void* (*) (void*);
    using DestroyFn = void (*) (void*);
    using CompileCsdTextFn = int (*) (void*, const char*);
    using StartFn = int (*) (void*);
    using PerformKsmpsFn = int (*) (void*);
    using GetSpoutFn = CsoundFloat* (*) (void*);
    using GetKsmpsFn = uint32_t (*) (void*);
    using GetNchnlsFn = uint32_t (*) (void*);
    using Get0dBfsFn = CsoundFloat (*) (void*);
    using SetControlChannelFn = void (*) (void*, const char*, CsoundFloat);
    using SetOpcodedirFn = void (*) (const char*);
    using GetSizeOfMyfltFn = int (*)();

    template <typename Fn>
    Fn loadFunction (const char* name, juce::String& error)
    {
        auto* fn = library != nullptr ? library->getFunction (name) : nullptr;
        if (fn == nullptr && error.isEmpty())
            error = "Missing Csound symbol: " + juce::String (name);

        return reinterpret_cast<Fn> (fn);
    }

    void loadApi (juce::String& error)
    {
        if (library == nullptr)
        {
            error = "Csound library is not loaded.";
            return;
        }

        create = loadFunction<CreateFn> ("csoundCreate", error);
        destroy = loadFunction<DestroyFn> ("csoundDestroy", error);
        compileCsdText = loadFunction<CompileCsdTextFn> ("csoundCompileCsdText", error);
        start = loadFunction<StartFn> ("csoundStart", error);
        performKsmps = loadFunction<PerformKsmpsFn> ("csoundPerformKsmps", error);
        getSpout = loadFunction<GetSpoutFn> ("csoundGetSpout", error);
        getKsmps = loadFunction<GetKsmpsFn> ("csoundGetKsmps", error);
        getNchnls = loadFunction<GetNchnlsFn> ("csoundGetNchnls", error);
        get0dBfs = loadFunction<Get0dBfsFn> ("csoundGet0dBFS", error);
        setControlChannel = loadFunction<SetControlChannelFn> ("csoundSetControlChannel", error);
        setOpcodedir = loadFunction<SetOpcodedirFn> ("csoundSetOpcodedir", error);
        auto getSizeOfMyflt = loadFunction<GetSizeOfMyfltFn> ("csoundGetSizeOfMYFLT", error);

        if (error.isEmpty() && getSizeOfMyflt != nullptr && getSizeOfMyflt() != (int) sizeof (CsoundFloat))
            error = "This Csound build uses an unsupported sample type.";

        if (error.isEmpty() && setOpcodedir != nullptr)
        {
            auto opcodes = juce::File (libraryPath).getParentDirectory().getChildFile ("Resources/Opcodes64");
            if (opcodes.isDirectory())
                setOpcodedir (opcodes.getFullPathName().toRawUTF8());
        }
    }

    void restartEngine()
    {
        const juce::ScopedLock lock (csoundLock);
        lastError.clear();
        finished = false;
        blockCursor = 0;
        ksmps = 32;
        csoundOutputChannels = numChannels;
        spout = nullptr;

        if (csound != nullptr && destroy != nullptr)
        {
            destroy (csound);
            csound = nullptr;
        }

        if (create == nullptr || compileCsdText == nullptr || start == nullptr)
        {
            lastError = "Csound API was not loaded.";
            return;
        }

        csound = create (nullptr);
        if (csound == nullptr)
        {
            lastError = "Could not create Csound engine.";
            return;
        }

        auto csdText = makeCsdText();
        auto compileResult = compileCsdText (csound, csdText.toRawUTF8());
        if (compileResult != 0)
        {
            lastError = "Csound compile failed.";
            finished = true;
            return;
        }

        if (start (csound) != 0)
        {
            lastError = "Csound start failed.";
            finished = true;
            return;
        }

        ksmps = juce::jmax (1, (int) getKsmps (csound));
        csoundOutputChannels = juce::jmax (1, (int) getNchnls (csound));
        zeroDbfs = juce::jmax ((CsoundFloat) 0.000001, get0dBfs (csound));
        blockCursor = ksmps;
        applyControlChannels();
    }

    void applyControlChannels()
    {
        if (csound == nullptr || setControlChannel == nullptr)
            return;

        for (auto key : lane.params.getAllKeys())
            setControlChannel (csound,
                               key.trim().toLowerCase().toRawUTF8(),
                               (CsoundFloat) lane.params.getValue (key, {}).getDoubleValue());
    }

    juce::String makeCsdText() const
    {
        if (lane.code.containsIgnoreCase ("<CsoundSynthesizer>"))
            return expandCodeParameters (lane.code, lane.params);

        auto scoreDuration = 3600.0f;

        return "<CsoundSynthesizer>\n"
               "<CsOptions>\n"
               "-n -d\n"
               "</CsOptions>\n"
               "<CsInstruments>\n"
               "sr = " + juce::String ((int) std::round (sampleRate)) + "\n"
               "ksmps = 32\n"
               "nchnls = " + juce::String (numChannels) + "\n"
               "0dbfs = 1\n\n"
               + expandCodeParameters (lane.code, lane.params) + "\n"
               "</CsInstruments>\n"
               "<CsScore>\n"
               "f 1 0 16384 10 1\n"
               "i 1 0 " + juce::String (scoreDuration, 3) + "\n"
               "e\n"
               "</CsScore>\n"
               "</CsoundSynthesizer>\n";
    }

    LaneDefinition lane;
    std::shared_ptr<juce::DynamicLibrary> library;
    juce::String libraryPath;
    juce::CriticalSection csoundLock;
    juce::String lastError;
    double sampleRate = 44100.0;
    int numChannels = 2;
    int ksmps = 32;
    int csoundOutputChannels = 2;
    int blockCursor = 32;
    bool finished = false;
    CsoundFloat zeroDbfs = 1.0;
    CsoundFloat* spout = nullptr;
    void* csound = nullptr;
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    CompileCsdTextFn compileCsdText = nullptr;
    StartFn start = nullptr;
    PerformKsmpsFn performKsmps = nullptr;
    GetSpoutFn getSpout = nullptr;
    GetKsmpsFn getKsmps = nullptr;
    GetNchnlsFn getNchnls = nullptr;
    Get0dBfsFn get0dBfs = nullptr;
    SetControlChannelFn setControlChannel = nullptr;
    SetOpcodedirFn setOpcodedir = nullptr;
};

class CsoundCliHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "csound"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
        auto libraryFile = findBundledCsoundLibrary();
        if (! libraryFile.existsAsFile())
        {
            error = "Csound library was not found. Install Csound, then this adapter can run live lane code.";
            return {};
        }

        auto library = std::make_shared<juce::DynamicLibrary>();
        if (! library->open (libraryFile.getFullPathName()))
        {
            error = "Could not open Csound library: " + libraryFile.getFullPathName();
            return {};
        }

        error.clear();
        auto program = std::make_unique<CsoundLiveProgram> (lane, library, libraryFile.getFullPathName(), error);
        return error.isEmpty() ? std::move (program) : nullptr;
    }
};

class CmajorRenderedProgram final : public AudioProgram
{
public:
    CmajorRenderedProgram (LaneDefinition laneToUse, juce::File cmajExecutableToUse)
        : lane (std::move (laneToUse)), cmajExecutable (std::move (cmajExecutableToUse))
    {
    }

    void prepare (double sampleRateToUse, int, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        numChannels = juce::jmax (1, numChannelsToUse);
        playhead = 0;
        renderLane();
    }

    void reset() override
    {
        playhead = 0;
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
            return false;

        lane.params.set (id, juce::String (value, 6));
        renderLane();
        return true;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (lane.muted || rendered.getNumSamples() == 0)
            return;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto sourceChannel = channel % rendered.getNumChannels();
                buffer.addSample (channel, startSample + sample, rendered.getSample (sourceChannel, playhead) * lane.gain);
            }

            playhead = (playhead + 1) % rendered.getNumSamples();
        }
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return "Cmajor render error: " + lastError;

        return "Cmajor rendered loop " + juce::String (rendered.getNumSamples()) + " samples"
             + " gain " + juce::String (lane.gain, 2);
    }

private:
    void renderLane()
    {
        lastError.clear();
        rendered.setSize (numChannels, 0);

        auto cacheKey = makeRenderedAudioCacheKey ("cmajor", lane, cmajExecutable, sampleRate, numChannels);
        if (loadRenderedAudioFromCache (cacheKey, rendered))
            return;

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getNonexistentChildFile ("markov-cmajor-lane", {});

        if (! workingDir.createDirectory())
        {
            lastError = "could not create temp folder";
            return;
        }

        auto sourceFile = workingDir.getChildFile ("MarkovLane.cmajor");
        auto patchFile = workingDir.getChildFile ("MarkovLane.cmajorpatch");
        auto wavFile = workingDir.getChildFile ("lane.wav");

        if (! sourceFile.replaceWithText (expandCodeParameters (lane.code, lane.params)))
        {
            lastError = "could not write Cmajor source";
            return;
        }

        auto patchText = "{\n"
                         "  \"CmajorVersion\": 1,\n"
                         "  \"ID\": \"com.markov.lane\",\n"
                         "  \"version\": \"1.0\",\n"
                         "  \"name\": \"Markov Lane\",\n"
                         "  \"category\": \"generator\",\n"
                         "  \"manufacturer\": \"Markov\",\n"
                         "  \"isInstrument\": true,\n"
                         "  \"source\": \"MarkovLane.cmajor\"\n"
                         "}\n";

        if (! patchFile.replaceWithText (patchText))
        {
            lastError = "could not write Cmajor patch";
            return;
        }

        auto duration = lane.params.getValue ("duration", "8").getFloatValue();
        if (duration <= 0.0f)
            duration = 8.0f;
        auto frameCount = (int) std::round (duration * sampleRate);

        int exitCode = -1;
        auto output = runProcess ({ cmajExecutable.getFullPathName(),
                                    "render",
                                    "--length=" + juce::String (frameCount),
                                    "--rate=" + juce::String ((int) std::round (sampleRate)),
                                    "--channels=" + juce::String (numChannels),
                                    "--blockSize=512",
                                    "--output=" + wavFile.getFullPathName(),
                                    patchFile.getFullPathName() },
                                  exitCode);

        if (exitCode != 0 || ! wavFile.existsAsFile())
        {
            lastError = output.trim().isEmpty() ? "cmaj render failed" : output.trim();
            return;
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wavFile));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            lastError = "could not read rendered wav";
            return;
        }

        rendered.setSize (juce::jmax (1, (int) reader->numChannels), (int) reader->lengthInSamples);
        reader->read (&rendered, 0, rendered.getNumSamples(), 0, true, true);
        storeRenderedAudioInCache (cacheKey, rendered);
    }

    LaneDefinition lane;
    juce::File cmajExecutable;
    juce::AudioBuffer<float> rendered;
    juce::String lastError;
    double sampleRate = 44100.0;
    int numChannels = 2;
    int playhead = 0;
};

class CmajorCliHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "cmajor"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
        auto cmaj = findExecutableOnPath ("cmaj");
        if (! cmaj.existsAsFile())
        {
            error = "Cmajor CLI was not found. Install Cmajor, then this adapter can compile lane code.";
            return {};
        }

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("MarkovStudio")
                              .getNonexistentChildFile ("cmajor-lane", {});
        workingDir.createDirectory();

        auto sourceFile = workingDir.getChildFile ("MarkovLane.cmajor");
        auto patchFile = workingDir.getChildFile ("MarkovLane.cmajorpatch");
        auto generatedFile = workingDir.getChildFile ("MarkovLaneGenerated.h");
        auto wrapperFile = workingDir.getChildFile ("wrapper.cpp");
        auto dylibFile = workingDir.getChildFile ("lane.dylib");

        if (! sourceFile.replaceWithText (expandCodeParameters (lane.code, lane.params)))
        {
            error = "Could not write Cmajor lane source.";
            return {};
        }

        auto patchText = "{\n"
                         "  \"CmajorVersion\": 1,\n"
                         "  \"ID\": \"com.markov.lane\",\n"
                         "  \"version\": \"1.0\",\n"
                         "  \"name\": \"Markov Lane\",\n"
                         "  \"category\": \"generator\",\n"
                         "  \"manufacturer\": \"Markov\",\n"
                         "  \"isInstrument\": true,\n"
                         "  \"source\": \"MarkovLane.cmajor\"\n"
                         "}\n";

        if (! patchFile.replaceWithText (patchText))
        {
            error = "Could not write Cmajor patch.";
            return {};
        }

        int exitCode = -1;
        auto output = runProcess ({ cmaj.getFullPathName(),
                                    "generate",
                                    "--target=cpp",
                                    "--maxFramesPerBlock=512",
                                    "--output=" + generatedFile.getFullPathName(),
                                    patchFile.getFullPathName() },
                                  exitCode);

        if (exitCode != 0 || ! generatedFile.existsAsFile())
        {
            error = "Cmajor native compile failed:\n" + output;
            return {};
        }

        auto wrapperSource = juce::String (R"CPP(
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include ")CPP") + cIncludePath (generatedFile) + R"CPP("

struct MarkovCmajorInstance
{
    MarkovLane dsp;
    uint32_t outputHandle = MarkovLane::getEndpointHandleForName ("out");
    std::vector<float> output;

    MarkovCmajorInstance() : output (MarkovLane::maxFramesPerBlock) {}
};

extern "C" MarkovCmajorInstance* markov_cmajor_create()
{
    return new MarkovCmajorInstance();
}

extern "C" void markov_cmajor_destroy (MarkovCmajorInstance* instance)
{
    delete instance;
}

extern "C" int markov_cmajor_max_block (MarkovCmajorInstance*)
{
    return (int) MarkovLane::maxFramesPerBlock;
}

extern "C" int markov_cmajor_outputs (MarkovCmajorInstance*)
{
    return (int) std::max<uint32_t> (1, MarkovLane::numAudioOutputChannels);
}

extern "C" void markov_cmajor_init (MarkovCmajorInstance* instance, int sampleRate)
{
    if (instance != nullptr)
        instance->dsp.initialise (1, sampleRate);
}

extern "C" void markov_cmajor_reset (MarkovCmajorInstance* instance)
{
    if (instance != nullptr)
        instance->dsp.reset();
}

extern "C" void markov_cmajor_render (MarkovCmajorInstance* instance, int frames, float** outputs)
{
    if (instance == nullptr || outputs == nullptr || frames <= 0)
        return;

    frames = std::min<int> (frames, (int) MarkovLane::maxFramesPerBlock);
    instance->dsp.advance (frames);
    std::fill (instance->output.begin(), instance->output.begin() + frames, 0.0f);
    instance->dsp.copyOutputFrames (instance->outputHandle, instance->output.data(), (uint32_t) frames);

    auto outputChannels = std::max<uint32_t> (1, MarkovLane::numAudioOutputChannels);
    for (uint32_t channel = 0; channel < outputChannels; ++channel)
    {
        if (outputs[channel] != nullptr)
            std::memcpy (outputs[channel], instance->output.data(), sizeof (float) * (size_t) frames);
    }
}

extern "C" int markov_cmajor_param_count (MarkovCmajorInstance*)
{
    int count = 0;
    for (const auto& endpoint : MarkovLane::inputEndpoints)
        if (endpoint.endpointType == MarkovLane::EndpointType::value)
            ++count;

    return count;
}

extern "C" const char* markov_cmajor_param_label (MarkovCmajorInstance*, int index)
{
    int count = 0;
    for (const auto& endpoint : MarkovLane::inputEndpoints)
        if (endpoint.endpointType == MarkovLane::EndpointType::value)
            if (count++ == index)
                return endpoint.name;

    return "";
}

extern "C" bool markov_cmajor_set_param (MarkovCmajorInstance* instance, const char* name, float value, int frames)
{
    if (instance == nullptr || name == nullptr)
        return false;

    auto handle = MarkovLane::getEndpointHandleForName (name);
    if (handle == 0)
        return false;

    instance->dsp.setValue (handle, &value, std::max (1, frames));
    return true;
}
)CPP";

        if (! wrapperFile.replaceWithText (wrapperSource))
        {
            error = "Could not write Cmajor native wrapper.";
            return {};
        }

        output = runProcess ({ "clang++",
                               "-std=c++17",
                               "-O2",
                               "-dynamiclib",
                               wrapperFile.getFullPathName(),
                               "-o", dylibFile.getFullPathName() },
                             exitCode);

        if (exitCode != 0 || ! dylibFile.existsAsFile())
        {
            error = "Cmajor native library build failed:\n" + output;
            return {};
        }

        auto library = std::make_unique<juce::DynamicLibrary>();
        if (! library->open (dylibFile.getFullPathName()))
        {
            error = "Could not load Cmajor native lane library.";
            return {};
        }

        using CreateFn = void* (*)();
        using DestroyFn = void (*) (void*);
        using InitFn = void (*) (void*, int);
        using ResetFn = void (*) (void*);
        using RenderFn = void (*) (void*, int, float**);
        using CountFn = int (*) (void*);
        using LabelFn = const char* (*) (void*, int);
        using SetParamFn = bool (*) (void*, const char*, float, int);

        auto create = reinterpret_cast<CreateFn> (library->getFunction ("markov_cmajor_create"));
        auto destroy = reinterpret_cast<DestroyFn> (library->getFunction ("markov_cmajor_destroy"));
        auto init = reinterpret_cast<InitFn> (library->getFunction ("markov_cmajor_init"));
        auto reset = reinterpret_cast<ResetFn> (library->getFunction ("markov_cmajor_reset"));
        auto render = reinterpret_cast<RenderFn> (library->getFunction ("markov_cmajor_render"));
        auto maxBlock = reinterpret_cast<CountFn> (library->getFunction ("markov_cmajor_max_block"));
        auto outputs = reinterpret_cast<CountFn> (library->getFunction ("markov_cmajor_outputs"));
        auto paramCount = reinterpret_cast<CountFn> (library->getFunction ("markov_cmajor_param_count"));
        auto paramLabel = reinterpret_cast<LabelFn> (library->getFunction ("markov_cmajor_param_label"));
        auto setParam = reinterpret_cast<SetParamFn> (library->getFunction ("markov_cmajor_set_param"));

        if (create == nullptr || destroy == nullptr || init == nullptr || reset == nullptr || render == nullptr
            || maxBlock == nullptr || outputs == nullptr || paramCount == nullptr || paramLabel == nullptr || setParam == nullptr)
        {
            error = "Cmajor native lane library is missing required entry points.";
            return {};
        }

        class CmajorDylibProgram final : public AudioProgram
        {
        public:
            CmajorDylibProgram (LaneDefinition laneToUse,
                                std::unique_ptr<juce::DynamicLibrary> libraryToUse,
                                CreateFn createToUse,
                                DestroyFn destroyToUse,
                                InitFn initToUse,
                                ResetFn resetToUse,
                                RenderFn renderToUse,
                                CountFn maxBlockToUse,
                                CountFn outputsToUse,
                                CountFn paramCountToUse,
                                LabelFn paramLabelToUse,
                                SetParamFn setParamToUse)
                : lane (std::move (laneToUse)),
                  library (std::move (libraryToUse)),
                  create (createToUse),
                  destroy (destroyToUse),
                  init (initToUse),
                  resetInstance (resetToUse),
                  renderInstance (renderToUse),
                  getMaxBlock (maxBlockToUse),
                  getOutputs (outputsToUse),
                  getParamCount (paramCountToUse),
                  getParamLabel (paramLabelToUse),
                  setParam (setParamToUse)
            {
                instance = create();
                refreshParameters();
            }

            ~CmajorDylibProgram() override
            {
                if (instance != nullptr)
                    destroy (instance);
            }

            void prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse) override
            {
                sampleRate = sampleRateToUse;
                hostChannels = juce::jmax (1, numChannelsToUse);
                maxBlockSize = juce::jmax (1, juce::jmin (maxBlockSizeToUse, getMaxBlock (instance)));
                numOutputs = juce::jmax (1, getOutputs (instance));
                outputScratch.setSize (numOutputs, maxBlockSize, false, false, true);
                outputPointers.resize ((size_t) numOutputs);

                init (instance, (int) std::round (sampleRate));
                applyLaneParameters (1);
            }

            void reset() override
            {
                if (instance != nullptr)
                {
                    resetInstance (instance);
                    applyLaneParameters (1);
                }
            }

            bool setParameter (const juce::String& parameterId, float value) override
            {
                auto id = parameterId.trim().toLowerCase();

                if (id == "gain")
                {
                    lane.gain = juce::jlimit (0.0f, 1.5f, value);
                    return true;
                }

                if (instance == nullptr)
                    return false;

                for (auto& parameter : parameters)
                {
                    if (parameter.id.equalsIgnoreCase (id))
                    {
                        parameter.currentValue = value;
                        lane.params.set (parameter.id, juce::String (value, 6));
                        return setParam (instance, parameter.id.toRawUTF8(), value, 64);
                    }
                }

                return false;
            }

            juce::Array<AudioParameterInfo> getParameters() const override
            {
                juce::Array<AudioParameterInfo> result = parameters;
                addParameterInfo (result, "gain", lane.gain, 0.0f, 1.5f, 0.01f);
                return result;
            }

            void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
            {
                if (lane.muted || instance == nullptr || numOutputs <= 0)
                    return;

                auto remaining = numSamples;
                auto offset = 0;

                while (remaining > 0)
                {
                    auto block = juce::jmin (remaining, maxBlockSize);
                    outputScratch.clear();

                    for (int output = 0; output < numOutputs; ++output)
                        outputPointers[(size_t) output] = outputScratch.getWritePointer (output);

                    renderInstance (instance, block, outputPointers.data());

                    for (int channel = 0; channel < hostChannels && channel < buffer.getNumChannels(); ++channel)
                    {
                        auto sourceOutput = channel % numOutputs;
                        buffer.addFrom (channel,
                                        startSample + offset,
                                        outputScratch,
                                        sourceOutput,
                                        0,
                                        block,
                                        lane.gain);
                    }

                    offset += block;
                    remaining -= block;
                }
            }

            juce::String describe() const override
            {
                return "Cmajor native DSP " + juce::String (numOutputs) + " out "
                     + juce::String (parameters.size()) + " params gain " + juce::String (lane.gain, 2);
            }

        private:
            void refreshParameters()
            {
                parameters.clear();

                if (instance == nullptr)
                    return;

                auto count = juce::jmax (0, getParamCount (instance));
                for (int i = 0; i < count; ++i)
                {
                    AudioParameterInfo info;
                    info.id = juce::String::fromUTF8 (getParamLabel (instance, i)).trim().toLowerCase();
                    info.label = info.id;
                    info.currentValue = lane.params.getValue (info.id, info.id == "freq" ? "440" : "0.2").getFloatValue();
                    info.defaultValue = info.currentValue;
                    info.minimumValue = info.id == "freq" ? 20.0f : 0.0f;
                    info.maximumValue = info.id == "freq" ? 5000.0f : 1.0f;
                    info.step = info.id == "freq" ? 1.0f : 0.01f;
                    parameters.add (info);
                }
            }

            void applyLaneParameters (int frames)
            {
                for (auto& parameter : parameters)
                    setParam (instance, parameter.id.toRawUTF8(), parameter.currentValue, frames);
            }

            LaneDefinition lane;
            std::unique_ptr<juce::DynamicLibrary> library;
            CreateFn create = nullptr;
            DestroyFn destroy = nullptr;
            InitFn init = nullptr;
            ResetFn resetInstance = nullptr;
            RenderFn renderInstance = nullptr;
            CountFn getMaxBlock = nullptr;
            CountFn getOutputs = nullptr;
            CountFn getParamCount = nullptr;
            LabelFn getParamLabel = nullptr;
            SetParamFn setParam = nullptr;
            void* instance = nullptr;
            double sampleRate = 44100.0;
            int maxBlockSize = 512;
            int hostChannels = 2;
            int numOutputs = 1;
            juce::AudioBuffer<float> outputScratch;
            std::vector<float*> outputPointers;
            juce::Array<AudioParameterInfo> parameters;
        };

        error.clear();
        return std::make_unique<CmajorDylibProgram> (lane,
                                                     std::move (library),
                                                     create,
                                                     destroy,
                                                     init,
                                                     reset,
                                                     render,
                                                     maxBlock,
                                                     outputs,
                                                     paramCount,
                                                     paramLabel,
                                                     setParam);
    }
};

class ChuckRenderedProgram final : public AudioProgram
{
public:
    ChuckRenderedProgram (LaneDefinition laneToUse, juce::File chuckExecutableToUse)
        : lane (std::move (laneToUse)), chuckExecutable (std::move (chuckExecutableToUse))
    {
    }

    void prepare (double sampleRateToUse, int, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        numChannels = juce::jmax (1, numChannelsToUse);
        playhead = 0;
        renderLane();
    }

    void reset() override
    {
        playhead = 0;
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
            return false;

        lane.params.set (id, juce::String (value, 6));
        renderLane();
        return true;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (lane.muted || rendered.getNumSamples() == 0)
            return;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto sourceChannel = channel % rendered.getNumChannels();
                buffer.addSample (channel, startSample + sample, rendered.getSample (sourceChannel, playhead) * lane.gain);
            }

            playhead = (playhead + 1) % rendered.getNumSamples();
        }
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return "ChucK render error: " + lastError;

        return "ChucK rendered loop " + juce::String (rendered.getNumSamples()) + " samples"
             + " gain " + juce::String (lane.gain, 2);
    }

private:
    void renderLane()
    {
        lastError.clear();
        rendered.setSize (numChannels, 0);

        auto cacheKey = makeRenderedAudioCacheKey ("chuck", lane, chuckExecutable, sampleRate, numChannels);
        if (loadRenderedAudioFromCache (cacheKey, rendered))
            return;

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getNonexistentChildFile ("markov-chuck-lane", {});

        if (! workingDir.createDirectory())
        {
            lastError = "could not create temp folder";
            return;
        }

        auto sourceFile = workingDir.getChildFile ("lane.ck");
        auto wavFile = workingDir.getChildFile ("lane.wav");
        auto code = expandCodeParameters (lane.code, lane.params)
                       .replace ("{{output}}", wavFile.getFullPathName().replace ("\\", "\\\\").replace ("\"", "\\\""));

        if (! sourceFile.replaceWithText (code))
        {
            lastError = "could not write ChucK source";
            return;
        }

        int exitCode = -1;
        auto output = runProcess ({ chuckExecutable.getFullPathName(),
                                    "--silent",
                                    "--srate:" + juce::String ((int) std::round (sampleRate)),
                                    sourceFile.getFullPathName() },
                                  exitCode);

        if (exitCode != 0 || ! wavFile.existsAsFile())
        {
            lastError = output.trim().isEmpty() ? "chuck failed" : output.trim();
            return;
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wavFile));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            lastError = "could not read rendered wav";
            return;
        }

        rendered.setSize (juce::jmax (1, (int) reader->numChannels), (int) reader->lengthInSamples);
        reader->read (&rendered, 0, rendered.getNumSamples(), 0, true, true);
        storeRenderedAudioInCache (cacheKey, rendered);
    }

    LaneDefinition lane;
    juce::File chuckExecutable;
    juce::AudioBuffer<float> rendered;
    juce::String lastError;
    double sampleRate = 44100.0;
    int numChannels = 2;
    int playhead = 0;
};

class ChuckCliHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "chuck"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
#if MARKOV_HAS_CHUCK_CORE
        class ChuckLiveProgram final : public AudioProgram
        {
        public:
            explicit ChuckLiveProgram (LaneDefinition laneToUse)
                : lane (std::move (laneToUse))
            {
            }

            ~ChuckLiveProgram() override
            {
                chuck.reset();
            }

            void prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse) override
            {
                sampleRate = sampleRateToUse;
                maxBlockSize = juce::jmax (1, maxBlockSizeToUse);
                numChannels = juce::jmax (1, numChannelsToUse);
                inputInterleaved.resize ((size_t) maxBlockSize * (size_t) numChannels);
                outputInterleaved.resize ((size_t) maxBlockSize * (size_t) numChannels);
                startVm();
            }

            void reset() override
            {
                if (! started)
                {
                    startVm();
                    return;
                }

                applyLaneGlobals();
            }

            bool setParameter (const juce::String& parameterId, float value) override
            {
                auto id = parameterId.trim().toLowerCase();

                if (id == "gain")
                {
                    lane.gain = juce::jlimit (0.0f, 1.5f, value);
                    return true;
                }

                if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
                    return false;

                lane.params.set (id, juce::String (value, 6));
                setGlobal (id, value);
                return true;
            }

            juce::Array<AudioParameterInfo> getParameters() const override
            {
                return makeRenderedLaneParameters (lane);
            }

            void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
            {
                if (lane.muted || chuck == nullptr || ! started)
                    return;

                auto remaining = numSamples;
                auto offset = 0;

                while (remaining > 0)
                {
                    auto block = juce::jmin (remaining, maxBlockSize);
                    std::fill (inputInterleaved.begin(), inputInterleaved.begin() + (size_t) block * (size_t) numChannels, (SAMPLE) 0);
                    std::fill (outputInterleaved.begin(), outputInterleaved.begin() + (size_t) block * (size_t) numChannels, (SAMPLE) 0);

                    chuck->run (inputInterleaved.data(), outputInterleaved.data(), block);

                    for (int sample = 0; sample < block; ++sample)
                    {
                        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                        {
                            auto sourceChannel = channel % numChannels;
                            auto value = (float) outputInterleaved[(size_t) sample * (size_t) numChannels + (size_t) sourceChannel] * lane.gain;
                            buffer.addSample (channel, startSample + offset + sample, value);
                        }
                    }

                    offset += block;
                    remaining -= block;
                }
            }

            juce::String describe() const override
            {
                if (lastError.isNotEmpty())
                    return "ChucK live error: " + lastError;

                return "ChucK live VM gain " + juce::String (lane.gain, 2);
            }

        private:
            void startVm()
            {
                lastError.clear();
                started = false;

                if (sampleRate <= 0.0)
                    return;

                chuck = std::make_unique<ChucK>();
                chuck->setParam (CHUCK_PARAM_SAMPLE_RATE, (t_CKINT) std::round (sampleRate));
                chuck->setParam (CHUCK_PARAM_INPUT_CHANNELS, (t_CKINT) 0);
                chuck->setParam (CHUCK_PARAM_OUTPUT_CHANNELS, (t_CKINT) numChannels);
                chuck->setParam (CHUCK_PARAM_VM_HALT, (t_CKINT) FALSE);
                chuck->setParam (CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, (t_CKINT) TRUE);

                if (! chuck->init() || ! chuck->start())
                {
                    lastError = "could not start ChucK VM";
                    chuck.reset();
                    return;
                }

                auto code = expandCodeParameters (lane.code, lane.params);
                if (! chuck->compileCode (code.toStdString(), "", 1, FALSE, nullptr, "markov-lane.ck"))
                {
                    lastError = "ChucK compile failed";
                    chuck.reset();
                    return;
                }

                started = true;
                applyLaneGlobals();
            }

            void applyLaneGlobals()
            {
                setGlobal ("freq", lane.params.getValue ("freq", "440").getFloatValue());
                setGlobal ("amp", lane.params.getValue ("amp", "0.2").getFloatValue());
                setGlobal ("cutoff", lane.params.getValue ("cutoff", "1400").getFloatValue());
                setGlobal ("duration", lane.params.getValue ("duration", "8").getFloatValue());
            }

            void setGlobal (const juce::String& id, float value)
            {
                if (chuck != nullptr && chuck->globals() != nullptr)
                    chuck->globals()->setGlobalFloat (("markov_" + id).toRawUTF8(), (t_CKFLOAT) value);
            }

            LaneDefinition lane;
            std::unique_ptr<ChucK> chuck;
            juce::String lastError;
            double sampleRate = 44100.0;
            int maxBlockSize = 512;
            int numChannels = 2;
            bool started = false;
            std::vector<SAMPLE> inputInterleaved;
            std::vector<SAMPLE> outputInterleaved;
        };

        error.clear();
        return std::make_unique<ChuckLiveProgram> (lane);
#else
        auto chuck = findExecutableOnPath ("chuck");
        if (! chuck.existsAsFile())
        {
            error = "ChucK CLI was not found. Install ChucK, then this adapter can render lane code.";
            return {};
        }

        error.clear();
        return std::make_unique<ChuckRenderedProgram> (lane, chuck);
#endif
    }
};

class PlaceholderHost final : public AudioLanguageHost
{
public:
    explicit PlaceholderHost (juce::String idToUse) : id (std::move (idToUse)) {}

    juce::String languageId() const override { return id; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition&, juce::String& error) override
    {
        error = id + " support is reserved for a dedicated adapter. This lane is silent for now.";
        return {};
    }

private:
    juce::String id;
};

class SuperColliderRenderedProgram final : public AudioProgram
{
public:
    SuperColliderRenderedProgram (LaneDefinition laneToUse, juce::File sclangExecutableToUse)
        : lane (std::move (laneToUse)), sclangExecutable (std::move (sclangExecutableToUse))
    {
    }

    void prepare (double sampleRateToUse, int, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        numChannels = juce::jmax (1, numChannelsToUse);
        playhead = 0;
        renderLane();
    }

    void reset() override
    {
        playhead = 0;
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
            return false;

        lane.params.set (id, juce::String (value, 6));
        renderLane();
        return lastError.isEmpty();
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (lane.muted || rendered.getNumSamples() == 0)
            return;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto sourceChannel = channel % rendered.getNumChannels();
                buffer.addSample (channel, startSample + sample, rendered.getSample (sourceChannel, playhead) * lane.gain);
            }

            playhead = (playhead + 1) % rendered.getNumSamples();
        }
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return "SuperCollider render error: " + lastError;

        return "SuperCollider rendered loop " + juce::String (rendered.getNumSamples()) + " samples"
             + " gain " + juce::String (lane.gain, 2);
    }

private:
    void renderLane()
    {
        lastError.clear();
        rendered.setSize (numChannels, 0);

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getNonexistentChildFile ("markov-supercollider-lane", {});

        if (! workingDir.createDirectory())
        {
            lastError = "could not create temp folder";
            return;
        }

        auto sourceFile = workingDir.getChildFile ("lane.scd");
        auto wavFile = workingDir.getChildFile ("lane.wav");
        auto oscFile = workingDir.getChildFile ("lane.osc");
        auto code = expandCodeParameters (lane.code, lane.params);
        auto duration = juce::jlimit (0.25f, 64.0f, lane.params.getValue ("duration", "8").getFloatValue());
        auto defaultFreq = lane.params.getValue ("freq", "440").getFloatValue();
        auto defaultAmp = lane.params.getValue ("amp", "0.15").getFloatValue();
        auto defaultCutoff = lane.params.getValue ("cutoff", "3000").getFloatValue();
        auto cacheKey = makeRenderedAudioCacheKey ("supercollider", lane, sclangExecutable, sampleRate, numChannels);

        if (loadRenderedAudioFromCache (cacheKey, rendered))
            return;

        auto scriptText = juce::String (R"SC(
(
var out = )SC") + scStringLiteral (wavFile.getFullPathName()) + R"SC(;
var osc = )SC" + scStringLiteral (oscFile.getFullPathName()) + R"SC(;
var duration = )SC" + juce::String (duration, 6) + R"SC(;
var options = ServerOptions.new.numOutputBusChannels_(2).sampleRate_()SC" + juce::String ((int) std::round (sampleRate)) + R"SC();
var def = SynthDef(\markovLane, {
    var freq = \freq.kr()SC" + juce::String (defaultFreq, 6) + R"SC();
    var amp = \amp.kr()SC" + juce::String (defaultAmp, 6) + R"SC();
    var cutoff = \cutoff.kr()SC" + juce::String (defaultCutoff, 6) + R"SC();
    var sig = )SC" + code + R"SC(;
    sig = LeakDC.ar(sig);
    sig = sig.asArray;
    sig = if(sig.size < 2, { [sig[0], sig[0]] }, { [sig[0], sig[1]] });
    Out.ar(0, sig);
});
Score.recordNRT([
    [0.0, [\d_recv, def.asBytes]],
    [0.0, [\s_new, \markovLane, 1000, 0, 0]],
    [duration, [\n_free, 1000]]
], osc, out, sampleRate: )SC" + juce::String ((int) std::round (sampleRate)) + R"SC(, headerFormat: "WAV", sampleFormat: "float", options: options, duration: duration, action: { 0.exit });
)
)SC";

        if (! sourceFile.replaceWithText (scriptText))
        {
            lastError = "could not write SuperCollider source";
            return;
        }

        int exitCode = -1;
        auto output = runProcess ({ sclangExecutable.getFullPathName(), sourceFile.getFullPathName() }, exitCode);

        if (exitCode != 0 || ! wavFile.existsAsFile())
        {
            lastError = output.trim().isEmpty() ? "sclang failed" : output.trim();
            return;
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wavFile));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            lastError = "could not read rendered wav";
            return;
        }

        rendered.setSize (juce::jmax (1, (int) reader->numChannels), (int) reader->lengthInSamples);
        reader->read (&rendered, 0, rendered.getNumSamples(), 0, true, true);
        storeRenderedAudioInCache (cacheKey, rendered);
    }

    LaneDefinition lane;
    juce::File sclangExecutable;
    juce::AudioBuffer<float> rendered;
    juce::String lastError;
    double sampleRate = 44100.0;
    int numChannels = 2;
    int playhead = 0;
};

class SuperColliderAuProgram final : public AudioProgram
{
public:
    SuperColliderAuProgram (LaneDefinition laneToUse, juce::File sclangExecutableToUse)
        : lane (std::move (laneToUse)),
          sclangExecutable (std::move (sclangExecutableToUse))
    {
    }

    ~SuperColliderAuProgram() override
    {
        shutdownPlugin();
    }

    void prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        maxBlockSize = juce::jmax (1, maxBlockSizeToUse);
        numChannels = juce::jmax (1, numChannelsToUse);
        midi.clear();
        startPlugin();
    }

    void reset() override
    {
        if (! ready)
        {
            startSynth();
            return;
        }

        sendNodeSet ("freq", lane.params.getValue ("freq", "440").getFloatValue());
        sendNodeSet ("amp", lane.params.getValue ("amp", "0.15").getFloatValue());
        sendNodeSet ("cutoff", lane.params.getValue ("cutoff", "3000").getFloatValue());
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        if (id != "freq" && id != "amp" && id != "cutoff" && id != "duration")
            return false;

        lane.params.set (id, juce::String (value, 6));
        sendNodeSet (id, value);
        return true;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (lane.muted || plugin == nullptr || ! ready)
            return;

        if (pluginBuffer.getNumChannels() != juce::jmax (2, numChannels)
            || pluginBuffer.getNumSamples() < numSamples)
            pluginBuffer.setSize (juce::jmax (2, numChannels), numSamples, false, false, true);

        pluginBuffer.clear (0, numSamples);
        plugin->processBlock (pluginBuffer, midi);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto sourceChannel = channel % pluginBuffer.getNumChannels();
            buffer.addFrom (channel, startSample, pluginBuffer, sourceChannel, 0, numSamples, lane.gain);
        }
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return "SuperCollider live AU error: " + lastError;

        return ready ? "SuperCollider live AU gain " + juce::String (lane.gain, 2)
                     : "SuperCollider live AU starting";
    }

    void releaseResources() override
    {
        shutdownPlugin();
    }

private:
    void startPlugin()
    {
        lastError.clear();
        ready = false;

        if (plugin != nullptr)
            shutdownPlugin();

        portSlot = nextSuperColliderAuSlot.fetch_add (1);
        oscPort = 9989 + portSlot;
        nodeId = 1000 + portSlot;
        synthDefName = "markovLane" + juce::String (nodeId);

        auto component = juce::File ("~/Library/Audio/Plug-Ins/Components/SuperColliderAU.component");
        component = component.getFullPathName();
        if (! component.exists())
        {
            lastError = "SuperColliderAU.component was not found.";
            return;
        }

        juce::AudioUnitPluginFormat format;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format.findAllTypesForFile (descriptions, component.getFullPathName());

        if (descriptions.isEmpty())
        {
            lastError = "SuperColliderAU could not be scanned.";
            return;
        }

        juce::String creationError;
        auto createdPlugin = format.createInstanceFromDescription (*descriptions[0],
                                                                   sampleRate,
                                                                   maxBlockSize,
                                                                   creationError);

        if (createdPlugin == nullptr)
        {
            lastError = creationError.isNotEmpty() ? creationError : "Could not start SuperColliderAU.";
            return;
        }

        plugin = std::move (createdPlugin);
        plugin->setPlayConfigDetails (juce::jmax (2, numChannels), juce::jmax (2, numChannels), sampleRate, maxBlockSize);
        plugin->prepareToPlay (sampleRate, maxBlockSize);

        osc.disconnect();
        oscConnected = osc.connect ("127.0.0.1", oscPort);
        startSynth();
    }

    void shutdownPlugin()
    {
        ready = false;

        try
        {
            stopNode();
            osc.disconnect();
            oscConnected = false;

            if (plugin != nullptr)
            {
                plugin->suspendProcessing (true);
                plugin->releaseResources();
                plugin.reset();
            }
        }
        catch (...)
        {
            plugin.reset();
            oscConnected = false;
        }
    }

    void startSynth()
    {
        if (plugin == nullptr || ! oscConnected)
            return;

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getNonexistentChildFile ("markov-supercollider-live-defs", {});
        workingDir.createDirectory();

        auto helper = makeSynthDefWriterScript (workingDir);
        auto helperFile = workingDir.getChildFile ("write-def.scd");

        if (! helperFile.replaceWithText (helper))
        {
            lastError = "Could not write SuperCollider live helper.";
            return;
        }

        int exitCode = -1;
        auto output = runProcess ({ sclangExecutable.getFullPathName(), helperFile.getFullPathName() }, exitCode);
        if (exitCode != 0)
        {
            lastError = output.trim().isEmpty() ? "Could not start SuperCollider live synth." : output.trim();
            return;
        }

        auto synthDefFile = workingDir.getChildFile (synthDefName + ".scsyndef");
        if (! synthDefFile.existsAsFile())
        {
            lastError = "SuperCollider did not create the live SynthDef.";
            return;
        }

        pumpPlugin (4);
        osc.send ("/d_load", juce::String (synthDefFile.getFullPathName()));
        pumpPlugin (24);
        osc.send ("/s_new",
                  synthDefName,
                  nodeId,
                  0,
                  0,
                  juce::String ("freq"),
                  lane.params.getValue ("freq", "440").getFloatValue(),
                  juce::String ("amp"),
                  lane.params.getValue ("amp", "0.15").getFloatValue(),
                  juce::String ("cutoff"),
                  lane.params.getValue ("cutoff", "3000").getFloatValue());
        pumpPlugin (16);
        ready = true;
    }

    juce::String makeSynthDefWriterScript (const juce::File& outputDirectory) const
    {
        auto code = expandCodeParameters (lane.code, lane.params);
        auto defaultFreq = lane.params.getValue ("freq", "440").getFloatValue();
        auto defaultAmp = lane.params.getValue ("amp", "0.15").getFloatValue();
        auto defaultCutoff = lane.params.getValue ("cutoff", "3000").getFloatValue();

        juce::String script (R"SC(
(
)SC");
        script += "    SynthDef(\\" + synthDefName + ", {\n";
        script += "        var freq = \\freq.kr(" + juce::String (defaultFreq, 6) + ");\n";
        script += "        var amp = \\amp.kr(" + juce::String (defaultAmp, 6) + ");\n";
        script += "        var cutoff = \\cutoff.kr(" + juce::String (defaultCutoff, 6) + ");\n";
        script += "        var sig = " + code + ";\n";
        script += R"SC(        sig = LeakDC.ar(sig);
        sig = sig.asArray;
        sig = if(sig.size < 2, { [sig[0], sig[0]] }, { [sig[0], sig[1]] });
        Out.ar(0, sig);
    }).writeDefFile()SC";
        script += scStringLiteral (outputDirectory.getFullPathName());
        script += R"SC();
    0.exit;
)
)SC";

        return script;
    }

    void pumpPlugin (int blocks)
    {
        if (plugin == nullptr)
            return;

        if (pluginBuffer.getNumChannels() != juce::jmax (2, numChannels)
            || pluginBuffer.getNumSamples() < maxBlockSize)
            pluginBuffer.setSize (juce::jmax (2, numChannels), maxBlockSize, false, false, true);

        for (int i = 0; i < blocks; ++i)
        {
            pluginBuffer.clear();
            plugin->processBlock (pluginBuffer, midi);
            std::this_thread::sleep_for (std::chrono::milliseconds (8));
        }
    }

    void stopNode()
    {
        if (oscConnected)
            osc.send ("/n_free", nodeId);
    }

    void sendNodeSet (const juce::String& id, float value)
    {
        if (oscConnected && ready)
            osc.send ("/n_set", nodeId, juce::String (id), value);
    }

    LaneDefinition lane;
    juce::File sclangExecutable;
    juce::String lastError;
    double sampleRate = 44100.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    int portSlot = 0;
    int oscPort = 9989;
    int nodeId = 1000;
    juce::String synthDefName;
    bool ready = false;
    bool oscConnected = false;
    juce::OSCSender osc;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> pluginBuffer;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
};

class SuperColliderProgram final : public AudioProgram
{
public:
    SuperColliderProgram (LaneDefinition laneToUse, juce::File sclangExecutableToUse)
        : lane (std::move (laneToUse)),
          sclangExecutable (std::move (sclangExecutableToUse))
    {
    }

    void prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        maxBlockSize = juce::jmax (1, maxBlockSizeToUse);
        numChannels = juce::jmax (1, numChannelsToUse);
        lastError.clear();
        usingFallback = false;

        liveProgram = std::make_unique<SuperColliderAuProgram> (lane, sclangExecutable);
        liveProgram->prepare (sampleRate, maxBlockSize, numChannels);
        liveProgram->reset();

        if (programProducesAudio (*liveProgram))
        {
            liveProgram->reset();
            renderedProgram.reset();
            return;
        }

        liveDescription = liveProgram->describe();
        liveProgram->releaseResources();
        liveProgram.reset();

        renderedProgram = std::make_unique<SuperColliderRenderedProgram> (lane, sclangExecutable);
        renderedProgram->prepare (sampleRate, maxBlockSize, numChannels);
        renderedProgram->reset();
        usingFallback = true;

        if (! programProducesAudio (*renderedProgram))
            lastError = "SuperCollider produced silence. " + renderedProgram->describe();

        renderedProgram->reset();
    }

    void reset() override
    {
        if (auto* active = getActiveProgram())
            active->reset();
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        auto id = parameterId.trim().toLowerCase();

        if (id == "gain")
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
        else if (id == "freq" || id == "amp" || id == "cutoff" || id == "duration")
            lane.params.set (id, juce::String (value, 6));

        if (auto* active = getActiveProgram())
            return active->setParameter (parameterId, value);

        return id == "gain" || id == "freq" || id == "amp" || id == "cutoff" || id == "duration";
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        return makeRenderedLaneParameters (lane);
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (auto* active = getActiveProgram())
            active->render (buffer, startSample, numSamples);
    }

    void releaseResources() override
    {
        if (liveProgram != nullptr)
            liveProgram->releaseResources();

        if (renderedProgram != nullptr)
            renderedProgram->releaseResources();
    }

    juce::String describe() const override
    {
        if (lastError.isNotEmpty())
            return lastError;

        if (usingFallback && renderedProgram != nullptr)
            return "SuperCollider rendered fallback; live AU was silent. " + renderedProgram->describe();

        if (liveProgram != nullptr)
            return liveProgram->describe();

        return liveDescription.isNotEmpty() ? liveDescription : "SuperCollider starting";
    }

private:
    AudioProgram* getActiveProgram() const
    {
        if (usingFallback)
            return renderedProgram.get();

        return liveProgram.get();
    }

    bool programProducesAudio (AudioProgram& program)
    {
        auto channels = juce::jmax (2, numChannels);
        auto probeSamples = juce::jlimit (maxBlockSize, (int) std::round (sampleRate * 1.0), maxBlockSize * 96);
        juce::AudioBuffer<float> probe (channels, probeSamples);
        probe.clear();

        for (int offset = 0; offset < probeSamples; offset += maxBlockSize)
        {
            auto block = juce::jmin (maxBlockSize, probeSamples - offset);
            program.render (probe, offset, block);
        }

        return juce::jmax (probe.getMagnitude (0, probeSamples),
                           channels > 1 ? probe.getMagnitude (1, probeSamples) : 0.0f) > 0.0001f;
    }

    LaneDefinition lane;
    juce::File sclangExecutable;
    juce::String lastError;
    juce::String liveDescription;
    double sampleRate = 44100.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    bool usingFallback = false;
    std::unique_ptr<AudioProgram> liveProgram;
    std::unique_ptr<AudioProgram> renderedProgram;
};

class SuperColliderHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "supercollider"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
        auto sclang = findExecutableOnPath ("sclang");
        if (! sclang.existsAsFile())
        {
            error = "SuperCollider was not found. Install SuperCollider or put sclang on PATH.";
            return {};
        }

        error.clear();
        if (! shouldUseSuperColliderAu())
            return std::make_unique<SuperColliderRenderedProgram> (lane, sclang);

        return std::make_unique<SuperColliderProgram> (lane, sclang);
    }
};

class FaustDylibProgram final : public AudioProgram
{
public:
    using CreateFn = void* (*)();
    using DestroyFn = void (*) (void*);
    using InitFn = void (*) (void*, int);
    using CountFn = int (*) (void*);
    using ComputeFn = void (*) (void*, int, float**, float**);
    using SetParamFn = bool (*) (void*, const char*, float);
    using ParamLabelFn = const char* (*) (void*, int);
    using ParamValueFn = float (*) (void*, int);

    FaustDylibProgram (LaneDefinition laneToUse,
                       juce::File dylibToUse,
                       std::unique_ptr<juce::DynamicLibrary> libraryToUse,
                       CreateFn createToUse,
                       DestroyFn destroyToUse,
                       InitFn initToUse,
                       CountFn inputsToUse,
                       CountFn outputsToUse,
                       ComputeFn computeToUse,
                       CountFn paramCountToUse,
                       ParamLabelFn paramLabelToUse,
                       ParamValueFn paramValueToUse,
                       ParamValueFn paramDefaultToUse,
                       ParamValueFn paramMinToUse,
                       ParamValueFn paramMaxToUse,
                       ParamValueFn paramStepToUse,
                       SetParamFn setParamToUse)
        : lane (std::move (laneToUse)),
          dylib (std::move (dylibToUse)),
          library (std::move (libraryToUse)),
          create (createToUse),
          destroy (destroyToUse),
          init (initToUse),
          countInputs (inputsToUse),
          countOutputs (outputsToUse),
          compute (computeToUse),
          countParams (paramCountToUse),
          getParamLabel (paramLabelToUse),
          getParamValue (paramValueToUse),
          getParamDefault (paramDefaultToUse),
          getParamMin (paramMinToUse),
          getParamMax (paramMaxToUse),
          getParamStep (paramStepToUse),
          setParam (setParamToUse)
    {
        dsp = create();
        refreshParameters();
    }

    ~FaustDylibProgram() override
    {
        if (dsp != nullptr)
            destroy (dsp);
    }

    void prepare (double sampleRateToUse, int maxBlockSizeToUse, int numChannelsToUse) override
    {
        sampleRate = sampleRateToUse;
        maxBlockSize = juce::jmax (1, maxBlockSizeToUse);
        numChannels = juce::jmax (1, numChannelsToUse);

        if (dsp != nullptr)
        {
            init (dsp, (int) std::round (sampleRate));
            numInputs = juce::jmax (0, countInputs (dsp));
            numOutputs = juce::jmax (0, countOutputs (dsp));
            refreshParameters();
        }

        inputScratch.setSize (juce::jmax (1, numInputs), maxBlockSize, false, false, true);
        outputScratch.setSize (juce::jmax (1, numOutputs), maxBlockSize, false, false, true);
        inputChannels.resize ((size_t) juce::jmax (1, numInputs));
        outputChannels.resize ((size_t) juce::jmax (1, numOutputs));
    }

    void reset() override
    {
        if (dsp != nullptr && sampleRate > 0.0)
            init (dsp, (int) std::round (sampleRate));
    }

    bool setParameter (const juce::String& parameterId, float value) override
    {
        if (parameterId.equalsIgnoreCase ("gain"))
        {
            lane.gain = juce::jlimit (0.0f, 1.5f, value);
            return true;
        }

        auto id = normaliseParameterId (parameterId);
        for (auto& parameter : parameters)
        {
            if (parameter.normalisedLabel == id || parameter.normalisedPath == id)
            {
                auto changed = setParam != nullptr && setParam (dsp, parameter.path.toRawUTF8(), value);
                if (changed)
                    refreshParameters();
                return changed;
            }
        }

        return false;
    }

    juce::Array<AudioParameterInfo> getParameters() const override
    {
        juce::Array<AudioParameterInfo> result;

        for (auto& parameter : parameters)
        {
            AudioParameterInfo info;
            info.id = parameter.path;
            info.label = parameter.label;
            info.currentValue = parameter.currentValue;
            info.defaultValue = parameter.defaultValue;
            info.minimumValue = parameter.minimumValue;
            info.maximumValue = parameter.maximumValue;
            info.step = parameter.step;
            result.add (info);
        }

        AudioParameterInfo gain;
        gain.id = "gain";
        gain.label = "gain";
        gain.currentValue = lane.gain;
        gain.defaultValue = lane.gain;
        gain.minimumValue = 0.0f;
        gain.maximumValue = 1.5f;
        gain.step = 0.01f;
        result.add (gain);

        return result;
    }

    void render (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (dsp == nullptr || lane.muted || numOutputs <= 0)
            return;

        auto remaining = numSamples;
        auto offset = 0;

        while (remaining > 0)
        {
            auto block = juce::jmin (remaining, maxBlockSize);
            inputScratch.clear();
            outputScratch.clear();

            for (int input = 0; input < numInputs; ++input)
            {
                auto sourceChannel = juce::jmin (input, buffer.getNumChannels() - 1);
                inputScratch.copyFrom (input, 0, buffer, sourceChannel, startSample + offset, block);
                inputChannels[(size_t) input] = inputScratch.getWritePointer (input);
            }

            for (int output = 0; output < numOutputs; ++output)
                outputChannels[(size_t) output] = outputScratch.getWritePointer (output);

            compute (dsp,
                     block,
                     numInputs > 0 ? inputChannels.data() : nullptr,
                     numOutputs > 0 ? outputChannels.data() : nullptr);

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto sourceOutput = juce::jmin (channel, numOutputs - 1);
                buffer.addFrom (channel,
                                startSample + offset,
                                outputScratch,
                                sourceOutput,
                                0,
                                block,
                                lane.gain);
            }

            offset += block;
            remaining -= block;
        }
    }

    juce::String describe() const override
    {
        return "Faust DSP " + juce::String (numInputs) + " in / " + juce::String (numOutputs)
             + " out " + juce::String (parameters.size()) + " params gain " + juce::String (lane.gain, 2);
    }

private:
    struct Parameter
    {
        juce::String path;
        juce::String label;
        juce::String normalisedPath;
        juce::String normalisedLabel;
        float currentValue = 0.0f;
        float defaultValue = 0.0f;
        float minimumValue = 0.0f;
        float maximumValue = 1.0f;
        float step = 0.0f;
    };

    void refreshParameters()
    {
        parameters.clear();

        if (dsp == nullptr || countParams == nullptr || getParamLabel == nullptr)
            return;

        auto count = juce::jmax (0, countParams (dsp));
        for (int i = 0; i < count; ++i)
        {
            if (auto* rawLabel = getParamLabel (dsp, i))
            {
                Parameter parameter;
                parameter.path = juce::String::fromUTF8 (rawLabel);
                parameter.label = parameter.path.fromLastOccurrenceOf ("/", false, false);
                parameter.normalisedPath = normaliseParameterId (parameter.path);
                parameter.normalisedLabel = normaliseParameterId (parameter.label);
                parameter.currentValue = getParamValue != nullptr ? getParamValue (dsp, i) : 0.0f;
                parameter.defaultValue = getParamDefault != nullptr ? getParamDefault (dsp, i) : parameter.currentValue;
                parameter.minimumValue = getParamMin != nullptr ? getParamMin (dsp, i) : 0.0f;
                parameter.maximumValue = getParamMax != nullptr ? getParamMax (dsp, i) : 1.0f;
                parameter.step = getParamStep != nullptr ? getParamStep (dsp, i) : 0.0f;
                parameters.push_back (parameter);
            }
        }
    }

    LaneDefinition lane;
    juce::File dylib;
    std::unique_ptr<juce::DynamicLibrary> library;
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    InitFn init = nullptr;
    CountFn countInputs = nullptr;
    CountFn countOutputs = nullptr;
    ComputeFn compute = nullptr;
    CountFn countParams = nullptr;
    ParamLabelFn getParamLabel = nullptr;
    ParamValueFn getParamValue = nullptr;
    ParamValueFn getParamDefault = nullptr;
    ParamValueFn getParamMin = nullptr;
    ParamValueFn getParamMax = nullptr;
    ParamValueFn getParamStep = nullptr;
    SetParamFn setParam = nullptr;
    void* dsp = nullptr;
    double sampleRate = 0.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    int numInputs = 0;
    int numOutputs = 0;
    juce::AudioBuffer<float> inputScratch;
    juce::AudioBuffer<float> outputScratch;
    std::vector<float*> inputChannels;
    std::vector<float*> outputChannels;
    std::vector<Parameter> parameters;
};

class FaustCliHost final : public AudioLanguageHost
{
public:
    juce::String languageId() const override { return "faust"; }

    std::unique_ptr<AudioProgram> compile (const LaneDefinition& lane, juce::String& error) override
    {
        auto faust = findExecutableOnPath ("faust");

        if (! faust.existsAsFile())
        {
            error = "Faust CLI was not found. Install Faust, then this adapter can compile lane DSP.";
            return {};
        }

        auto workingDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("MarkovStudio")
                              .getNonexistentChildFile ("faust-lane", {});
        workingDir.createDirectory();

        auto dspFile = workingDir.getChildFile ("lane.dsp");
        auto generatedFile = workingDir.getChildFile ("lane.cpp");
        auto wrapperFile = workingDir.getChildFile ("wrapper.cpp");
        auto dylibFile = workingDir.getChildFile ("lane.dylib");

        if (! dspFile.replaceWithText (lane.code))
        {
            error = "Could not write Faust lane source.";
            return {};
        }

        int exitCode = 0;
        auto faustOutput = runProcess ({ faust.getFullPathName(),
                                         "-lang", "cpp",
                                         "-cn", "MarkovFaustDSP",
                                         dspFile.getFullPathName(),
                                         "-o", generatedFile.getFullPathName() },
                                       exitCode);

        if (exitCode != 0 || ! generatedFile.existsAsFile())
        {
            error = "Faust compile failed:\n" + faustOutput;
            return {};
        }

        auto includeDir = faust.getParentDirectory().getParentDirectory().getChildFile ("include");
        auto wrapperSource = juce::String (R"CPP(
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
struct Meta { virtual ~Meta() = default; virtual void declare(const char*, const char*) {} };
#include ")CPP") + cIncludePath (generatedFile) + R"CPP("

struct MarkovFaustParameter
{
    std::string path;
    std::string label;
    FAUSTFLOAT* zone = nullptr;
    FAUSTFLOAT init = 0;
    FAUSTFLOAT min = 0;
    FAUSTFLOAT max = 1;
    FAUSTFLOAT step = 0;
};

struct MarkovFaustUI final : public UI
{
    std::vector<std::string> groups;
    std::vector<MarkovFaustParameter> parameters;

    void openTabBox(const char* label) override { pushGroup(label); }
    void openHorizontalBox(const char* label) override { pushGroup(label); }
    void openVerticalBox(const char* label) override { pushGroup(label); }
    void closeBox() override { if (!groups.empty()) groups.pop_back(); }

    void addButton(const char* label, FAUSTFLOAT* zone) override { addParameter(label, zone, 0, 0, 1, 1); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override { addParameter(label, zone, 0, 0, 1, 1); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override { addParameter(label, zone, init, min, max, step); }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override { addParameter(label, zone, init, min, max, step); }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override { addParameter(label, zone, init, min, max, step); }
    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    void pushGroup(const char* label)
    {
        if (label != nullptr && label[0] != 0)
            groups.push_back(label);
    }

    void addParameter(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        MarkovFaustParameter parameter;
        parameter.label = label == nullptr ? "" : label;
        parameter.path = "/";
        for (const auto& group : groups) {
            parameter.path += group;
            parameter.path += "/";
        }
        parameter.path += parameter.label;
        parameter.zone = zone;
        parameter.init = init;
        parameter.min = min;
        parameter.max = max;
        parameter.step = step;
        parameters.push_back(parameter);
    }
};

struct MarkovFaustInstance
{
    MarkovFaustDSP dsp;
    MarkovFaustUI ui;

    MarkovFaustInstance()
    {
        dsp.buildUserInterface(&ui);
    }
};

extern "C" void* markov_faust_create() { return new MarkovFaustInstance(); }
extern "C" void markov_faust_destroy(void* p) { delete static_cast<MarkovFaustInstance*>(p); }
extern "C" void markov_faust_init(void* p, int sampleRate)
{
    auto* instance = static_cast<MarkovFaustInstance*>(p);
    instance->dsp.init(sampleRate);
    instance->ui.parameters.clear();
    instance->dsp.buildUserInterface(&instance->ui);
}
extern "C" int markov_faust_inputs(void* p) { return static_cast<MarkovFaustInstance*>(p)->dsp.getNumInputs(); }
extern "C" int markov_faust_outputs(void* p) { return static_cast<MarkovFaustInstance*>(p)->dsp.getNumOutputs(); }
extern "C" void markov_faust_compute(void* p, int frames, float** inputs, float** outputs)
{
    static_cast<MarkovFaustInstance*>(p)->dsp.compute(frames, inputs, outputs);
}
extern "C" int markov_faust_param_count(void* p)
{
    return static_cast<int>(static_cast<MarkovFaustInstance*>(p)->ui.parameters.size());
}
extern "C" const char* markov_faust_param_label(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return nullptr;
    return parameters[static_cast<size_t>(index)].path.c_str();
}
extern "C" float markov_faust_param_value(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()) || parameters[static_cast<size_t>(index)].zone == nullptr)
        return 0.0f;
    return *parameters[static_cast<size_t>(index)].zone;
}
extern "C" float markov_faust_param_default(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return 0.0f;
    return parameters[static_cast<size_t>(index)].init;
}
extern "C" float markov_faust_param_min(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return 0.0f;
    return parameters[static_cast<size_t>(index)].min;
}
extern "C" float markov_faust_param_max(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return 1.0f;
    return parameters[static_cast<size_t>(index)].max;
}
extern "C" float markov_faust_param_step(void* p, int index)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return 0.0f;
    return parameters[static_cast<size_t>(index)].step;
}
extern "C" bool markov_faust_set_param(void* p, const char* path, float value)
{
    auto& parameters = static_cast<MarkovFaustInstance*>(p)->ui.parameters;
    std::string target = path == nullptr ? "" : path;

    for (auto& parameter : parameters) {
        if (parameter.path == target || parameter.label == target) {
            if (parameter.zone != nullptr) {
                *parameter.zone = std::max<float>(parameter.min, std::min<float>(parameter.max, value));
                return true;
            }
        }
    }

    return false;
}
)CPP";

        if (! wrapperFile.replaceWithText (wrapperSource))
        {
            error = "Could not write Faust wrapper source.";
            return {};
        }

        auto clangOutput = runProcess ({ "/usr/bin/clang++",
                                         "-std=c++17",
                                         "-dynamiclib",
                                         wrapperFile.getFullPathName(),
                                         "-I", includeDir.getFullPathName(),
                                         "-o", dylibFile.getFullPathName() },
                                       exitCode);

        if (exitCode != 0 || ! dylibFile.existsAsFile())
        {
            error = "Faust dylib build failed:\n" + clangOutput;
            return {};
        }

        auto library = std::make_unique<juce::DynamicLibrary>();
        if (! library->open (dylibFile.getFullPathName()))
        {
            error = "Could not load Faust dylib: " + dylibFile.getFullPathName();
            return {};
        }

        auto create = reinterpret_cast<FaustDylibProgram::CreateFn> (library->getFunction ("markov_faust_create"));
        auto destroy = reinterpret_cast<FaustDylibProgram::DestroyFn> (library->getFunction ("markov_faust_destroy"));
        auto init = reinterpret_cast<FaustDylibProgram::InitFn> (library->getFunction ("markov_faust_init"));
        auto inputs = reinterpret_cast<FaustDylibProgram::CountFn> (library->getFunction ("markov_faust_inputs"));
        auto outputs = reinterpret_cast<FaustDylibProgram::CountFn> (library->getFunction ("markov_faust_outputs"));
        auto compute = reinterpret_cast<FaustDylibProgram::ComputeFn> (library->getFunction ("markov_faust_compute"));
        auto paramCount = reinterpret_cast<FaustDylibProgram::CountFn> (library->getFunction ("markov_faust_param_count"));
        auto paramLabel = reinterpret_cast<FaustDylibProgram::ParamLabelFn> (library->getFunction ("markov_faust_param_label"));
        auto paramValue = reinterpret_cast<FaustDylibProgram::ParamValueFn> (library->getFunction ("markov_faust_param_value"));
        auto paramDefault = reinterpret_cast<FaustDylibProgram::ParamValueFn> (library->getFunction ("markov_faust_param_default"));
        auto paramMin = reinterpret_cast<FaustDylibProgram::ParamValueFn> (library->getFunction ("markov_faust_param_min"));
        auto paramMax = reinterpret_cast<FaustDylibProgram::ParamValueFn> (library->getFunction ("markov_faust_param_max"));
        auto paramStep = reinterpret_cast<FaustDylibProgram::ParamValueFn> (library->getFunction ("markov_faust_param_step"));
        auto setParam = reinterpret_cast<FaustDylibProgram::SetParamFn> (library->getFunction ("markov_faust_set_param"));

        if (create == nullptr || destroy == nullptr || init == nullptr || inputs == nullptr || outputs == nullptr || compute == nullptr
            || paramCount == nullptr || paramLabel == nullptr || paramValue == nullptr || paramDefault == nullptr
            || paramMin == nullptr || paramMax == nullptr || paramStep == nullptr || setParam == nullptr)
        {
            error = "Faust dylib is missing expected entry points.";
            return {};
        }

        error.clear();
        return std::make_unique<FaustDylibProgram> (lane,
                                                    dylibFile,
                                                    std::move (library),
                                                    create,
                                                    destroy,
                                                    init,
                                                    inputs,
                                                    outputs,
                                                    compute,
                                                    paramCount,
                                                    paramLabel,
                                                    paramValue,
                                                    paramDefault,
                                                    paramMin,
                                                    paramMax,
                                                    paramStep,
                                                    setParam);
    }
};
}

AudioLanguageRegistry::AudioLanguageRegistry()
{
    hosts.add (new MiniToneHost());
    hosts.add (new FaustCliHost());
    hosts.add (new CsoundCliHost());
    hosts.add (new CmajorCliHost());
    hosts.add (new ChuckCliHost());
    hosts.add (new PlaceholderHost ("rtcmix"));
    hosts.add (new SuperColliderHost());
}

std::unique_ptr<AudioProgram> AudioLanguageRegistry::compile (const LaneDefinition& lane, juce::String& error) const
{
    for (auto* host : hosts)
        if (host->languageId().equalsIgnoreCase (lane.language))
            return host->compile (lane, error);

    error = "Unknown audio language: " + lane.language;
    return {};
}

juce::StringArray AudioLanguageRegistry::getLanguageIds() const
{
    juce::StringArray ids;
    for (auto* host : hosts)
        ids.add (host->languageId());
    return ids;
}
