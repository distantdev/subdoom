cmake_minimum_required(VERSION 3.25)

project(Subdoom
    VERSION 0.1.1
    DESCRIPTION "Downtuned bass processor"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CPM.cmake)

option(SUBDOOM_BUILD_PLUGIN "Build the JUCE 9 VST3, CLAP, and Standalone targets" ON)
option(SUBDOOM_BUILD_CLAP "Build the CLAP target (requires the plugin)" ON)
option(SUBDOOM_BUILD_TESTS "Build the headless DSP tests" ON)

add_library(subdoom_dsp STATIC
    dsp/src/Processor.cpp
    dsp/src/FactoryPresets.cpp
    dsp/src/PresetIO.cpp
)
target_include_directories(subdoom_dsp PUBLIC dsp/include)
target_compile_features(subdoom_dsp PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(subdoom_dsp PRIVATE /W4 /permissive-)
else()
    target_compile_options(subdoom_dsp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SUBDOOM_BUILD_TESTS)
    enable_testing()

    add_executable(subdoom_test_cli tests/test_cli_main.cpp)
    target_link_libraries(subdoom_test_cli PRIVATE subdoom_dsp)
    if(MSVC)
        target_compile_options(subdoom_test_cli PRIVATE /W4 /permissive-)
    else()
        target_compile_options(subdoom_test_cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME cli_self_test COMMAND subdoom_test_cli --self-test)
    add_test(NAME cli_impulse COMMAND subdoom_test_cli --signal impulse --blocks 32 --json)
    add_test(NAME cli_noise COMMAND subdoom_test_cli --signal noise --blocks 48 --json)
    add_test(NAME cli_zero COMMAND subdoom_test_cli --signal zero --blocks 16 --json)

    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1
        OPTIONS
            "CATCH_INSTALL_DOCS OFF"
            "CATCH_BUILD_TESTING OFF"
    )

    if(Catch2_ADDED)
        add_executable(subdoom_unit_tests tests/test_dsp.cpp)
        target_link_libraries(subdoom_unit_tests PRIVATE subdoom_dsp Catch2::Catch2WithMain)
        if(MSVC)
            target_compile_options(subdoom_unit_tests PRIVATE /W4 /permissive-)
        else()
            target_compile_options(subdoom_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME unit_tests COMMAND subdoom_unit_tests)
    endif()
endif()

if(SUBDOOM_BUILD_PLUGIN AND MINGW)
    message(FATAL_ERROR
        "JUCE 8 removed MinGW support. Use the MSVC desktop toolset for the plugin, "
        "or configure -DSUBDOOM_BUILD_PLUGIN=OFF to build the DSP tests.")
endif()

if(SUBDOOM_BUILD_PLUGIN)
    CPMAddPackage(
        NAME JUCE
        GITHUB_REPOSITORY juce-framework/JUCE
        GIT_TAG 9.0.5
        OPTIONS
            "JUCE_BUILD_EXAMPLES OFF"
            "JUCE_BUILD_EXTRAS OFF"
    )

    juce_add_plugin(Subdoom
        COMPANY_NAME "Distant Nebula"
        BUNDLE_ID "com.subdoom.plugin"
        ICON_BIG plugin/assets/subdoom_icon_512.png
        ICON_SMALL plugin/assets/subdoom_icon_32.png
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGIN_MANUFACTURER_CODE Dmzr
        PLUGIN_CODE Sbdm
        FORMATS VST3 Standalone
        PRODUCT_NAME "Subdoom"
        VST3_CATEGORIES Fx Distortion
    )

    target_sources(Subdoom PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp
        plugin/PresetLibrary.cpp
        plugin/ReleaseCheck.cpp
    )

    # GitHub release check. Owner and repo are the public repository. The version
    # string tracks project() VERSION, which juce_add_plugin publishes as well.
    set(SUBDOOM_RELEASE_DEFS
        "SUBDOOM_GITHUB_OWNER=\"distantdev\""
        "SUBDOOM_GITHUB_REPO=\"subdoom\""
        "SUBDOOM_VERSION_STRING=\"${PROJECT_VERSION}\""
    )

    target_compile_definitions(Subdoom PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_ASIO=1
        ${SUBDOOM_RELEASE_DEFS}
    )

    target_link_libraries(Subdoom PRIVATE
        subdoom_dsp
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    if(SUBDOOM_BUILD_CLAP)
        CPMAddPackage(
            NAME clap-juce-extensions
            GITHUB_REPOSITORY free-audio/clap-juce-extensions
            GIT_TAG 6bde108329ff9fc24404b79b8b397261a75cf6a8
        )
        if(clap-juce-extensions_ADDED)
            clap_juce_extensions_plugin(
                TARGET Subdoom
                CLAP_ID "com.subdoom.plugin"
                CLAP_FEATURES audio-effect distortion
            )
        endif()
    endif()

    # The wrapper layer only exists in a plugin tree, so its tests live here rather
    # than in the plugin-off audit tree. They need JUCE for the AudioProcessor and a
    # MessageManager, but create no editor and touch no audio device.
    if(SUBDOOM_BUILD_TESTS AND Catch2_ADDED)
        add_executable(subdoom_plugin_tests
            tests/test_plugin.cpp
            plugin/PluginProcessor.cpp
            plugin/PluginEditor.cpp
            plugin/PresetLibrary.cpp
            plugin/ReleaseCheck.cpp
        )
        target_include_directories(subdoom_plugin_tests PRIVATE plugin)
        target_compile_definitions(subdoom_plugin_tests PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_MODAL_LOOPS_PERMITTED=1
            ${SUBDOOM_RELEASE_DEFS}
        )
        target_link_libraries(subdoom_plugin_tests PRIVATE
            subdoom_dsp
            Catch2::Catch2
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
        )
        if(MSVC)
            target_compile_options(subdoom_plugin_tests PRIVATE /W4 /permissive-)
        else()
            target_compile_options(subdoom_plugin_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME plugin_tests COMMAND subdoom_plugin_tests)
    endif()
endif()
