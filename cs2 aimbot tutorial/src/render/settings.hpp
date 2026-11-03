#pragma once

#include "imgui.h"

// shared cheat settings - toggled from the imgui menu, read by the aimbot each tick
struct CheatSettings
{
	bool espBoxes = true;
	bool aimEnabled = true;
	bool fovCircle = true;
	float aimFov = 150.f; // px radius around the crosshair

	bool cornerBoxes = false;  // corners-only box style
	float cornerLength = 10.f; // px, clamped to the box size when drawn

	bool skeletonEnabled = true; // bone lines over the box
	ImVec4 skeletonColor = ImVec4(1.f, 1.f, 1.f, 1.f);

	float aimSmooth = 1.f; // 1 = instant snap, higher = smoother approach

	// movement
	bool autoStopEnabled = false; // counter-strafe on movement key release
	int stopDurationMs = 150;     // how long the opposite keys stay pressed

	// chams via material hook (opt-in from the menu: remote material creation
	// has crashed the game once, so never auto-initialize)
	bool chamsEnabled = false;
	ImVec4 chamColor = ImVec4(1.f, 0.f, 1.f, 1.f);

	// aim activation
	int aimKey = VK_RBUTTON; // any VK, bindable from the menu
	bool aimToggle = false;  // false = hold the key, true = press to toggle

	// colors
	ImVec4 boxColor = ImVec4(0.f, 1.f, 0.47f, 1.f);     // normal enemy box
	ImVec4 lowHpColor = ImVec4(1.f, 0.27f, 0.27f, 1.f); // hp <= 35
	ImVec4 fovColor = ImVec4(1.f, 1.f, 1.f, 1.f);       // fov circle ring

	// properties
	int pollMs = 8; // reader thread interval, ms - lower = fresher boxes/aim
};

inline CheatSettings g_settings;
