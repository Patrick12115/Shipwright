#include "SohMenu.h"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/SkipGIAnimations.h"

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

static const std::unordered_map<int32_t, const char*> skipGetItemAnimationOptions = {
    { SGIA_DISABLED, "Disabled" },
    { SGIA_JUNK, "Junk Items" },
    { SGIA_ALL, "All but Ice Traps" },
    { SGIA_ADVANCED, "Advanced" },
};

void SohMenu::AddMenuRandomizer() {
    // Add Randomizer Menu
    AddMenuEntry("Randomizer", CVAR_SETTING("Menu.RandomizerSidebarSection"));

    // Seed Settings
    WidgetPath path = { "Randomizer", "Seed Settings", SECTION_COLUMN_1 };
    AddSidebarEntry("Randomizer", path.sidebarName, 1);
    AddWidget(path, "Popout Randomizer Settings Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("RandomizerSettings"))
        .WindowName("Randomizer Settings")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Randomizer Settings Window."));

    // Enhancements
    path.sidebarName = "Enhancements";
    AddSidebarEntry("Randomizer", path.sidebarName, 3);
    path.column = SECTION_COLUMN_1;
    AddWidget(path, "Randomizer Enhancements", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Rando-Relevant Navi Hints", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("RandoRelevantNavi"))
        .Options(CheckboxOptions()
                     .Tooltip("Replace Navi's overworld quest hints with rando-related gameplay hints.")
                     .DefaultValue(true));
    AddWidget(path, "Random Rupee Names", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("RandomizeRupeeNames"))
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("When obtaining Rupees, randomize what the Rupee is called in the textbox.")
                     .DefaultValue(true));
    AddWidget(path, "Use Custom Key Models", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("CustomKeyModels"))
        .Options(
            CheckboxOptions()
                .Tooltip("Use Custom graphics for Dungeon Keys, Big and Small, so that they can be easily told apart.")
                .DefaultValue(true));
    AddWidget(path, "Map & Compass Colors Match Dungeon", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("ColoredMapsAndCompasses"))
        .PreFunc([](WidgetInfo& info) {
            info.options->disabled = !(OTRGlobals::Instance->gRandoContext->GetOption(RSK_SHUFFLE_MAPANDCOMPASS)
                                           .IsNot(RO_DUNGEON_ITEM_LOC_STARTWITH) &&
                                       OTRGlobals::Instance->gRandoContext->GetOption(RSK_SHUFFLE_MAPANDCOMPASS)
                                           .IsNot(RO_DUNGEON_ITEM_LOC_VANILLA) &&
                                       OTRGlobals::Instance->gRandoContext->GetOption(RSK_SHUFFLE_MAPANDCOMPASS)
                                           .IsNot(RO_DUNGEON_ITEM_LOC_OWN_DUNGEON));
            info.options->disabledTooltip =
                "This setting is disabled because a savefile is loaded without the map & compass.\n"
                "Shuffle settings set to \"Any Dungeon\", \"Overworld\" or \"Anywhere\".";
        })
        .Options(
            CheckboxOptions()
                .Tooltip("Matches the color of maps & compasses to the dungeon they belong to. "
                         "This helps identify maps & compasses from afar and adds a little bit of flair.\n\nThis only "
                         "applies to seeds with maps & compasses shuffled to \"Any Dungeon\", \"Overworld\", or "
                         "\"Anywhere\".")
                .DefaultValue(true));
    AddWidget(path, "Quest Item Fanfares", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("QuestItemFanfares"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Play unique fanfares when obtaining quest items (medallions/stones/songs). Note that these "
            "fanfares can be longer than usual."));
    AddWidget(path, "Mysterious Shuffled Items", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("MysteriousShuffle"))
        .Options(CheckboxOptions().Tooltip(
            "Displays a \"Mystery Item\" model in place of any freestanding/GS/shop items that were shuffled, "
            "and replaces item names for them and scrubs and merchants, regardless of hint settings, "
            "so you never know what you're getting."));
    AddWidget(path, "Simpler Boss Soul Models", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("SimplerBossSoulModels"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "When shuffling boss souls, they'll appear as a simpler model instead of showing the boss' models."
            "This might make boss souls more distinguishable from a distance, and can help with performance."));
    AddWidget(path, "Signs Hint Entrances", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("EntrancesOnSigns"))
        .Options(CheckboxOptions().Tooltip("If enabled, signs near loading zones will tell you where they lead to."));
    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Skip Get Item Animations", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation"))
        .Options(ComboboxOptions().ComboMap(skipGetItemAnimationOptions).DefaultIndex(SGIA_JUNK));
    AddWidget(path, "Item Scale: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationScale"))
        .PreFunc([](WidgetInfo& info) {
            s32 setting = CVarGetInteger(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation"), SGIA_JUNK);

            // Only enable scale slider when *some* skipping mode is active (Junk / All / Advanced),
            // and keep it disabled when set to "None" if you have that mode.
            info.options->disabled = (setting == SGIA_DISABLED);
            info.options->disabledTooltip =
                "This slider only applies when using the \"Skip Get Item Animations\" option.";
        })
        .Options(FloatSliderOptions().Min(5.0f).Max(15.0f).Format("%.2f").DefaultValue(10.0f).Tooltip(
            "The size of the item when it is picked up."));

    AddWidget(path, "Advanced Skip GI Categories", WIDGET_SEPARATOR_TEXT).PreFunc([](WidgetInfo& info) {
        s32 setting = CVarGetInteger(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation"), SGIA_JUNK);
        // Only show the list when the main combo is set to Advanced
        info.isHidden = (setting != SGIA_ADVANCED);
    });

    AddWidget(path, "Advanced Skip GI Bulk Actions", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) {
            s32 setting = CVarGetInteger(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation"), SGIA_JUNK);
            info.isHidden = (setting != SGIA_ADVANCED);
        })
        .CustomFunction([](WidgetInfo& info) {
            // Split available width across two buttons, accounting for spacing
            const float avail = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float half = (avail - spacing) * 0.5f;

            if (UIWidgets::Button("Skip All", UIWidgets::ButtonOptions()
                                                  .Size(ImVec2(half, 0))
                                                  .Tooltip("Enable all Advanced Skip GI categories."))) {
                for (int i = 0; i < SKIP_GI_ADVANCED_CATEGORY_COUNT; ++i) {
                    CVarSetInteger(skipGIAdvancedCVarList[i], 1);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }

            ImGui::SameLine();

            if (UIWidgets::Button("Clear All", UIWidgets::ButtonOptions()
                                                   .Size(ImVec2(half, 0))
                                                   .Tooltip("Disable all Advanced Skip GI categories."))) {
                for (int i = 0; i < SKIP_GI_ADVANCED_CATEGORY_COUNT; ++i) {
                    CVarSetInteger(skipGIAdvancedCVarList[i], 0);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
        });

    AddWidget(path, "Advanced Skip GI Category Grid", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) {
            s32 setting = CVarGetInteger(CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation"), SGIA_JUNK);
            info.isHidden = (setting != SGIA_ADVANCED);
        })
        .CustomFunction([](WidgetInfo& info) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float colW = (avail - spacing) * 0.5f;

            if (ImGui::BeginTable("##SkipGIAdvancedTable", 2,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {

                ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthFixed, colW);
                ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, colW);

                for (int i = 0; i < SKIP_GI_ADVANCED_CATEGORY_COUNT; i += 2) {
                    ImGui::TableNextRow();

                    // Left column
                    ImGui::TableSetColumnIndex(0);
                    {
                        bool v = CVarGetInteger(skipGIAdvancedCVarList[i], 0) != 0;
                        if (ImGui::Checkbox(skipGIAdvancedNameList[i], &v)) {
                            CVarSetInteger(skipGIAdvancedCVarList[i], v ? 1 : 0);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }
                    }

                    // Right column (if present)
                    if (i + 1 < SKIP_GI_ADVANCED_CATEGORY_COUNT) {
                        ImGui::TableSetColumnIndex(1);
                        bool v = CVarGetInteger(skipGIAdvancedCVarList[i + 1], 0) != 0;
                        if (ImGui::Checkbox(skipGIAdvancedNameList[i + 1], &v)) {
                            CVarSetInteger(skipGIAdvancedCVarList[i + 1], v ? 1 : 0);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }
                    }
                }

                ImGui::EndTable();
            }
        });

    // Plandomizer
    path.sidebarName = "Plandomizer";
    AddSidebarEntry("Randomizer", path.sidebarName, 1);
    AddWidget(path, "Popout Plandomizer Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("PlandomizerEditor"))
        .RaceDisable(false)
        .WindowName("Plandomizer Editor")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Randomizer Settings Window."));

    // Item Tracker
    path.sidebarName = "Item Tracker";
    AddSidebarEntry("Randomizer", path.sidebarName, 1);

    AddWidget(path, "Item Tracker", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Item Tracker", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ItemTracker"))
        .RaceDisable(false)
        .WindowName("Item Tracker")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Item Tracker.").EmbedWindow(false));

    AddWidget(path, "Item Tracker Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Popout Item Tracker Settings", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ItemTrackerSettings"))
        .RaceDisable(false)
        .WindowName("Item Tracker Settings")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Item Tracker Settings Window."));

    // Entrance Tracker
    path.sidebarName = "Entrance Tracker";
    AddSidebarEntry("Randomizer", path.sidebarName, 1);

    AddWidget(path, "Entrance Tracker", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Entrance Tracker", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("EntranceTracker"))
        .RaceDisable(false)
        .WindowName("Entrance Tracker")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Entrance Tracker.").EmbedWindow(false));

    AddWidget(path, "Entrance Tracker Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Popout Entrance Tracker Settings", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("EntranceTrackerSettings"))
        .RaceDisable(false)
        .WindowName("Entrance Tracker Settings")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Entrance Tracker Settings Window."));

    // Check Tracker
    path.sidebarName = "Check Tracker";
    AddSidebarEntry("Randomizer", path.sidebarName, 1);

    AddWidget(path, "Check Tracker", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Check Tracker", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("CheckTracker"))
        .RaceDisable(false)
        .WindowName("Check Tracker")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Check Tracker.").EmbedWindow(false));

    AddWidget(path, "Check Tracker Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Popout Check Tracker Settings", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("CheckTrackerSettings"))
        .RaceDisable(false)
        .WindowName("Check Tracker Settings")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Check Tracker Settings Window."));
}

} // namespace SohGui
