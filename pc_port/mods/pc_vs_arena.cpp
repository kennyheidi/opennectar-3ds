#include "pc_vs_arena.h"
#include "pc_vs_map.h"

#include "MapMgr.h"
#include "Stream.h"
#include "Vector.h"
#include "system.h"

#include "nectar_mapcore/compile.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// La geometría, la colisión, los caminos y el .ini salen de
// libs/nectar_mapcore (el mismo compilador que usa el editor de mapas); aquí
// solo se le dan los archivos del juego y se sirven las rutas virtuales.

namespace {

PcVsReaction reaction(const nectar::MapReaction& r)
{
	return { r.kind, r.pellet, r.teki, r.boss, r.count, r.radius };
}

bool readGameFile(const std::string& path, std::vector<u8>& out)
{
	RandomAccessStream* s = gsys->openFile(path.c_str(), true);
	if (!s) {
		return false;
	}
	const int n = s->getPending();
	out.resize(n > 0 ? n : 0);
	if (n > 0) {
		s->read(out.data(), n);
	}
	s->close();
	return n > 0;
}

// Lo compilado del mapa activo (se tira al elegir otro).
std::vector<u8> sMod, sIni, sGen, sPlants;
int sBuiltSerial = -1;
std::string sBuildError;

bool build()
{
	const PcVsMap* map = pc_vs_map();
	if (!map) {
		sBuildError = pc_vs_map_error();
		return false;
	}
	nectar::CompiledMap out;
	if (!nectar::compileMap(*map, readGameFile, PC_VS_ARENA_MOD, out, sBuildError)) {
		fprintf(stderr, "[VS arena] %s\n", sBuildError.c_str());
		return false;
	}
	sMod.swap(out.mod);
	sIni.swap(out.ini);
	sGen.swap(out.genDefault);
	sPlants.swap(out.genPlants);
	fprintf(stderr, "[VS arena] built %s: %zu bytes, %zu verts, %zu coll tris, %zu route points, %zu props\n", PC_VS_ARENA_MOD, sMod.size(),
	        out.vertices, out.collisionTris, out.routePoints, out.props);
	return true;
}

void dropIfStale()
{
	if (sBuiltSerial != pc_vs_map_serial()) {
		sMod.clear();
		sIni.clear();
		sGen.clear();
		sPlants.clear();
		sBuiltSerial = pc_vs_map_serial();
	}
}

} // namespace

const char* pc_vs_arena_prepare(void)
{
	static std::string sWhy;
	dropIfStale();
	if (sMod.empty() && !build()) {
		sMod.clear();
		sIni.clear();
		sWhy = sBuildError;
		return sWhy.c_str();
	}
	return nullptr;
}

void pc_vs_arena_compiled(const u8** mod, size_t* modLen, const u8** ini, size_t* iniLen)
{
	*mod    = sMod.data();
	*modLen = sMod.size();
	*ini    = sIni.data();
	*iniLen = sIni.size();
}

RandomAccessStream* pc_vs_arena_open(const char* path)
{
	const bool isMod = strcmp(path, PC_VS_ARENA_MOD) == 0, isIni = strcmp(path, PC_VS_ARENA_STAGE) == 0;
	const bool isGen = strcmp(path, PC_VS_ARENA_GEN) == 0, isPlants = strcmp(path, PC_VS_ARENA_PLANTS) == 0;
	if (!isMod && !isIni && !isGen && !isPlants) return nullptr;
	dropIfStale();
	if (sMod.empty() && !build()) return nullptr;
	std::vector<u8>& data = isMod ? sMod : isIni ? sIni : isGen ? sGen : sPlants;
	if (data.empty()) return nullptr; // sin generadores: como si no hubiera archivo
	return new RamStream(data.data(), int(data.size()));
}

// Sin mapa (map.json roto y sin copia válida) la arena ni se abre; aquí
// solo se evita leer de un puntero nulo.
namespace {
const PcVsMap& activeMap()
{
	static const PcVsMap sEmpty {};
	const PcVsMap* m = pc_vs_map();
	return m ? *m : sEmpty;
}

// Con terreno editable, la altura del suelo; en los llanos, 0. Con un
// escenario del juego, la de su colisión.
f32 groundAt(f32 x, f32 z)
{
	if (!activeMap().course.empty() && mapMgr) return mapMgr->getMinY(x, z, true);
	return nectar::terrainHeight(activeMap(), x, z);
}
} // namespace

void pc_vs_arena_base(int player, Vector3f& pos, f32& faceDirection)
{
	// Dirección (sin, cos): 90° mira hacia +X.
	const nectar::MapSpot& s = activeMap().bases[player];
	pos.set(s.x, groundAt(s.x, s.z), s.z);
	faceDirection = s.face;
}

void pc_vs_arena_onion(int player, int color, Vector3f& pos)
{
	const nectar::MapSpot& s = activeMap().onions[player][color];
	pos.set(s.x, groundAt(s.x, s.z), s.z);
}

int pc_vs_arena_pellet_spots(PcVsPelletSpot* out, int max)
{
	int n = 0;
	for (const nectar::MapPellet& s : activeMap().pellets) {
		if (n < max) out[n++] = { s.x, s.z, s.id };
	}
	return n;
}

void pc_vs_arena_rocket(int player, Vector3f& pos, f32& faceDirection)
{
	const nectar::MapSpot& s = activeMap().rockets[player];
	pos.set(s.x, groundAt(s.x, s.z), s.z);
	faceDirection = s.face;
}

int pc_vs_arena_piece_spots(PcVsPieceSpot* out, int max)
{
	int n = 0;
	for (const nectar::MapPiece& s : activeMap().pieces) {
		if (n < max) out[n++] = { s.x, s.z, s.kind, s.part };
	}
	return n;
}

void pc_vs_arena_big_piece(Vector3f& pos)
{
	const nectar::MapSpot& s = activeMap().bigPiece;
	pos.set(s.x, groundAt(s.x, s.z), s.z);
}

int pc_vs_arena_guard_spots(PcVsGuardSpot* out, int max)
{
	int n = 0;
	for (const nectar::MapSpot& s : activeMap().guards) {
		if (n < max) out[n++] = { s.x, s.z, reaction(s.reaction) };
	}
	return n;
}

void pc_vs_arena_gate(int player, Vector3f& pos, f32& faceDirection)
{
	// El modelo es ancho en su X local: girado 90° tapa un hueco en Z.
	const nectar::MapSpot& s = activeMap().gates[player];
	pos.set(s.x, groundAt(s.x, s.z), s.z);
	faceDirection = s.face;
}

PcVsReaction pc_vs_arena_gate_reaction(int player) { return reaction(activeMap().gates[player].reaction); }

void pc_vs_arena_bomb_pile(int player, Vector3f& pos)
{
	const nectar::MapSpot& s = activeMap().bombPiles[player];
	pos.set(s.x, groundAt(s.x, s.z), s.z);
}

int pc_vs_arena_enemy_spots(PcVsEnemySpot* out, int max)
{
	int n = 0;
	for (const nectar::MapEnemy& e : activeMap().enemies) {
		if (n < max) out[n++] = { e.x, e.z, e.face, e.teki, e.boss, reaction(e.reaction) };
	}
	return n;
}

int pc_vs_arena_object_spots(PcVsObjectSpot* out, int max)
{
	int n = 0;
	for (const nectar::MapObject& o : activeMap().objects) {
		if (n < max) out[n++] = { o.x, o.z, o.face, o.push, o.kind, o.work, o.y, reaction(o.reaction) };
	}
	return n;
}
