#pragma once

// =============================================================================================== //
// kutaQ3 hook - dedicated configuration files (NOT the game-folder imgui.ini)
//
// Dear ImGui's recommended split:
//   1. Application / cheat settings belong in YOUR own file (typed, stable schema).
//   2. ImGui window layout belongs in ImGui's own serializer, but pointed at a file YOU own
//      via SaveIniSettingsToMemory() / LoadIniSettingsFromMemory() with io.IniFilename = NULL.
//      That is how you persist window Pos/Size/Collapsed, table widths, and (on the docking
//      branch) dock-space layouts without writing "imgui.ini" next to the game.
//
// This module writes two files next to the injected DLL:
//
//     <dll folder>\kutaQ3.cfg         cheat feature toggles (hand-editable INI)
//     <dll folder>\kutaQ3_imgui.ini   ImGui layout blob (Pos/Size/Collapsed/tables)
//
// The default CWD imgui.ini / imgui_log.txt stay disabled.
//
// This tree is ImGui 1.92.9b *master* (no IMGUI_HAS_DOCKING). Dock layouts cannot be stored
// until the docking branch is used; the same kutaQ3_imgui.ini path will then pick them up.
// =============================================================================================== //

namespace Config
{
	static const char kFileName[]      = "kutaQ3.cfg";
	static const char kImGuiFileName[] = "kutaQ3_imgui.ini";

	// Live cheat settings. These are the values the menu edits and the hooks read.
	struct Settings
	{
		bool chamsEnabled;
		int  chamsStyle;      // 0 = solid, 1 = wireframe
		int  chamsGlowStyle;   // 0 = original, 1 = bright, 2 = halo, 3 = neon
		bool logShaders;

		// optional one-shot fallback if an older kutaQ3.cfg had a [Menu] section
		// and kutaQ3_imgui.ini does not exist yet
		bool  hasLegacyMenuLayout;
		float menuX, menuY, menuW, menuH;
		bool  applyLegacyMenuLayout;
	};

	void ResetToDefaults(Settings& s);

	extern Settings g_Settings;

	// pointers are valid until the next GetDirectoryFile() call
	const char* Path();
	const char* ImGuiPath();

	bool FileExists();
	bool ImGuiFileExists();

	// write kutaQ3.cfg (+ kutaQ3_imgui.ini if an ImGui context is alive)
	bool Save();

	// read kutaQ3.cfg. ImGui layout is applied on the next frame (see FlushPendingImGuiLoad).
	bool Load();

	// ImGui layout - require a live ImGui context
	bool SaveImGuiLayout();
	bool LoadImGuiLayout();

	// call once per frame BEFORE ImGui::NewFrame()
	void FlushPendingImGuiLoad();

	// last Save()/Load() result, for the menu status line (never NULL)
	const char* LastStatus();
	bool        LastStatusOk();
}
