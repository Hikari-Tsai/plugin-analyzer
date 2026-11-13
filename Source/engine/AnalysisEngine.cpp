#include "AnalysisEngine.h"

namespace pa
{

AnalysisEngine::AnalysisEngine() : juce::Thread ("Analysis")
{
    for (auto& c : chains)
        c.prepare (settings.sampleRate);
    preparedRate = settings.sampleRate;
}

AnalysisEngine::~AnalysisEngine()
{
    stop();
}

void AnalysisEngine::start()
{
    startThread (juce::Thread::Priority::normal);
}

void AnalysisEngine::stop()
{
    signalThreadShouldExit();
    notify();
    stopThread (10000);
}

AnalysisSettings AnalysisEngine::getSettings() const
{
    const juce::ScopedLock sl (settingsLock);
    return settings;
}

void AnalysisEngine::updateSettings (const std::function<void (AnalysisSettings&)>& change)
{
    {
        const juce::ScopedLock sl (settingsLock);
        change (settings);
    }

    ++settingsVersion;
    notify();
}

std::shared_ptr<const AnalysisResult> AnalysisEngine::getLatestResult() const
{
    const juce::SpinLock::ScopedLockType sl (resultLock);
    return latest;
}

std::shared_ptr<const AnalysisResult> AnalysisEngine::measureOnce (AnalysisSettings s, int timeoutMs)
{
    const juce::ScopedLock serialise (oneAtATime);

    const auto current = getSettings();
    s.sampleRate = current.sampleRate;   // keep the plugins prepared as they are
    s.blockSize = current.blockSize;

    auto request = std::make_shared<OnceRequest>();
    request->settings = s;
    {
        const juce::ScopedLock sl (onceLock);
        pendingOnce = request;
        oncePending = true;
    }
    notify();

    const bool finished = request->done.wait (timeoutMs);
    {
        const juce::ScopedLock sl (onceLock);
        if (pendingOnce == request)
        {
            pendingOnce.reset();
            oncePending = false;
        }
    }

    return finished && request->ok ? request->result : nullptr;
}

bool AnalysisEngine::runPendingOnce()
{
    std::shared_ptr<OnceRequest> request;
    {
        const juce::ScopedLock sl (onceLock);
        request = std::move (pendingOnce);
        oncePending = false;
    }
    if (request == nullptr)
        return false;

    const auto& s = request->settings;
    auto result = std::make_shared<AnalysisResult>();
    result->mode = s.mode;
    result->sampleRate = s.sampleRate;
    result->midSide = s.midSide;

    auto shouldAbort = [this] { return threadShouldExit() || pendingEdits.load() > 0; };
    bool ok;
    {
        const juce::ScopedLock sl (chainLock);
        if (! juce::exactlyEqual (preparedRate, s.sampleRate))
        {
            for (auto& c : chains)
                c.prepare (s.sampleRate);
            preparedRate = s.sampleRate;
        }

        ChainRenderer first (chains[0], s.blockSize, shouldAbort);
        ChainRenderer second (chains[1], s.blockSize, shouldAbort);
        const bool hasSecond = chains[1].hasAnyProcessor();

        if (s.difference && hasSecond && s.mode != Mode::Performance)
        {
            DifferenceRenderer diff (first, second);
            ok = runMeasurement (s, diff, 2, *result);
        }
        else
        {
            ok = runMeasurement (s, first, 0, *result);
            if (ok && hasSecond)
                ok = runMeasurement (s, second, 1, *result);
        }
    }

    request->result = result;
    request->ok = ok;
    request->done.signal();
    return true;
}

void AnalysisEngine::run()
{
    while (! threadShouldExit())
    {
        while (pendingEdits.load() > 0 && ! threadShouldExit())
            juce::Thread::sleep (2);

        if (runPendingOnce())   // one-shot requests (e.g. from an MCP client) go first
            continue;

        if (paused.load())
        {
            wait (250);
            continue;
        }

        const auto s = getSettings();
        const auto version = settingsVersion.load();

        auto result = std::make_shared<AnalysisResult>();
        result->mode = s.mode;
        result->sampleRate = s.sampleRate;
        result->midSide = s.midSide;

        auto shouldAbort = [this, version]
        {
            return threadShouldExit() || pendingEdits.load() > 0 || settingsVersion.load() != version || oncePending.load();
        };

        bool ok = true;
        {
            const juce::ScopedLock sl (chainLock);

            if (! juce::exactlyEqual (preparedRate, s.sampleRate))
            {
                for (auto& c : chains)
                    c.prepare (s.sampleRate);
                preparedRate = s.sampleRate;
            }

            ChainRenderer first (chains[0], s.blockSize, shouldAbort);
            ChainRenderer second (chains[1], s.blockSize, shouldAbort);
            const bool hasSecond = chains[1].hasAnyProcessor();

            if (s.difference && hasSecond && s.mode != Mode::Performance)
            {
                DifferenceRenderer diff (first, second);
                ok = runMeasurement (s, diff, 2, *result);
            }
            else
            {
                ok = runMeasurement (s, first, 0, *result);
                if (ok && hasSecond)
                    ok = runMeasurement (s, second, 1, *result);
            }
        }

        if (ok && ! shouldAbort())
        {
            result->sequence = ++sequence;
            {
                const juce::SpinLock::ScopedLockType sl (resultLock);
                latest = result;
            }
            sendChangeMessage();
        }

        if (threadShouldExit())
            break;

        if (ok)
        {
            int pause = s.pauseMs;
            if (s.mode == Mode::Performance) pause = juce::jmax (pause, 500);
            if (s.mode == Mode::Specs) pause = juce::jmax (pause, 1000);
            if (s.mode == Mode::Reverb || (s.mode == Mode::Harmonic && s.harmonicMode == HarmonicMode::Sweep2D))
                pause = juce::jmax (pause, 150);
            wait (pause);
        }
    }
}

} // namespace pa
