#include "ReleaseCheck.h"

#include <vector>

namespace {

constexpr int kConnectionTimeoutMs = 2500;
constexpr int kMaxBodyBytes = 256 * 1024;

int leadingInt(const juce::String& token)
{
    int value = 0;
    bool sawDigit = false;
    for (int i = 0; i < token.length(); ++i)
    {
        const auto ch = token[i];
        if (ch < '0' || ch > '9')
            break;
        sawDigit = true;
        if (value > 100000000)
            return 1000000000;
        value = value * 10 + static_cast<int>(ch - '0');
    }
    return sawDigit ? value : 0;
}

std::vector<int> numericParts(juce::String text)
{
    text = text.trim();
    if (text.startsWithChar('v') || text.startsWithChar('V'))
        text = text.substring(1);

    const int dash = text.indexOfChar('-');
    if (dash >= 0)
        text = text.substring(0, dash);

    text = text.trim();
    if (text.isEmpty())
        return {};

    std::vector<int> parts;
    int start = 0;
    for (int i = 0; i <= text.length(); ++i)
    {
        if (i == text.length() || text[i] == '.')
        {
            parts.push_back(leadingInt(text.substring(start, i)));
            start = i + 1;
        }
    }
    return parts;
}

bool isGitHubToken(const juce::String& text)
{
    if (text.isEmpty() || text.length() > 100)
        return false;

    for (int i = 0; i < text.length(); ++i)
    {
        const auto ch = text[i];
        const bool ok = (ch >= 'A' && ch <= 'Z')
                        || (ch >= 'a' && ch <= 'z')
                        || (ch >= '0' && ch <= '9')
                        || ch == '-' || ch == '_' || ch == '.';
        if (!ok)
            return false;
    }
    return true;
}

juce::String cleanUserAgent(juce::String agent)
{
    agent = agent.replace("\r", {}).replace("\n", {}).trim();
    if (agent.isEmpty())
        return "doom-audio";
    if (agent.length() > 200)
        agent = agent.substring(0, 200);
    return agent;
}

} // namespace

bool isNewerRelease(const juce::String& latestTag, const juce::String& currentVersion)
{
    const auto latest = numericParts(latestTag);
    if (latest.empty())
        return false;

    const auto current = numericParts(currentVersion);
    const auto count = latest.size() > current.size() ? latest.size() : current.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const int lhs = index < latest.size() ? latest[index] : 0;
        const int rhs = index < current.size() ? current[index] : 0;
        if (lhs != rhs)
            return lhs > rhs;
    }
    return false;
}

ReleaseChecker::~ReleaseChecker()
{
    cancel();
}

void ReleaseChecker::joinWorker()
{
    if (worker.joinable())
        worker.join();
}

void ReleaseChecker::clearStream(const std::shared_ptr<juce::WebInputStream>& stream)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (activeStream == stream)
        activeStream.reset();
}

void ReleaseChecker::cancel()
{
    std::shared_ptr<juce::WebInputStream> stream;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (stop != nullptr)
            stop->store(true, std::memory_order_release);
        stream = activeStream;
    }

    // cancel() unblocks connect() and read() on the worker. It does not join.
    if (stream != nullptr)
        stream->cancel();

    joinWorker();
}

void ReleaseChecker::check(Target target,
                           juce::Component::SafePointer<juce::Component> editor,
                           std::function<void(const ReleaseInfo&)> onResult)
{
    cancel();

    auto stopFlag = std::make_shared<std::atomic<bool>>(false);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stop = stopFlag;
    }

    try
    {
        worker = std::thread([this,
                              target = std::move(target),
                              editor,
                              onResult = std::move(onResult),
                              stopFlag]() mutable {
            try
            {
                juce::Thread::setCurrentThreadName("Release check");
                fetchAndPublish(std::move(target), editor, std::move(onResult), stopFlag);
            }
            catch (...)
            {
            }
        });
    }
    catch (...)
    {
    }
}

void ReleaseChecker::fetchAndPublish(Target target,
                                     juce::Component::SafePointer<juce::Component> editor,
                                     std::function<void(const ReleaseInfo&)> onResult,
                                     const std::shared_ptr<std::atomic<bool>>& stopFlag)
{
    if (stopFlag == nullptr || stopFlag->load(std::memory_order_acquire))
        return;
    if (!isGitHubToken(target.owner) || !isGitHubToken(target.repo))
        return;

    const auto address = "https://api.github.com/repos/" + target.owner + "/"
                         + target.repo + "/releases/latest";
    auto stream = std::make_shared<juce::WebInputStream>(juce::URL(address), false);
    const auto headers = "User-Agent: " + cleanUserAgent(target.userAgent) + "\r\n"
                         "Accept: application/vnd.github+json\r\n";
    stream->withExtraHeaders(headers)
        .withConnectionTimeout(kConnectionTimeoutMs)
        .withNumRedirectsToFollow(5)
        .withCustomRequestCommand("GET");

    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (stopFlag->load(std::memory_order_acquire))
            return;
        activeStream = stream;
    }

    const bool opened = stream->connect(nullptr);
    if (stopFlag->load(std::memory_order_acquire) || !opened || stream->isError())
    {
        clearStream(stream);
        return;
    }

    const int status = stream->getStatusCode();
    juce::MemoryOutputStream body;
    char buffer[2048];
    bool truncated = false;
    while (static_cast<int>(body.getDataSize()) < kMaxBodyBytes)
    {
        if (stopFlag->load(std::memory_order_acquire))
        {
            clearStream(stream);
            return;
        }

        const int room = kMaxBodyBytes - static_cast<int>(body.getDataSize());
        const int chunk = juce::jmin(static_cast<int>(sizeof(buffer)), room);
        const int got = stream->read(buffer, chunk);
        if (got <= 0)
            break;
        body.write(buffer, static_cast<size_t>(got));
        if (static_cast<int>(body.getDataSize()) >= kMaxBodyBytes)
            truncated = true;
    }
    clearStream(stream);

    if (truncated || stopFlag->load(std::memory_order_acquire) || status != 200)
        return;

    ReleaseInfo info;
    const auto parsed = juce::JSON::parse(body.toString());
    if (auto* object = parsed.getDynamicObject())
    {
        const auto tag = object->getProperty("tag_name").toString().trim();
        const auto html = object->getProperty("html_url").toString().trim();
        const auto releaseUrl = juce::URL(html);
        if (isNewerRelease(tag, target.currentVersion)
            && releaseUrl.getScheme().equalsIgnoreCase("https")
            && releaseUrl.getDomain().equalsIgnoreCase("github.com"))
        {
            info.tag = tag;
            info.htmlUrl = html;
            info.updateAvailable = true;
        }
    }

    if (!info.updateAvailable || stopFlag->load(std::memory_order_acquire))
        return;
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        return;

    // The callback runs later, possibly after this checker is gone. It keeps
    // its own stop flag and only touches the editor through the safe pointer.
    juce::MessageManager::callAsync(
        [stopFlag, editor, onResult = std::move(onResult), info]() {
            if (stopFlag->load(std::memory_order_acquire))
                return;
            if (editor == nullptr || onResult == nullptr)
                return;
            onResult(info);
        });
}
