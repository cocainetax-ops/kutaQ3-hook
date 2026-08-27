#include "config.h"
#include "main.h"
#include "imgui/imgui.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdarg>
#include <fstream>
#include <string>

using namespace std;

// =============================================================================================== //

void Config::ResetToDefaults(Settings& s)
{
	s.chamsEnabled           = true;
	s.chamsStyle             = 0;
	s.logShaders             = true;
	s.hasLegacyMenuLayout    = false;
	s.menuX                  = 60.0f;
	s.menuY                  = 60.0f;
	s.menuW                  = 320.0f;
	s.menuH                  = 250.0f;
	s.applyLegacyMenuLayout  = false;
}

Config::Settings Config::g_Settings;

static char  s_status[260] = "";
static bool  s_statusOk    = true;
static bool  s_pendingImGuiLoad = false;

static void SetStatus(bool ok, const char* fmt, ...)
{
	s_statusOk = ok;
	va_list ap;
	va_start(ap, fmt);
	vsprintf_s(s_status, fmt, ap);
	va_end(ap);
}

const char* Config::LastStatus()
{
	return s_status[0] ? s_status : "";
}

bool Config::LastStatusOk()
{
	return s_statusOk;
}

const char* Config::Path()
{
	return GetDirectoryFile(const_cast<char*>(kFileName));
}

const char* Config::ImGuiPath()
{
	return GetDirectoryFile(const_cast<char*>(kImGuiFileName));
}

static bool PathExists(const char* path)
{
	DWORD attr = GetFileAttributesA(path);
	return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool Config::FileExists()
{
	char pathCopy[320];
	strcpy_s(pathCopy, Path());
	return PathExists(pathCopy);
}

bool Config::ImGuiFileExists()
{
	char pathCopy[320];
	strcpy_s(pathCopy, ImGuiPath());
	return PathExists(pathCopy);
}

static void CopyPath(char* dest, size_t destSize, const char* src)
{
	strncpy_s(dest, destSize, src, _TRUNCATE);
}

// =============================================================================================== //
// tiny INI helpers
// =============================================================================================== //

static void TrimInPlace(char* s)
{
	if (!s) return;
	char* start = s;
	while (*start && isspace((unsigned char)*start))
		++start;
	if (start != s)
		memmove(s, start, strlen(start) + 1);

	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
}

static bool EqualsNoCase(const char* a, const char* b)
{
	if (!a || !b) return false;
	while (*a && *b)
	{
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		++a;
		++b;
	}
	return *a == 0 && *b == 0;
}

static bool ParseBool(const char* v, bool& out)
{
	if (!v || !v[0]) return false;
	if (EqualsNoCase(v, "1") || EqualsNoCase(v, "true") || EqualsNoCase(v, "yes") || EqualsNoCase(v, "on"))
	{
		out = true;
		return true;
	}
	if (EqualsNoCase(v, "0") || EqualsNoCase(v, "false") || EqualsNoCase(v, "no") || EqualsNoCase(v, "off"))
	{
		out = false;
		return true;
	}
	return false;
}

static bool ParseInt(const char* v, int& out)
{
	if (!v || !v[0]) return false;
	if (EqualsNoCase(v, "solid"))     { out = 0; return true; }
	if (EqualsNoCase(v, "wireframe")) { out = 1; return true; }

	char* end = NULL;
	long n = strtol(v, &end, 10);
	if (end == v) return false;
	out = (int)n;
	return true;
}

static bool ParseFloat(const char* v, float& out)
{
	if (!v || !v[0]) return false;
	char* end = NULL;
	float f = (float)strtod(v, &end);
	if (end == v) return false;
	out = f;
	return true;
}

static void ApplyKey(Config::Settings& s, const char* section, const char* key, const char* value)
{
	if (EqualsNoCase(section, "Features") || EqualsNoCase(section, "kutaQ3") || section[0] == 0)
	{
		if (EqualsNoCase(key, "ChamsEnabled")) { ParseBool(value, s.chamsEnabled); return; }
		if (EqualsNoCase(key, "ChamsStyle"))
		{
			int style = s.chamsStyle;
			if (ParseInt(value, style))
			{
				if (style < 0) style = 0;
				if (style > 1) style = 1;
				s.chamsStyle = style;
			}
			return;
		}
		if (EqualsNoCase(key, "LogShaders")) { ParseBool(value, s.logShaders); return; }
	}

	// legacy [Menu] from the first save/load revision - only used if kutaQ3_imgui.ini is missing
	if (EqualsNoCase(section, "Menu"))
	{
		if (EqualsNoCase(key, "PosX"))  { if (ParseFloat(value, s.menuX)) s.hasLegacyMenuLayout = true; return; }
		if (EqualsNoCase(key, "PosY"))  { if (ParseFloat(value, s.menuY)) s.hasLegacyMenuLayout = true; return; }
		if (EqualsNoCase(key, "SizeW")) { if (ParseFloat(value, s.menuW)) s.hasLegacyMenuLayout = true; return; }
		if (EqualsNoCase(key, "SizeH")) { if (ParseFloat(value, s.menuH)) s.hasLegacyMenuLayout = true; return; }
	}
}

// =============================================================================================== //
// ImGui layout - official memory API, dedicated file next to the DLL
// =============================================================================================== //

bool Config::SaveImGuiLayout()
{
	if (ImGui::GetCurrentContext() == NULL)
		return false;

	char pathCopy[320];
	CopyPath(pathCopy, sizeof(pathCopy), ImGuiPath());

	size_t size = 0;
	const char* data = ImGui::SaveIniSettingsToMemory(&size);
	if (!data)
		return false;

	ofstream file(pathCopy, ios::out | ios::binary | ios::trunc);
	if (!file.is_open())
		return false;

	file.write(data, (std::streamsize)size);
	file.close();

	ImGui::GetIO().WantSaveIniSettings = false;
	return true;
}

bool Config::LoadImGuiLayout()
{
	if (ImGui::GetCurrentContext() == NULL)
	{
		s_pendingImGuiLoad = true;
		return false;
	}

	char pathCopy[320];
	CopyPath(pathCopy, sizeof(pathCopy), ImGuiPath());

	ifstream file(pathCopy, ios::in | ios::binary);
	if (!file.is_open())
		return false;

	file.seekg(0, ios::end);
	const std::streamoff len = file.tellg();
	file.seekg(0, ios::beg);
	if (len <= 0)
	{
		file.close();
		return false;
	}

	std::string buf;
	buf.resize((size_t)len);
	file.read(&buf[0], len);
	file.close();

	ImGui::LoadIniSettingsFromMemory(buf.c_str(), buf.size());
	s_pendingImGuiLoad = false;
	return true;
}

void Config::FlushPendingImGuiLoad()
{
	if (!s_pendingImGuiLoad)
		return;
	if (ImGui::GetCurrentContext() == NULL)
		return;
	LoadImGuiLayout();
}

// =============================================================================================== //

bool Config::Save()
{
	char pathCopy[320];
	CopyPath(pathCopy, sizeof(pathCopy), Path());

	ofstream file(pathCopy, ios::out | ios::trunc);
	if (!file.is_open())
	{
		SetStatus(false, "Save failed: could not write %s", kFileName);
		return false;
	}

	const Settings& s = g_Settings;
	file << "; kutaQ3 hook - dedicated cheat settings\n";
	file << "; Window layout lives in " << kImGuiFileName << " (ImGui serializer).\n";
	file << "; Neither file is the game-folder imgui.ini.\n";
	file << "\n";
	file << "[Features]\n";
	file << "ChamsEnabled=" << (s.chamsEnabled ? 1 : 0) << "\n";
	file << "ChamsStyle=" << s.chamsStyle << "          ; 0 = solid, 1 = wireframe\n";
	file << "LogShaders=" << (s.logShaders ? 1 : 0) << "\n";
	file.close();

	const bool imguiOk = SaveImGuiLayout();
	if (imguiOk)
		SetStatus(true, "Saved %s + %s", kFileName, kImGuiFileName);
	else
		SetStatus(true, "Saved %s", kFileName);
	return true;
}

bool Config::Load()
{
	char pathCopy[320];
	CopyPath(pathCopy, sizeof(pathCopy), Path());

	ifstream file(pathCopy, ios::in);
	if (!file.is_open())
	{
		SetStatus(false, "Load failed: %s not found", kFileName);
		return false;
	}

	Settings loaded = g_Settings;
	char section[64] = "";
	char line[512];

	while (file.getline(line, sizeof(line)))
	{
		TrimInPlace(line);
		if (line[0] == 0 || line[0] == ';' || line[0] == '#')
			continue;

		if (line[0] == '[')
		{
			char* end = strchr(line, ']');
			if (!end) continue;
			*end = 0;
			strncpy_s(section, line + 1, _TRUNCATE);
			TrimInPlace(section);
			continue;
		}

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char* key = line;
		char* value = eq + 1;
		TrimInPlace(key);
		TrimInPlace(value);

		char* comment = strchr(value, ';');
		if (comment)
		{
			*comment = 0;
			TrimInPlace(value);
		}

		if (key[0] == 0) continue;
		ApplyKey(loaded, section, key, value);
	}
	file.close();

	if (loaded.menuW < 80.0f) loaded.menuW = 80.0f;
	if (loaded.menuH < 80.0f) loaded.menuH = 80.0f;

	// prefer the dedicated ImGui layout file; fall back to the old [Menu] keys once
	if (ImGuiFileExists())
	{
		s_pendingImGuiLoad = true;
		if (ImGui::GetCurrentContext() != NULL)
			LoadImGuiLayout();
		loaded.applyLegacyMenuLayout = false;
	}
	else if (loaded.hasLegacyMenuLayout)
	{
		loaded.applyLegacyMenuLayout = true;
	}

	g_Settings = loaded;
	SetStatus(true, "Loaded %s", kFileName);
	return true;
}

namespace
{
	struct DefaultsInit
	{
		DefaultsInit() { Config::ResetToDefaults(Config::g_Settings); }
	};
	static DefaultsInit s_defaults;
}
