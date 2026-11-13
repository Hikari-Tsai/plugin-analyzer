#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "LiveProtocol.h"

namespace pa
{

class AnalyzerCore;

/** Lets local tools (the MCP server) inspect and drive this running instance.
    Listens on 127.0.0.1 only, requires the token from the discovery file, and is only
    started when the user enables it in Settings. See LiveProtocol.h for the protocol. */
class LiveServer : private juce::Thread
{
public:
    explicit LiveServer (AnalyzerCore& core);
    ~LiveServer() override;

    bool start();
    int getPort() const { return port; }
    const juce::String& getInstanceId() const { return instanceId; }

    /** Track name reported by the DAW (plugin version); updates the discovery file. */
    void setTrackName (const juce::String& name);

private:
    void run() override;
    void serve (juce::StreamingSocket& client);
    juce::var handle (const juce::String& method, const juce::var& params);
    juce::var onMessageThread (std::function<juce::var()> fn, int timeoutMs = 10000);
    void writeDiscoveryFile();

    juce::var state();
    juce::var measure (const juce::var& params);
    juce::var slotParameters (const juce::var& params);
    juce::var setParameters (const juce::var& params);
    juce::var loadPlugin (const juce::var& params);
    juce::var removePlugin (const juce::var& params);
    juce::var setBypass (const juce::var& params);
    juce::var setPlay (const juce::var& params);
    juce::var snapshot();

    AnalyzerCore& core;
    std::unique_ptr<juce::StreamingSocket> listener;
    int port = 0;
    juce::String token, instanceId, trackName;
    juce::File discoveryFile;
    juce::CriticalSection fileLock;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace pa
