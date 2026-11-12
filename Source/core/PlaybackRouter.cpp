#include "PlaybackRouter.h"

namespace pa
{

static constexpr int delayCapacity = 1 << 18;   // ~5.4 s at 48 kHz of latency alignment

void PlaybackRouter::prepare (double sampleRate, int maximumBlockSize)
{
    const juce::ScopedLock sl (lock);

    rate = sampleRate;
    maxBlock = juce::jmax (16, maximumBlockSize);
    work.setSize (2, maxBlock);
    scratch.setSize (32, maxBlock);
    delayLine.setSize (2, delayCapacity);
    delayLine.clear();
    delayWrite = 0;
    prepared = true;

    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < PluginChain::numSlots; ++s)
            if (auto* p = engine.getChain (c).slot (s).playProcessor.get())
                prepareProcessor (*p);
}

static void prepareTwin (juce::AudioProcessor& p, double rate, int maxBlock)
{
    p.releaseResources();
    configureStereoLayout (p);
    p.setRateAndBufferSizeDetails (rate, maxBlock);
    p.setProcessingPrecision (juce::AudioProcessor::singlePrecision);
    p.prepareToPlay (rate, maxBlock);
}

void PlaybackRouter::prepareProcessor (juce::AudioProcessor& p)
{
    if (! prepared)
        return;

    prepareTwin (p, rate, maxBlock);

    const int channels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), 2);
    if (channels > scratch.getNumChannels())
        scratch.setSize (channels, maxBlock);
}

int PlaybackRouter::getChainLatency (int chain) const
{
    int total = 0;
    for (int s = 0; s < PluginChain::numSlots; ++s)
    {
        auto& slot = engine.getChain (chain).slot (s);
        if (slot.playProcessor != nullptr && ! slot.bypassed)
            total += slot.playProcessor->getLatencySamples();
    }
    return total;
}

void PlaybackRouter::updateLatency()
{
    const int latency = juce::jlimit (0, delayCapacity / 2, juce::jmax (getChainLatency (0), getChainLatency (1)));

    if (reportedLatency.exchange (latency) != latency && onLatencyChanged)
        onLatencyChanged (latency);
}

void PlaybackRouter::process (juce::AudioBuffer<float>& buffer)
{
    const juce::ScopedTryLock tl (lock);
    if (! tl.isLocked() || ! prepared || buffer.getNumChannels() == 0)
        return;   // dry pass-through for this block

    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    const int total = buffer.getNumSamples();
    const int chain = playChain.load();
    const int target = reportedLatency.load();

    for (int pos = 0; pos < total; pos += maxBlock)
    {
        const int n = juce::jmin (maxBlock, total - pos);

        work.copyFrom (0, 0, buffer, 0, pos, n);
        work.copyFrom (1, 0, buffer, numChannels - 1, pos, n);

        int pathLatency = 0;
        if (chain >= 0)
        {
            for (int s = 0; s < PluginChain::numSlots; ++s)
            {
                auto& slot = engine.getChain (chain).slot (s);
                if (slot.playProcessor == nullptr || slot.bypassed)
                    continue;

                processInPlace (*slot.playProcessor, work, n, scratch, midi);
                pathLatency += slot.playProcessor->getLatencySamples();
            }
        }

        // Delay this path so every path has the same total (reported) latency.
        const int delay = juce::jlimit (0, delayCapacity - maxBlock - 1, target - pathLatency);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* line = delayLine.getWritePointer (ch);
            float* data = work.getWritePointer (ch);
            int w = delayWrite;

            for (int i = 0; i < n; ++i)
            {
                line[w] = data[i];
                data[i] = line[(w - delay + delayCapacity) & (delayCapacity - 1)];
                w = (w + 1) & (delayCapacity - 1);
            }
        }
        delayWrite = (delayWrite + n) & (delayCapacity - 1);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, pos, work, ch, 0, n);
    }
}

//==============================================================================
ParamMirror::ParamMirror (juce::AudioProcessor& src, juce::AudioProcessor& dst) : source (src), target (dst)
{
    for (auto* p : source.getParameters())
        p->addListener (this);
}

ParamMirror::~ParamMirror()
{
    for (auto* p : source.getParameters())
        p->removeListener (this);
}

void ParamMirror::parameterValueChanged (int index, float newValue)
{
    const auto& params = target.getParameters();
    if (juce::isPositiveAndBelow (index, params.size()))
        params[index]->setValue (newValue);
}

void copyProcessorState (juce::AudioProcessor& from, juce::AudioProcessor& to)
{
    juce::MemoryBlock state;
    from.getStateInformation (state);
    if (state.getSize() > 0)
        to.setStateInformation (state.getData(), (int) state.getSize());
}

} // namespace pa
