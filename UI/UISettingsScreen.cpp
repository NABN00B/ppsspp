// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "UI/UISettingsScreen.h"

#include "Common/Data/Text/I18n.h"
#include "UI/MiscViews.h"

UISettingsScreen::UISettingsScreen(const Path &gamePath)
	: UITabbedBaseDialogScreen(gamePath, nullptr, TabDialogFlags::AddAutoTitles) {
}

void UISettingsScreen::CreateTabs() {
	auto ui = GetI18NCategory(I18NCat::UISETTINGS);

	AddTab("GeneralUI", ui->T("General UI"), [this](UI::LinearLayout *parent) {
		CreateGeneralUISettings(parent);
	});

	AddTab("UISounds", ui->T("UI sounds"), [this](UI::LinearLayout *parent) {
		CreateUISoundsSettings(parent);
	});

	AddTab("Customization", ui->T("Customization"), [this](UI::LinearLayout *parent) {
		CreateCustomizationSettings(parent);
	});

	AddTab("Accessibility", ui->T("Accessibility"), [this](UI::LinearLayout *parent) {
		CreateAccessibilitySettings(parent);
	});
}

void UISettingsScreen::CreateGeneralUISettings(UI::ViewGroup *generalUISettings) {
	using namespace UI;

	auto ui = GetI18NCategory(I18NCat::UISETTINGS);
    auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);

     // Shared with achievements.
	static const char *positions[] = { "None", "Bottom Left", "Bottom Center", "Bottom Right", "Top Left", "Top Center", "Top Right", "Center Left", "Center Right" };
	generalUISettings->Add(new PopupMultiChoice(&g_Config.iNotificationPos, sy->T("Notification screen position"), positions, -1, ARRAY_SIZE(positions), I18NCat::DIALOG, screenManager()));

	generalUISettings->Add(new CheckBox(&g_Config.bShowSaveLoadIndicator, dev->T("Show indicator when saving/loading")));
}

void UISettingsScreen::CreateUISoundsSettings(UI::ViewGroup *uiSoundsSettings) {
	using namespace UI;

	auto ui = GetI18NCategory(I18NCat::UISETTINGS);
	auto a = GetI18NCategory(I18NCat::AUDIO);

	PopupSliderChoice *achievementVolume = uiSoundsSettings->Add(new PopupSliderChoice(&g_Config.iAchievementVolume, VOLUME_OFF, VOLUMEHI_FULL, Config::GetDefaultValueInt(&g_Config.iAchievementVolume), ac->T("Achievement sound volume"), screenManager()));
	achievementVolume->SetFormat("%d%%");
	achievementVolume->SetEnabledPtr(&g_Config.bEnableSound);
	achievementVolume->SetZeroLabel(a->T("Mute"));
	achievementVolume->OnChange.Add([](UI::EventParams &e) {
		// Audio preview
		float achievementVolume = Volume100ToMultiplier(g_Config.iAchievementVolume);
		g_BackgroundAudio.SFX().Play(UI::UISound::ACHIEVEMENT_UNLOCKED, achievementVolume);
	});

	uiSoundsSettings->Add(new ItemHeader(a->T("UI sound")));

	uiSoundsSettings->Add(new CheckBox(&g_Config.bUISound, a->T("UI sound")));
	PopupSliderChoice *uiVolume = uiSoundsSettings->Add(new PopupSliderChoice(&g_Config.iUIVolume, 0, VOLUMEHI_FULL, Config::GetDefaultValueInt(&g_Config.iUIVolume), a->T("UI volume"), screenManager()));
	uiVolume->SetFormat("%d%%");
	uiVolume->SetZeroLabel(a->T("Mute"));
	uiVolume->SetLiveUpdate(true);
	uiVolume->OnChange.Add([](UI::EventParams &e) {
		static double lastTimePlayed = 0.0;
		double now = time_now_d();
		if (now - lastTimePlayed < 0.1) {
			return; // Don't play if we just played one, to avoid spamming when dragging.
		}
		lastTimePlayed = now;
		// Audio preview
		PlayUISound(UI::UISound::CONFIRM);
	});
	uiVolume->SetEnabledPtr(&g_Config.bUISound);

	PopupSliderChoice *gamePreviewVolume = uiSoundsSettings->Add(new PopupSliderChoice(&g_Config.iGamePreviewVolume, VOLUME_OFF, VOLUMEHI_FULL, Config::GetDefaultValueInt(&g_Config.iGamePreviewVolume), a->T("Game preview volume"), screenManager()));
	gamePreviewVolume->SetFormat("%d%%");
	gamePreviewVolume->SetZeroLabel(a->T("Mute"));
}

void UISettingsScreen::CreateCustomizationSettings(UI::ViewGroup *customizationSettings) {
	using namespace UI;

	auto ui = GetI18NCategory(I18NCat::UISETTINGS);
    auto th = GetI18NCategory(I18NCat::THEMES);
    auto sy = GetI18NCategory(I18NCat::SYSTEM);

    customizationSettings->Add(new ItemHeader(ui->T("Customization")));

	static const char *backgroundAnimations[] = { "No animation", "Floating symbols", "Recent games", "Waves", "Moving background", "Bouncing icon", "Colored floating symbols" };
	customizationSettings->Add(new PopupMultiChoice(&g_Config.iBackgroundAnimation, sy->T("UI background animation"), backgroundAnimations, 0, ARRAY_SIZE(backgroundAnimations), I18NCat::SYSTEM, screenManager()));

	PopupMultiChoiceDynamic *theme = customizationSettings->Add(new PopupMultiChoiceDynamic(&g_Config.sThemeName, sy->T("Theme"), GetThemeInfoNames(), I18NCat::THEMES, screenManager()));
	theme->OnChoice.Add([](EventParams &e) {
		UpdateTheme();
		// Reset the tint/saturation if the theme changed.
		if (e.b) {
			g_Config.fUITint = 0.0f;
			g_Config.fUISaturation = 1.0f;
		}
	});

	Draw::DrawContext *draw = screenManager()->getDrawContext();

	if (!draw->GetBugs().Has(Draw::Bugs::RASPBERRY_SHADER_COMP_HANG)) {
		// We use shaders without tint capability on hardware with this driver bug.
		PopupSliderChoiceFloat *tint = new PopupSliderChoiceFloat(&g_Config.fUITint, 0.0f, 1.0f, 0.0f, sy->T("Color tint"), 0.01f, screenManager());
		tint->SetHasDropShadow(false);
		tint->SetLiveUpdate(true);
		customizationSettings->Add(tint);
		PopupSliderChoiceFloat *saturation = new PopupSliderChoiceFloat(&g_Config.fUISaturation, 0.0f, 2.0f, 1.0f, sy->T("Color saturation"), 0.01f, screenManager());
		saturation->SetHasDropShadow(false);
		saturation->SetLiveUpdate(true);
		customizationSettings->Add(saturation);
	}
}

void UISettingsScreen::CreateAccessibilitySettings(UI::ViewGroup *accessibilitySettings) {
	using namespace UI;

	auto ui = GetI18NCategory(I18NCat::UISETTINGS);
}
