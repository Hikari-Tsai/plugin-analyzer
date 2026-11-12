#include "PluginChain.h"

namespace pa
{

PluginChain::PluginChain()
{
    scratch.setSize (32, maxBlockSize);
}

void configureStereoLayout (juce::AudioProcessor& p)
{
    p.enableAllBuses();

    const auto current = p.getBusesLayout();

    auto tryLayout = [&] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        auto layout = current;
        if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0) = in;
        if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = out;
        return p.setBusesLayout (layout);
    };

    using CS = juce::AudioChannelSet;

    if (! tryLayout (CS::stereo(), CS::stereo()))
        if (! tryLayout (CS::mono(), CS::stereo()))
            tryLayout (CS::mono(), CS::mono());
}

void PluginChain::prepareProcessor (juce::AudioProcessor& p)
{
    p.releaseResources();
    configureStereoLayout (p);
    p.setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    p.setProcessingPrecision (juce::AudioProcessor::singlePrecision);
    p.prepareToPlay (sampleRate, maxBlockSize);

    const int channels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), 2);
    if (channels > scratch.getNumChannels())
        scratch.setSize (channels, maxBlockSize);
}

void PluginChain::setSlot (int index, std::unique_ptr<juce::AudioProcessor> processor,
                           const juce::String& name, std::unique_ptr<juce::XmlElement> source,
                           std::unique_ptr<juce::AudioProcessor> playProcessor)
{
    auto& s = slots[(size_t) index];

    if (s.processor != nullptr)
        s.processor->releaseResources();

    s.playProcessor = std::move (playProcessor);   // prepared separately, at the DAW's rate
    s.processor = std::move (processor);
    s.name = name;
    s.source = std::move (source);
    s.bypassed = false;

    if (s.processor != nullptr)
        prepareProcessor (*s.processor);
}

void PluginChain::prepare (double newRate)
{
    sampleRate = newRate;

    for (auto& s : slots)
        if (s.processor != nullptr)
            prepareProcessor (*s.processor);
}

bool PluginChain::hasAnyProcessor() const
{
    for (auto& s : slots)
        if (s.processor != nullptr)
            return true;
    return false;
}

int PluginChain::getLatencySamples() const
{
    int total = 0;
    for (auto& s : slots)
        if (s.processor != nullptr && ! s.bypassed)
            total += s.processor->getLatencySamples();
    return total;
}

void PluginChain::reset()
{
    for (auto& s : slots)
        if (s.processor != nullptr)
            s.processor->reset();
}

void processInPlace (juce::AudioProcessor& p, juce::AudioBuffer<float>& io, int numSamples,
                     juce::AudioBuffer<float>& scratch, juce::MidiBuffer& midi)
{
    const juce::ScopedLock sl (p.getCallbackLock());

    const int inCh = p.getMainBusNumInputChannels();
    const int outCh = p.getMainBusNumOutputChannels();
    const int total = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), 2);

    jassert (total <= scratch.getNumChannels() && numSamples <= scratch.getNumSamples());
    juce::AudioBuffer<float> buffer (scratch.getArrayOfWritePointers(), juce::jmin (total, scratch.getNumChannels()), numSamples);
    buffer.clear();

    if (inCh >= 1) buffer.copyFrom (0, 0, io, 0, 0, numSamples);
    if (inCh >= 2) buffer.copyFrom (1, 0, io, 1, 0, numSamples);

    midi.clear();
    p.processBlock (buffer, midi);

    if (outCh >= 2)
    {
        io.copyFrom (0, 0, buffer, 0, 0, numSamples);
        io.copyFrom (1, 0, buffer, 1, 0, numSamples);
    }
    else if (outCh == 1)
    {
        io.copyFrom (0, 0, buffer, 0, 0, numSamples);
        io.copyFrom (1, 0, buffer, 0, 0, numSamples);
    }
}

void PluginChain::process (juce::AudioBuffer<float>& io, int numSamples)
{
    jassert (numSamples <= maxBlockSize);

    for (auto& s : slots)
        if (s.processor != nullptr && ! s.bypassed)
            processInPlace (*s.processor, io, numSamples, scratch, midi);
}

//==============================================================================
bool ChainRenderer::render (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int flushSamples)
{
    const int length = input.getNumSamples();
    const int latency = chain.getLatencySamples();
    output.setSize (2, length, false, false, true);
    output.clear();

    juce::AudioBuffer<float> block (2, blockSize);

    chain.reset();

    for (int pos = 0; pos < flushSamples; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, flushSamples - pos);
        block.clear();
        chain.process (block, n);

        if (shouldAbort())
            return false;
    }

    const int total = length + latency;

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        block.clear();

        const int available = juce::jlimit (0, n, length - pos);
        for (int ch = 0; ch < 2; ++ch)
            if (available > 0)
                block.copyFrom (ch, 0, input, juce::jmin (ch, input.getNumChannels() - 1), pos, available);

        chain.process (block, n);

        // Drop the first `latency` samples so output lines up with input.
        const int dstStart = pos - latency;
        const int skip = juce::jmax (0, -dstStart);
        const int count = juce::jmin (n - skip, length - (dstStart + skip));

        if (count > 0)
            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom (ch, dstStart + skip, block, ch, skip, count);

        if (shouldAbort())
            return false;
    }

    return true;
}

double ChainRenderer::timeBlocks (int size, int numBlocks, bool silenceAfterSignal)
{
    size = juce::jlimit (1, PluginChain::maxBlockSize, size);
    juce::AudioBuffer<float> block (2, size);
    juce::Random rng (1234);
    double seconds = 0.0;

    auto fillNoise = [&]
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < size; ++i)
                block.setSample (ch, i, 0.25f * (2.0f * rng.nextFloat() - 1.0f));
    };

    if (silenceAfterSignal)
    {
        // Untimed: excite the plugin for ~100 ms so filters/reverbs have tails to decay.
        const int preBlocks = juce::jmax (1, (int) (0.1 * chain.getSampleRate() / size));
        for (int b = 0; b < preBlocks; ++b)
        {
            fillNoise();
            chain.process (block, size);
        }
    }

    for (int b = 0; b < numBlocks; ++b)
    {
        if (silenceAfterSignal)
            block.clear();
        else
            fillNoise();

        const auto start = juce::Time::getHighResolutionTicks();
        chain.process (block, size);
        seconds += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);

        if (shouldAbort())
            return -1.0;
    }

    return seconds;
}

//==============================================================================
bool DifferenceRenderer::render (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int flushSamples)
{
    juce::AudioBuffer<float> other;

    if (! first.render (input, output, flushSamples) || ! second.render (input, other, flushSamples))
        return false;

    for (int ch = 0; ch < 2; ++ch)
        output.addFrom (ch, 0, other, ch, 0, output.getNumSamples(), -1.0f);

    return true;
}

} // namespace pa
