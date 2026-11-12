#include "ChainPanel.h"
#include "PlotComponents.h"
#include "host/BuiltinProcessors.h"

namespace pa
{

using juce::String;

static int editorKey (int chain, int slot) { return chain * 16 + slot; }

//==============================================================================
class ChainPanel::EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow (juce::AudioProcessor& processor, const String& title, bool floatOnTop, std::function<void()> onCloseFn)
        : DocumentWindow (title, Theme::panel, DocumentWindow::closeButton), onClose (std::move (onCloseFn))
    {
        setUsingNativeTitleBar (true);
        // Inside a DAW the host's own plugin windows float above normal windows; keep ours visible.
        setAlwaysOnTop (floatOnTop);

        juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (processor);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        setTopLeftPosition (120 + juce::Random::getSystemRandom().nextInt (200),
                            140 + juce::Random::getSystemRandom().nextInt (120));
        setVisible (true);
    }

    ~EditorWindow() override { clearContentComponent(); }

    void closeButtonPressed() override
    {
        // Let the owner delete us outside this callback.
        juce::MessageManager::callAsync ([fn = onClose] { fn(); });
    }

private:
    std::function<void()> onClose;
};

//==============================================================================
SlotComponent::SlotComponent (ChainPanel& o, int chain, int slot)
    : owner (o), chainIndex (chain), slotIndex (slot)
{
    for (auto* b : { &editButton, &bypassButton, &removeButton })
    {
        addAndMakeVisible (b);
        b->setColour (juce::TextButton::buttonColourId, Theme::plotBg);
        b->setColour (juce::TextButton::textColourOffId, Theme::textDim);
        // Connected edges shrink the text indent so single letters fit in narrow buttons.
        b->setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    }

    editButton.setTooltip ("Show / hide the plugin window");
    bypassButton.setTooltip ("Bypass (mute) this slot");
    removeButton.setTooltip ("Remove");
    bypassButton.setClickingTogglesState (false);

    editButton.onClick = [this] { owner.toggleEditor (chainIndex, slotIndex); };
    bypassButton.onClick = [this] { owner.toggleBypass (chainIndex, slotIndex); };
    removeButton.onClick = [this] { owner.removeSlot (chainIndex, slotIndex); };
    refresh();
}

void SlotComponent::refresh()
{
    auto& s = owner.engine.getChain (chainIndex).slot (slotIndex);
    const bool loaded = s.processor != nullptr;

    editButton.setVisible (loaded);
    bypassButton.setVisible (loaded);
    removeButton.setVisible (loaded);

    bypassButton.setColour (juce::TextButton::buttonColourId, s.bypassed ? juce::Colour (0xffb23a48) : Theme::plotBg);
    bypassButton.setColour (juce::TextButton::textColourOffId, s.bypassed ? Theme::text : Theme::textDim);
    editButton.setColour (juce::TextButton::textColourOffId,
                          owner.isEditorOpen (chainIndex, slotIndex) ? Theme::accent : Theme::textDim);

    String tip = loaded ? s.name : String ("Click to load a plugin, or drop a plugin file here");
    if (loaded)
        tip << "\nLatency: " << s.processor->getLatencySamples() << " samples";
    setTooltip (tip);
    repaint();
}

void SlotComponent::paint (juce::Graphics& g)
{
    auto& s = owner.engine.getChain (chainIndex).slot (slotIndex);
    const bool loaded = s.processor != nullptr;
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (dragOver ? Theme::accent.withAlpha (0.25f) : (loaded ? Theme::plotBg : Theme::panel.brighter (0.03f)));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (loaded ? Theme::curveColour (chainIndex, 0).withAlpha (s.bypassed ? 0.3f : 0.8f) : Theme::grid);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    auto textArea = getLocalBounds().reduced (8, 2);
    if (loaded)
        textArea.removeFromRight (3 * 26 + 6);

    g.setFont (juce::FontOptions (12.5f));
    g.setColour (loaded ? (s.bypassed ? Theme::textDim : Theme::text) : Theme::textDim);
    g.drawText (loaded ? s.name : String ("+  slot ") + String (slotIndex + 1), textArea,
                juce::Justification::centredLeft, true);
}

void SlotComponent::resized()
{
    auto r = getLocalBounds().reduced (5, 6);
    removeButton.setBounds (r.removeFromRight (24));
    r.removeFromRight (2);
    bypassButton.setBounds (r.removeFromRight (24));
    r.removeFromRight (2);
    editButton.setBounds (r.removeFromRight (24));
}

void SlotComponent::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked())
    {
        auto& s = owner.engine.getChain (chainIndex).slot (slotIndex);
        if (s.processor != nullptr && ! e.mods.isPopupMenu())
            owner.toggleEditor (chainIndex, slotIndex);
        else
            owner.showLoadMenu (chainIndex, slotIndex, this);
    }
}

void SlotComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();

    if (! files.isEmpty() && ! owner.loadFile (chainIndex, slotIndex, juce::File (files[0])))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a plugin",
                                                "No VST3/AU plugin was found in:\n" + files[0]);
}

//==============================================================================
ChainPanel::ChainPanel (AnalyzerCore& c) : core (c), engine (c.engine)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        chainLabels[ch].setText ("Chain " + String (ch + 1), juce::dontSendNotification);
        chainLabels[ch].setColour (juce::Label::textColourId, Theme::curveColour (ch, 0));
        chainLabels[ch].setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (chainLabels[ch]);

        for (int s = 0; s < PluginChain::numSlots; ++s)
            addAndMakeVisible (slots.add (new SlotComponent (*this, ch, s)));
    }

    core.addListener (this);
    startTimerHz (4);
}

ChainPanel::~ChainPanel()
{
    core.removeListener (this);
    closeAllEditors();
}

void ChainPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
}

void ChainPanel::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    const int rowH = r.getHeight() / 2;

    for (int c = 0; c < 2; ++c)
    {
        auto row = r.removeFromTop (rowH);
        chainLabels[c].setBounds (row.removeFromLeft (70));
        const int w = row.getWidth() / PluginChain::numSlots;

        for (int s = 0; s < PluginChain::numSlots; ++s)
            slots[c * PluginChain::numSlots + s]->setBounds (row.removeFromLeft (w));
    }
}

void ChainPanel::timerCallback()
{
    for (auto* s : slots)
        s->refresh();
}

void ChainPanel::refreshAll()
{
    for (auto* s : slots)
        s->refresh();
}

void ChainPanel::slotAboutToChange (int chain, int slot)
{
    // The processor is about to be replaced or deleted: its editor must go first.
    editors.erase (editorKey (chain, slot));
}

bool ChainPanel::isEditorOpen (int chain, int slot) const
{
    return editors.find (editorKey (chain, slot)) != editors.end();
}

void ChainPanel::closeAllEditors()
{
    editors.clear();
}

void ChainPanel::toggleEditor (int chain, int slot)
{
    const int key = editorKey (chain, slot);

    if (auto it = editors.find (key); it != editors.end())
    {
        editors.erase (it);
        refreshAll();
        return;
    }

    auto& s = engine.getChain (chain).slot (slot);
    if (s.processor == nullptr)
        return;

    juce::Component::SafePointer<ChainPanel> safe (this);
    const bool floatOnTop = core.hostType == AnalyzerCore::HostType::Plugin;
    editors[key] = std::make_unique<EditorWindow> (*s.processor, "Chain " + String (chain + 1) + " / " + s.name, floatOnTop,
                                                   [safe, key]
                                                   {
                                                       if (safe != nullptr)
                                                       {
                                                           safe->editors.erase (key);
                                                           safe->refreshAll();
                                                       }
                                                   });
    refreshAll();
}

std::function<void()> ChainPanel::openEditorLater (int chain, int slot)
{
    juce::Component::SafePointer<ChainPanel> safe (this);
    return [safe, chain, slot]
    {
        if (safe != nullptr && ! safe->isEditorOpen (chain, slot))
            safe->toggleEditor (chain, slot);
    };
}

void ChainPanel::toggleBypass (int chain, int slot)
{
    core.setBypass (chain, slot, ! engine.getChain (chain).slot (slot).bypassed.load());
}

void ChainPanel::removeSlot (int chain, int slot)
{
    core.removeSlot (chain, slot);
}

bool ChainPanel::loadFile (int chain, int slot, const juce::File& file)
{
    return core.loadFile (chain, slot, file, openEditorLater (chain, slot));
}

void ChainPanel::browseForFile (int chain, int slot)
{
#if JUCE_MAC
    const juce::File start ("/Library/Audio/Plug-Ins");
#else
    const juce::File start ("C:\\Program Files\\Common Files\\VST3");
#endif

    chooser = std::make_unique<juce::FileChooser> ("Load a plugin", start, "*.vst3;*.component");
    // Files only: on macOS plugin bundles (.vst3/.component) are presented as files; on Windows
    // combining file and folder selection would switch to a folder-only picker.
    // (A Windows .vst3 bundle folder can still be loaded by dropping it onto a slot.)
    const int flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<ChainPanel> safe (this);
    chooser->launchAsync (flags, [safe, chain, slot] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        if (! safe->loadFile (chain, slot, fc.getResult()))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a plugin",
                                                    "No VST3/AU plugin was found in:\n" + fc.getResult().getFullPathName());
    });
}

void ChainPanel::showLoadMenu (int chain, int slot, juce::Component* target)
{
    enum { idBrowse = 100000, idRemove, idBuiltinBase = 100100 };

    juce::PopupMenu menu, builtins, plugins;
    const auto& list = getBuiltinList();
    for (size_t i = 0; i < list.size(); ++i)
        builtins.addItem (idBuiltinBase + (int) i, list[i].name);

    const auto types = core.knownPlugins.getTypes();
    juce::KnownPluginList::addToMenu (plugins, types, juce::KnownPluginList::sortByManufacturer);

    menu.addSectionHeader ("Chain " + String (chain + 1) + ", slot " + String (slot + 1));
    menu.addSubMenu ("Plugins", plugins, ! types.isEmpty());
    menu.addSubMenu ("Built-in test processors", builtins);
    menu.addItem (idBrowse, "Load plugin file...");

    if (engine.getChain (chain).slot (slot).processor != nullptr)
    {
        menu.addSeparator();
        menu.addItem (idRemove, "Remove");
    }

    if (types.isEmpty())
    {
        menu.addSeparator();
        menu.addItem (-1, "(No plugins scanned yet: use the Plugins... button)", false);
    }

    juce::Component::SafePointer<ChainPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [safe, chain, slot, types] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result == idBrowse)
                safe->browseForFile (chain, slot);
            else if (result == idRemove)
                safe->removeSlot (chain, slot);
            else if (result >= idBuiltinBase && result < idBuiltinBase + 100)
            {
                if (safe->core.loadBuiltin (chain, slot, getBuiltinList()[(size_t) (result - idBuiltinBase)].id))
                    safe->toggleEditor (chain, slot);
            }
            else
            {
                const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);
                if (index >= 0)
                    safe->core.loadDescription (chain, slot, types.getReference (index), nullptr,
                                                safe->openEditorLater (chain, slot));
            }
        });
}

} // namespace pa
