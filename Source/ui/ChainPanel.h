#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "core/AnalyzerCore.h"

namespace pa
{

class ChainPanel;

/** One processor slot: click to load, buttons for editor / bypass / remove. */
class SlotComponent : public juce::Component,
                      public juce::SettableTooltipClient,
                      public juce::FileDragAndDropTarget
{
public:
    SlotComponent (ChainPanel& owner, int chain, int slot);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

    void refresh();

private:
    ChainPanel& owner;
    const int chainIndex, slotIndex;
    juce::TextButton editButton { "E" }, bypassButton { "M" }, removeButton { "X" };
    bool dragOver = false;
};

class ChainPanel : public juce::Component,
                   private AnalyzerCore::Listener,
                   private juce::Timer
{
public:
    explicit ChainPanel (AnalyzerCore&);
    ~ChainPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Slot actions (forwarded to the core; the panel only owns the editor windows)
    void showLoadMenu (int chain, int slot, juce::Component* target);
    bool loadFile (int chain, int slot, const juce::File& file);
    void browseForFile (int chain, int slot);
    void removeSlot (int chain, int slot);
    void toggleBypass (int chain, int slot);
    void toggleEditor (int chain, int slot);
    bool isEditorOpen (int chain, int slot) const;
    void closeAllEditors();

    AnalyzerCore& core;
    AnalysisEngine& engine;

private:
    friend class SlotComponent;
    class EditorWindow;

    void slotAboutToChange (int chain, int slot) override;
    void chainsChanged() override { refreshAll(); }
    std::function<void()> openEditorLater (int chain, int slot);
    void refreshAll();
    void timerCallback() override;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::OwnedArray<SlotComponent> slots;
    std::map<int, std::unique_ptr<EditorWindow>> editors;   // key = chain * 16 + slot
    juce::Label chainLabels[2];
};

} // namespace pa
