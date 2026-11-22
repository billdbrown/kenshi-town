// Town Ledger: milestone 1 of kenshi-town.
//
// A read-only management panel. Press F8 in game to toggle a window listing
// every character in the player faction with what they are doing right now.
// Its real job is proving the toolchain: build, load, hook the main loop, draw
// MyGUI, read live game state. City-builder behaviour comes after this works.

#include <Debug.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
// KenshiLib 0.4.0 bug: AI.h forward-declares `class CharacterMessage` while
// Character.h defines it as an enum. Rename AI.h's copy for the include. Only
// AI::sendMessage uses it, and we never call that.
#define CharacterMessage CharacterMessage_AIForwardDecl
#include <kenshi/AI/AI.h>
#undef CharacterMessage
#include <kenshi/AI/AITaskSystem.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Delegate.h>

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <sstream>
#include <string>

namespace
{
    const int TOGGLE_KEY = VK_F8;
    const int REFRESH_EVERY_N_FRAMES = 30;

    MyGUI::Window* g_window = NULL;
    MyGUI::EditBox* g_text = NULL;
    bool g_keyWasDown = false;
    int g_frame = 0;

    bool kenshiHasFocus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    std::string currentActivity(Character* c)
    {
        if (c->isDead())
            return "dead";
        if (!c->ai)
            return "-";
        AITaskSytem* tasks = c->ai->getTaskSystem();
        if (!tasks)
            return "-";
        std::string goal = tasks->getCurrentGoalString();
        return goal.empty() ? "idle" : goal;
    }

    std::string buildLedger()
    {
        if (!ou || !ou->player)
            return "No game loaded.";

        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        std::ostringstream out;
        out << "Population: " << people.size() << "\n\n";
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            Character* c = people[i];
            if (!c)
                continue;
            out << c->displayName << "  -  " << currentActivity(c) << "\n";
        }
        return out.str();
    }

    void refresh()
    {
        if (g_text)
            g_text->setCaption(buildLedger());
    }

    void onRefreshClick(MyGUI::WidgetPtr)
    {
        refresh();
    }

    void createWindow()
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;

        g_window = gui->createWidgetReal<MyGUI::Window>(
            "Kenshi_WindowCX", 0.62f, 0.15f, 0.33f, 0.6f,
            MyGUI::Align::Default, "Window", "TownLedgerWindow");
        g_window->setCaption("Town Ledger");

        MyGUI::Widget* client = g_window->getClientWidget();
        g_text = client->createWidgetReal<MyGUI::EditBox>(
            "Kenshi_EditBox", 0.03f, 0.03f, 0.94f, 0.82f,
            MyGUI::Align::Stretch, "TownLedgerText");
        g_text->setEditMultiLine(true);
        g_text->setEditReadOnly(true);
        g_text->setEditWordWrap(true);
        g_text->setVisibleVScroll(true);

        MyGUI::Button* button = client->createWidgetReal<MyGUI::Button>(
            "Kenshi_Button1", 0.03f, 0.88f, 0.3f, 0.09f,
            MyGUI::Align::Left | MyGUI::Align::Bottom, "TownLedgerRefresh");
        button->setCaption("Refresh");
        button->eventMouseButtonClick += MyGUI::newDelegate(onRefreshClick);

        g_window->setVisible(false);
    }

    void toggle()
    {
        if (!g_window)
            createWindow();
        if (!g_window)
            return;
        bool show = !g_window->getVisible();
        g_window->setVisible(show);
        if (show)
            refresh();
    }

    void (*mainLoop_orig)(GameWorld*, float) = NULL;

    void mainLoop_hook(GameWorld* thisptr, float time)
    {
        mainLoop_orig(thisptr, time);

        bool down = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
        if (down && !g_keyWasDown && kenshiHasFocus())
            toggle();
        g_keyWasDown = down;

        if (g_window && g_window->getVisible() && ++g_frame % REFRESH_EVERY_N_FRAMES == 0)
            refresh();
    }
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("TownLedger: loading");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            mainLoop_hook, &mainLoop_orig))
        ErrorLog("TownLedger: could not hook the main loop");
    else
        DebugLog("TownLedger: ready, press F8 in game");
}
