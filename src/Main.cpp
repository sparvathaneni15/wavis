#include "ui/MainComponent.h"

#include <memory>

class WavisApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Wavis"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        juce::StringArray tokens;
        tokens.addTokens(commandLine, true);

        juce::String demoDir;
        int seconds = 8;
        for (int i = 0; i < tokens.size(); ++i)
        {
            if (tokens[i] == "--demo-dir" && i + 1 < tokens.size())
                demoDir = tokens[++i];
            else if (tokens[i] == "--demo-seconds" && i + 1 < tokens.size())
                seconds = tokens[++i].getIntValue();
        }

        mainWindow = std::make_unique<MainWindow>(demoDir, seconds);
    }

    void shutdown() override { mainWindow.reset(); }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& demoDir, int seconds)
            : DocumentWindow("Wavis", juce::Colour(0xff101218), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setResizable(true, true);
            auto* content = new MainComponent();
            setContentOwned(content, true);
            centreWithSize(1200, 740);
            setVisible(true);
            content->grabKeyboardFocus();
            if (demoDir.isNotEmpty())
                content->startDemo(juce::File(demoDir), seconds);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(WavisApplication)
