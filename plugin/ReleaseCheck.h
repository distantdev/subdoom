#pragma once

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// One GitHub release. updateAvailable is set only when tag_name is newer than
// the running plugin and htmlUrl is an https://github.com release page.
struct ReleaseInfo
{
    juce::String tag;
    juce::String htmlUrl;
    bool updateAvailable = false;
};

// Leading v/V is ignored. A pre-release suffix from the first '-' is ignored.
// Components are the leading integers of each dot-separated field. Missing
// fields count as zero, so "1.2" and "1.2.0" are the same version.
bool isNewerRelease(const juce::String& latestTag, const juce::String& currentVersion);

// One in-flight GET /repos/{owner}/{repo}/releases/latest.
//
// Owner, repo, and the current version are not hardcoded here. Each plugin's
// CMakeLists.txt publishes them as compile definitions, and the version
// definition follows that project's VERSION.
//
// check() returns immediately. The request runs on a worker thread. A result
// is posted with MessageManager::callAsync and dropped if the editor is gone
// or cancel() has run. cancel() is safe to call more than once and from the
// checker destructor. Call it on the message thread; it joins the worker.
class ReleaseChecker
{
public:
    struct Target
    {
        juce::String owner;
        juce::String repo;
        juce::String currentVersion;
        juce::String userAgent;
    };

    ReleaseChecker() = default;
    ~ReleaseChecker();

    ReleaseChecker(const ReleaseChecker&) = delete;
    ReleaseChecker& operator=(const ReleaseChecker&) = delete;

    void check(Target target,
               juce::Component::SafePointer<juce::Component> editor,
               std::function<void(const ReleaseInfo&)> onResult);

    void cancel();

private:
    void joinWorker();
    void clearStream(const std::shared_ptr<juce::WebInputStream>& stream);
    void fetchAndPublish(Target target,
                         juce::Component::SafePointer<juce::Component> editor,
                         std::function<void(const ReleaseInfo&)> onResult,
                         const std::shared_ptr<std::atomic<bool>>& stopFlag);

    std::mutex mutex;
    std::shared_ptr<std::atomic<bool>> stop;
    std::shared_ptr<juce::WebInputStream> activeStream;
    std::thread worker;
};
