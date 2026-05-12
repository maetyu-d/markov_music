cmake_minimum_required(VERSION 3.22)

project(MarkovMusic VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)

FetchContent_MakeAvailable(lua)

add_library(lua_static STATIC
    ${lua_SOURCE_DIR}/src/lapi.c
    ${lua_SOURCE_DIR}/src/lauxlib.c
    ${lua_SOURCE_DIR}/src/lbaselib.c
    ${lua_SOURCE_DIR}/src/lcode.c
    ${lua_SOURCE_DIR}/src/lcorolib.c
    ${lua_SOURCE_DIR}/src/lctype.c
    ${lua_SOURCE_DIR}/src/ldblib.c
    ${lua_SOURCE_DIR}/src/ldebug.c
    ${lua_SOURCE_DIR}/src/ldo.c
    ${lua_SOURCE_DIR}/src/ldump.c
    ${lua_SOURCE_DIR}/src/lfunc.c
    ${lua_SOURCE_DIR}/src/lgc.c
    ${lua_SOURCE_DIR}/src/linit.c
    ${lua_SOURCE_DIR}/src/liolib.c
    ${lua_SOURCE_DIR}/src/llex.c
    ${lua_SOURCE_DIR}/src/lmathlib.c
    ${lua_SOURCE_DIR}/src/lmem.c
    ${lua_SOURCE_DIR}/src/loadlib.c
    ${lua_SOURCE_DIR}/src/lobject.c
    ${lua_SOURCE_DIR}/src/lopcodes.c
    ${lua_SOURCE_DIR}/src/loslib.c
    ${lua_SOURCE_DIR}/src/lparser.c
    ${lua_SOURCE_DIR}/src/lstate.c
    ${lua_SOURCE_DIR}/src/lstring.c
    ${lua_SOURCE_DIR}/src/lstrlib.c
    ${lua_SOURCE_DIR}/src/ltable.c
    ${lua_SOURCE_DIR}/src/ltablib.c
    ${lua_SOURCE_DIR}/src/ltm.c
    ${lua_SOURCE_DIR}/src/lundump.c
    ${lua_SOURCE_DIR}/src/lutf8lib.c
    ${lua_SOURCE_DIR}/src/lvm.c
    ${lua_SOURCE_DIR}/src/lzio.c)

target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)

set(JUCE_PATH "/Users/user/Documents/Granny/JUCE" CACHE PATH "Path to a JUCE checkout")
add_subdirectory(${JUCE_PATH} JUCE)

juce_add_gui_app(MarkovStudio
    PRODUCT_NAME "Markov Music by matd.space"
    COMPANY_NAME "matd.space"
    BUNDLE_ID "space.matd.markovmusic"
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(MarkovStudio PRIVATE
    Source/AudioLanguages.cpp
    Source/ControlScripting.cpp
    Source/Main.cpp
    Source/MarkovEngine.cpp
    Source/StateMapComponent.cpp)

target_include_directories(MarkovStudio PRIVATE
    Source
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/chuck/source/chuck/src/core)

target_compile_definitions(MarkovStudio PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MarkovStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    MARKOV_BUNDLED_FAUST_BIN="${CMAKE_CURRENT_SOURCE_DIR}/tools/faust/current/bin/faust"
    MARKOV_BUNDLED_CSOUND_BIN="${CMAKE_CURRENT_SOURCE_DIR}/tools/csound/current/bin/csound"
    MARKOV_BUNDLED_CSOUND_LIB="${CMAKE_CURRENT_SOURCE_DIR}/tools/csound/current/Library/Frameworks/CsoundLib64.framework/Versions/6.0/CsoundLib64"
    MARKOV_BUNDLED_CMAJ_BIN="${CMAKE_CURRENT_SOURCE_DIR}/tools/cmajor/current/bin/cmaj"
    MARKOV_BUNDLED_CHUCK_BIN="${CMAKE_CURRENT_SOURCE_DIR}/tools/chuck/current/bin/chuck"
    MARKOV_HAS_CHUCK_CORE=1
    MARKOV_HAS_LUA=1
    JUCE_PLUGINHOST_AU=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(MarkovStudio PRIVATE
    lua_static
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/chuck/current/lib/libchuckcore.a
    "-framework CoreAudio"
    "-framework CoreMIDI"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework Carbon"
    "-framework AppKit"
    "-framework Foundation"
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_osc
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

target_link_options(MarkovStudio PRIVATE
    "-F/System/Library/PrivateFrameworks"
    "-weak_framework"
    "MultitouchSupport")
