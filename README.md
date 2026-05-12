# Markov Music by matd.space

A JUCE/C++ sketch for a generative composition app where a Markov state machine moves between musical sections such as verse, chorus, and bridge.

Each Markov state owns instrument lanes. A lane declares an audio language and a code block. `faust` lanes can compile and run through the project-local Faust toolchain. `csound`, `cmajor`, `chuck`, and `supercollider` lanes can render through command-line engines and loop their rendered audio inside the JUCE host. `minitone` is also included as a tiny built-in synth language for quick sketches and host testing.

The intended lane languages are `faust`, `cmajor`, `csound`, `chuck`, `rtcmix`, and `supercollider`. Faust, Csound, Cmajor, ChucK, and SuperCollider are active now; RTcmix is registered as an adapter slot, so it can be implemented behind `AudioLanguageHost` without changing the Markov state model.

Lua and Python are reserved for control scripting: transition policy, lane parameter changes, event scheduling, and higher-level generative rules. They are not treated as audio-rate lane languages.

## Build

```sh
cmake -S . -B build -DJUCE_PATH=/Users/user/Documents/Granny/JUCE
cmake --build build
open "build/MarkovStudio_artefacts/Markov Music by matd.space.app"
```

Faust 2.85.5 is installed project-locally at:

```sh
tools/faust/bin/faust
```

The app is built with that bundled Faust path, so `faust` lanes can detect the compiler even if it is not installed globally.

Csound 6.18.1 is installed project-locally at:

```sh
tools/csound/current/bin/csound
```

The launcher points Csound at the extracted local framework, so it does not require a system-wide `/Library/Frameworks` install.

Cmajor 1.0.3066 is installed project-locally at:

```sh
tools/cmajor/current/bin/cmaj
```

ChucK 1.5.5.7 is installed project-locally at:

```sh
tools/chuck/current/bin/chuck
```

You can verify the active adapters without opening the UI:

```sh
"build/MarkovStudio_artefacts/Markov Music by matd.space.app/Contents/MacOS/Markov Music by matd.space" --faust-smoke
"build/MarkovStudio_artefacts/Markov Music by matd.space.app/Contents/MacOS/Markov Music by matd.space" --csound-smoke
"build/MarkovStudio_artefacts/Markov Music by matd.space.app/Contents/MacOS/Markov Music by matd.space" --cmajor-smoke
"build/MarkovStudio_artefacts/Markov Music by matd.space.app/Contents/MacOS/Markov Music by matd.space" --chuck-smoke
"build/MarkovStudio_artefacts/Markov Music by matd.space.app/Contents/MacOS/Markov Music by matd.space" --export-smoke
```

## Script Shape

The app edits JSON directly for now:

```json
{
  "bpm": 112,
  "control": {
    "language": "lua",
    "code": "function on_state_enter(ctx)\n  ctx:set('Lead', 'gain', 0.35)\nend\n\nfunction choose_next_state(ctx)\n  if ctx.current == 'verse_a' and ctx.visit_count > 1 then return 'chorus' end\n  return ctx:choose_weighted()\nend"
  },
  "states": [
    {
      "id": "verse_a",
      "section": "Verse",
      "durationBeats": 16,
      "transitions": [{ "to": "chorus", "weight": 0.6 }],
      "lanes": [
        {
          "name": "Bass",
          "language": "minitone",
          "gain": 0.6,
          "code": "wave=saw freq=110 pulse=2 tone=0.35 pan=0 bpm=112"
        }
      ]
    }
  ]
}
```

Lua control scripts can define state-entry and transition hooks:

```lua
function on_state_enter(ctx)
  ctx:set("Lead", "gain", 0.35)
end

function choose_next_state(ctx)
  -- ctx.current, ctx.section, ctx.visit_count, ctx.choices
  if ctx.current == "verse_a" and ctx.visit_count > 1 then
    return "chorus"
  end

  return ctx:choose_weighted() -- or return nil to fall back to C++ weighting
end
```

For the current built-in `minitone` lanes, `ctx:set` supports `gain`, `freq`, `frequency`, `tone`, `pan`, `pulse`, and `bpm`. The same control path will be used for Faust/Cmajor/etc. parameters as those adapters come online.

For `faust` lanes, slider/button/entry labels are discovered from the Faust UI. For example, this lane:

```faust
import("stdfaust.lib");
freq = hslider("freq", 440, 80, 2000, 1);
tonegain = hslider("tonegain", 0.2, 0, 1, 0.01);
process = os.osc(freq) * tonegain;
```

can be controlled with:

```lua
ctx:set("Lead", "freq", 660)
ctx:set("Lead", "tonegain", 0.3)
```

The app side panel also lists discovered runtime parameters for compiled lanes, including their current value and range.
Use the state, lane, and parameter selectors above the inspector to choose a runtime parameter and move the slider. Slider edits update the active compiled program and are written back into the JSON script as lane `params`.

```json
{
  "name": "Future Faust lane",
  "language": "faust",
  "gain": 0.4,
  "params": {
    "freq": 660,
    "tonegain": 0.3
  },
  "code": "..."
}
```

Lane `params` are applied after compile and after resets, so they act as the saved starting values before Lua control scripts make any later changes.

Transition weights are also editable from the side panel. Choose a state, choose one of its outgoing transitions, then move the transition weight slider; the JSON script is rewritten and the Markov engine reloads with the new weights.

State durations are editable in the same side panel. The duration slider writes `durationBeats` back into the selected state and reloads the engine.

Global tempo is editable from the tempo slider. It rewrites the top-level `bpm` value, reloads the engine, and keeps the current editor selections where possible.

The state template selector chooses whether Add State creates a Verse, Chorus, Bridge, Intro, Outro, or Breakdown. Add State appends the section to the JSON, gives it one `minitone` lane, adds a default transition, reloads the engine, and selects the new state so it can be shaped immediately. Duplicate State and Delete State work on the selected state.

The lane template selector chooses the starter template used by Add Lane. It can append Minitone bass/pulse/texture, Faust oscillator/filter, Csound drone/pluck, Cmajor sine, ChucK sine/pulse, and SuperCollider synth templates. Faust, Csound, Cmajor, ChucK, and SuperCollider templates are playable now; RTcmix templates are stored in the script but remain silent until their host is wired.

Selecting a lane fills the lane editor above the raw JSON. From there you can edit the lane name, language, gain, mute state, and code, then apply the lane back into the script and hot-reload the engine. Re-render reloads the current script and rebuilds rendered lanes. Duplicate and Delete work on the selected lane, which makes it faster to build variations inside a section without hand-editing JSON.

The selected state also has direct edit fields for its id and section label. Applying a state id change updates incoming transitions that referenced the old id, then reloads the engine. The transition target selector can add a new outgoing transition from the current state, and Delete Transition removes the selected outgoing transition.

Open, Save, and Snapshot in the header work with `.markov.json` project files. Save writes the current JSON, Open loads and applies a saved project, and Snapshot writes a timestamped copy next to the current project or in Documents. Export renders a 60-second WAV snapshot from the current script using a separate offline engine, so it does not disturb live playback.

Csound lanes can be written as an instrument body or as a complete `.csd`. Instrument-body lanes are wrapped with a default Csound document, a sine table, and a score event for instrument 1. Add a lane `params.duration` value to choose the offline render length in seconds; otherwise the loop renders 16 seconds.

Cmajor lanes are written as `.cmajor` source. The adapter wraps the source in a temporary `.cmajorpatch`, calls `cmaj render`, reads the WAV output, and loops it inside the JUCE audio engine. Add a lane `params.duration` value to choose the offline render length in seconds; otherwise the loop renders 8 seconds.

ChucK lanes render by writing a WAV with `WvOut2`. Use `{{output}}` in ChucK code for the generated output path. Rendered-lane templates support `{{freq}}`, `{{amp}}`, `{{cutoff}}`, and `{{duration}}` placeholders; the same names appear in the parameter selector and can be changed from Lua with `ctx:set`.

SuperCollider lanes render through the installed `/Applications/SuperCollider.app/Contents/MacOS/sclang`. Lane code is treated as a SuperCollider audio-rate expression, with `freq`, `amp`, and `cutoff` available as SynthDef controls. The rendered WAV loop is read back into the JUCE engine and cached in memory, so loaded nodes can start immediately without rerendering unchanged code. The experimental SuperColliderAU live engine can be enabled with `MARKOV_SUPERCOLLIDER_AU=1`, but the stable renderer is the default because it avoids section-change gaps and silent AU starts.

## Language Adapter Plan

| Language | Role | Likely Integration |
|---|---|---|
| Faust | Audio-rate DSP lanes | Compile Faust DSP to C++/LLVM/WASM and wrap as `AudioProgram`. |
| Cmajor | Audio-rate DSP lanes | Active via project-local `cmaj render` to loop; later upgrade path is embedded performer/libCmajPerformer. |
| Csound | Synthesis/effects lanes | Active via project-local Csound CLI render-to-loop; later upgrade path is embedded libcsound for live control. |
| ChucK | Timed procedural lanes | Active via project-local ChucK CLI render-to-loop using `WvOut2`; later upgrade path is hosted VM instances. |
| RTcmix | Score/instrument lanes | Prefer external process or library adapter depending on availability. |
| SuperCollider | Generative synth lanes | Active via installed `sclang` non-realtime render to loop; later upgrade path is `scsynth`/SuperColliderAU hosting for live control. |
| Lua | Control | Embed Lua VM for deterministic transition and parameter scripts. |
| Python | Control | Use for slower/higher-level control, offline generation, or supervised scripts outside the realtime callback. |
