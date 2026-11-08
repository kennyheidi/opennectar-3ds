#include "pc_vs_map.h"

#include "pc_vs_default_map.h" // generado por CMake desde maps/vs/impact_arena/map.json

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kDiskMap  = "maps/vs/impact_arena/map.json";
constexpr const char* kUserMaps = "user/maps/vs";

std::string sSelected; // "" = la arena de serie
std::string sFile;     // si no está vacío, un map.json por ruta (--vs-map)
int sSerial = 0;
PcVsMap sMap;
int sState = 0; // 0 sin leer, 1 listo, -1 error
std::string sError;
std::string sText; // el map.json del mapa activo
std::vector<PcVsMapEntry> sList;

bool readText(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

std::string userMapFile(const std::string& id) { return std::string(kUserMaps) + "/" + id + "/map.json"; }

// La arena de serie: la de disco si vale, si no la incluida.
bool loadBuiltIn(PcVsMap& out, std::string& error, std::string* source = nullptr)
{
	std::string text;
	if (readText(kDiskMap, text)) {
		if (nectar::parseMap(text, out, error)) {
			fprintf(stderr, "[VS map] %s: \"%s\"\n", kDiskMap, out.name.c_str());
			if (source) *source = text;
			return true;
		}
		fprintf(stderr, "[VS map] %s no es válido (%s); se usa el incluido\n", kDiskMap, error.c_str());
	}
	if (source) *source = kPcVsDefaultMap;
	return nectar::parseMap(kPcVsDefaultMap, out, error);
}

bool loadPath(const std::string& path, PcVsMap& out, std::string& error, std::string* source = nullptr)
{
	std::string text;
	if (!readText(path, text)) {
		error = "no se encuentra " + path;
		return false;
	}
	if (source) *source = text;
	return nectar::parseMap(text, out, error);
}

} // namespace

const std::vector<PcVsMapEntry>& pc_vs_map_list(bool rescan)
{
	if (!sList.empty() && !rescan) return sList;
	sList.clear();

	PcVsMapEntry builtIn;
	PcVsMap m;
	if (loadBuiltIn(m, builtIn.error)) {
		builtIn.name   = m.name;
		builtIn.author = m.author;
	} else {
		builtIn.name = "Impact Arena";
	}
	sList.push_back(builtIn);

	namespace fs = std::filesystem;
	std::error_code ec;
	std::vector<PcVsMapEntry> user;
	for (fs::directory_iterator it(kUserMaps, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_directory(ec)) continue;
		PcVsMapEntry e;
		e.id = it->path().filename().string();
		if (!fs::exists(userMapFile(e.id), ec)) continue;
		PcVsMap um;
		if (loadPath(userMapFile(e.id), um, e.error)) {
			e.name   = um.name;
			e.author = um.author;
		} else {
			e.name = e.id;
		}
		const fs::path thumb = it->path() / "thumb.png";
		if (fs::exists(thumb, ec)) e.thumb = thumb.string();
		user.push_back(e);
	}
	std::sort(user.begin(), user.end(), [](const PcVsMapEntry& a, const PcVsMapEntry& b) { return a.id < b.id; });
	sList.insert(sList.end(), user.begin(), user.end());
	return sList;
}

void pc_vs_map_select(const std::string& id)
{
	if (id == sSelected && sFile.empty() && sState > 0) return;
	sSelected = id;
	sFile.clear();
	sState = 0;
	sSerial++;
}

void pc_vs_map_select_file(const std::string& path)
{
	sSelected.clear();
	sFile  = path;
	sState = 0;
	sSerial++;
}

const std::string& pc_vs_map_selected() { return sSelected; }
int pc_vs_map_serial() { return sSerial; }
const std::string& pc_vs_map_error() { return sError; }
const std::string& pc_vs_map_source_text() { return sText; }

const PcVsMap* pc_vs_map()
{
	if (sState) return sState > 0 ? &sMap : nullptr;
	sError.clear();
	sText.clear();
	bool ok;
	if (!sFile.empty()) ok = loadPath(sFile, sMap, sError, &sText);
	else if (sSelected.empty()) ok = loadBuiltIn(sMap, sError, &sText);
	else ok = loadPath(userMapFile(sSelected), sMap, sError, &sText);
	if (!ok) sText.clear();
	if (!ok) {
		const std::string which = !sFile.empty() ? sFile : (sSelected.empty() ? "arena de serie" : sSelected);
		fprintf(stderr, "[VS map] %s: %s\n", which.c_str(), sError.c_str());
	}
	sState = ok ? 1 : -1;
	return ok ? &sMap : nullptr;
}
