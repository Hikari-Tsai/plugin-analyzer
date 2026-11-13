#pragma once

#include "host/PluginChain.h"
#include <juce_events/juce_events.h>

namespace pa
{

/** Runs measurements continuously on a background thread and publishes the latest result. */
class AnalysisEngine : private juce::Thread,
                       public juce::ChangeBroadcaster
{
public:
    AnalysisEngine();
    ~AnalysisEngine() override;

    void start();
    void stop();

    AnalysisSettings getSettings() const;
    void updateSettings (const std::function<void (AnalysisSettings&)>& change);

    std::shared_ptr<const AnalysisResult> getLatestResult() const;

    PluginChain& getChain (int index) { return chains[(size_t) index]; }

    /** Paused engines measure nothing (used when a plugin instance has no open editor). */
    void setPaused (bool shouldPause) { paused = shouldPause; notify(); }

    /** Runs one measurement with these settings on the analysis thread, between the
        continuous measurements (also while paused), and waits for it. The continuous
        settings (and so the UI) are not touched. Sample rate and block size are the
        engine's own. Returns nullptr on timeout or if it was aborted. Any thread. */
    std::shared_ptr<const AnalysisResult> measureOnce (AnalysisSettings settings, int timeoutMs);

    /** Hold this while changing a chain's contents from the message thread.
        It makes the analysis thread abandon its current measurement promptly. */
    class EditScope
    {
    public:
        explicit EditScope (AnalysisEngine& e) : engine (e)
        {
            ++engine.pendingEdits;
            engine.chainLock.enter();
        }

        ~EditScope()
        {
            engine.chainLock.exit();
            --engine.pendingEdits;
            engine.notify();
        }

    private:
        AnalysisEngine& engine;
        JUCE_DECLARE_NON_COPYABLE (EditScope)
    };

private:
    void run() override;

    mutable juce::CriticalSection settingsLock;
    AnalysisSettings settings;
    std::atomic<juce::uint64> settingsVersion { 0 };

    juce::CriticalSection chainLock;
    std::array<PluginChain, 2> chains;
    std::atomic<int> pendingEdits { 0 };
    std::atomic<bool> paused { false };

    struct OnceRequest
    {
        AnalysisSettings settings;
        std::shared_ptr<AnalysisResult> result;
        bool ok = false;
        juce::WaitableEvent done;
    };
    bool runPendingOnce();
    juce::CriticalSection onceLock, oneAtATime;
    std::shared_ptr<OnceRequest> pendingOnce;
    std::atomic<bool> oncePending { false };   // interrupts the continuous measurement
    double preparedRate = 0.0;

    mutable juce::SpinLock resultLock;
    std::shared_ptr<const AnalysisResult> latest;
    juce::uint64 sequence = 0;
};

} // namespace pa
