#include "ControlScripting.h"

#if MARKOV_HAS_LUA
extern "C"
{
    #include <lauxlib.h>
    #include <lua.h>
    #include <lualib.h>
}
#endif

namespace
{
static bool isValidChoice (const ControlRequest& request, const juce::String& stateId)
{
    for (auto& choice : request.choices)
        if (choice.id == stateId)
            return true;

    return false;
}

#if MARKOV_HAS_LUA
struct LuaBridge
{
    std::vector<ControlParameterChange>* parameterChanges = nullptr;
    const ControlRequest* request = nullptr;
};

struct LuaState
{
    LuaState() : state (luaL_newstate()) {}
    ~LuaState()
    {
        if (state != nullptr)
            lua_close (state);
    }

    lua_State* state = nullptr;
};

static int luaSetParameter (lua_State* state)
{
    auto* bridge = static_cast<LuaBridge*> (lua_touserdata (state, lua_upvalueindex (1)));
    if (bridge == nullptr || bridge->parameterChanges == nullptr)
        return 0;

    auto laneName = luaL_checkstring (state, 1);
    auto parameterId = luaL_checkstring (state, 2);
    auto value = (float) luaL_checknumber (state, 3);

    ControlParameterChange change;
    change.laneName = juce::String::fromUTF8 (laneName);
    change.parameterId = juce::String::fromUTF8 (parameterId);
    change.value = value;
    bridge->parameterChanges->push_back (change);
    return 0;
}

static int luaChooseWeighted (lua_State* state)
{
    auto* bridge = static_cast<LuaBridge*> (lua_touserdata (state, lua_upvalueindex (1)));
    if (bridge == nullptr || bridge->request == nullptr || bridge->request->choices.isEmpty())
        return 0;

    auto totalWeight = 0.0;
    for (auto& choice : bridge->request->choices)
        totalWeight += choice.weight;

    if (totalWeight <= 0.0)
    {
        lua_pushstring (state, bridge->request->choices[0].id.toRawUTF8());
        return 1;
    }

    auto pick = juce::Random::getSystemRandom().nextDouble() * totalWeight;
    for (auto& choice : bridge->request->choices)
    {
        pick -= choice.weight;
        if (pick <= 0.0)
        {
            lua_pushstring (state, choice.id.toRawUTF8());
            return 1;
        }
    }

    lua_pushstring (state, bridge->request->choices.getLast().id.toRawUTF8());
    return 1;
}

static void openSafeLuaLibraries (lua_State* state)
{
    luaL_requiref (state, "_G", luaopen_base, 1);
    lua_pop (state, 1);
    luaL_requiref (state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop (state, 1);
    luaL_requiref (state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop (state, 1);
    luaL_requiref (state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop (state, 1);
}

static void pushContext (lua_State* state, const ControlRequest& request, LuaBridge& bridge)
{
    lua_newtable (state);

    lua_pushlightuserdata (state, &bridge);
    lua_pushcclosure (state, luaSetParameter, 1);
    lua_setfield (state, -2, "set");

    lua_pushlightuserdata (state, &bridge);
    lua_pushcclosure (state, luaChooseWeighted, 1);
    lua_setfield (state, -2, "choose_weighted");

    lua_pushstring (state, request.currentStateId.toRawUTF8());
    lua_setfield (state, -2, "current");

    lua_pushstring (state, request.currentSection.toRawUTF8());
    lua_setfield (state, -2, "section");

    lua_pushinteger (state, request.visitCount);
    lua_setfield (state, -2, "visit_count");

    lua_newtable (state);
    int index = 1;
    for (auto& choice : request.choices)
    {
        lua_newtable (state);

        lua_pushstring (state, choice.id.toRawUTF8());
        lua_setfield (state, -2, "id");

        lua_pushstring (state, choice.section.toRawUTF8());
        lua_setfield (state, -2, "section");

        lua_pushnumber (state, choice.weight);
        lua_setfield (state, -2, "weight");

        lua_rawseti (state, -2, index++);
    }
    lua_setfield (state, -2, "choices");
}

static juce::String readLuaError (lua_State* state)
{
    auto error = lua_tostring (state, -1);
    return error == nullptr ? "Unknown Lua error." : juce::String::fromUTF8 (error);
}
#endif
}

ControlResult ControlScriptRunner::chooseNextState (const ControlRequest& request) const
{
    if (request.code.trim().isEmpty())
        return {};

    if (request.language == "lua")
        return runLua (request, "choose_next_state", true);

    if (request.language == "python")
        return runPythonPlaceholder (request);

    return { {}, "Unsupported control language: " + request.language, {} };
}

ControlResult ControlScriptRunner::enterState (const ControlRequest& request) const
{
    if (request.code.trim().isEmpty())
        return {};

    if (request.language == "lua")
        return runLua (request, "on_state_enter", false);

    if (request.language == "python")
        return runPythonPlaceholder (request);

    return { {}, "Unsupported control language: " + request.language, {} };
}

ControlResult ControlScriptRunner::runLua (const ControlRequest& request,
                                           const juce::String& functionName,
                                           bool allowReturnedState) const
{
#if MARKOV_HAS_LUA
    ControlResult result;
    LuaState lua;
    if (lua.state == nullptr)
        return { {}, "Could not create Lua state.", {} };

    openSafeLuaLibraries (lua.state);

    if (luaL_loadstring (lua.state, request.code.toRawUTF8()) != LUA_OK)
        return { {}, readLuaError (lua.state), {} };

    if (lua_pcall (lua.state, 0, 1, 0) != LUA_OK)
        return { {}, readLuaError (lua.state), {} };

    if (allowReturnedState && lua_isstring (lua.state, -1))
    {
        auto chosen = juce::String::fromUTF8 (lua_tostring (lua.state, -1));
        result.nextStateId = isValidChoice (request, chosen) ? chosen : juce::String();
        result.error = result.nextStateId.isEmpty() ? "Lua returned unavailable state: " + chosen : juce::String();
        return result;
    }

    lua_pop (lua.state, 1);
    lua_getglobal (lua.state, functionName.toRawUTF8());
    if (! lua_isfunction (lua.state, -1))
        return result;

    LuaBridge bridge { &result.parameterChanges, &request };
    pushContext (lua.state, request, bridge);

    if (lua_pcall (lua.state, 1, 1, 0) != LUA_OK)
        return { {}, readLuaError (lua.state), result.parameterChanges };

    if (! allowReturnedState || ! lua_isstring (lua.state, -1))
        return result;

    auto chosen = juce::String::fromUTF8 (lua_tostring (lua.state, -1));
    result.nextStateId = isValidChoice (request, chosen) ? chosen : juce::String();
    result.error = result.nextStateId.isEmpty() ? "Lua returned unavailable state: " + chosen : juce::String();
    return result;
#else
    juce::ignoreUnused (request);
    return { {}, "Lua is not enabled in this build." };
#endif
}

ControlResult ControlScriptRunner::runPythonPlaceholder (const ControlRequest& request) const
{
    juce::ignoreUnused (request);
    return { {}, "Python control is reserved for the next adapter; use Lua for now.", {} };
}
