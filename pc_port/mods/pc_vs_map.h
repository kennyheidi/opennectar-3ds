#ifndef _PC_VS_MAP_H
#define _PC_VS_MAP_H

#include "types.h"
#include "mods/pc_vs_arena.h"
#include "nectar_mapcore/map.h"

#include <string>
#include <vector>

/*
 * Mapas VS (map.json). El formato y su compilador están en
 * libs/nectar_mapcore, compartidos con el editor de mapas; aquí, qué mapa se
 * juega y la lista del menú.
 */
using PcVsMap = nectar::MapData;

/// Un mapa de la lista del selector. id vacío = la arena de serie.
struct PcVsMapEntry {
	std::string id;     ///< nombre de la carpeta en user/maps/vs/
	std::string name;   ///< del map.json (o el id si no se puede leer)
	std::string author;
	std::string thumb;  ///< ruta de thumb.png, vacía si no hay
	std::string error;  ///< por qué no se puede jugar; vacío si vale
};

/// La arena de serie y después user/maps/vs/<id>/map.json por orden
/// alfabético. Con rescan vuelve a recorrer la carpeta.
const std::vector<PcVsMapEntry>& pc_vs_map_list(bool rescan);

/// Elige el mapa de las próximas partidas (id de la lista).
void pc_vs_map_select(const std::string& id);
/// Elige un map.json cualquiera por su ruta (opción --vs-map del editor).
void pc_vs_map_select_file(const std::string& path);
const std::string& pc_vs_map_selected();
/// Cambia cada vez que se elige otro mapa (para tirar lo compilado).
int pc_vs_map_serial();

/// Mapa VS activo. La arena de serie es maps/vs/impact_arena/map.json en
/// disco o, si no está o no es válido, la copia incluida en el ejecutable;
/// un mapa de usuario es el suyo y nada más. nullptr si no se puede leer
/// (el motivo, en pc_vs_map_error).
const PcVsMap* pc_vs_map();
const std::string& pc_vs_map_error();
/// El map.json del que salió pc_vs_map() (vacío si no se pudo leer); netplay
/// lo manda al otro PC con la oferta.
const std::string& pc_vs_map_source_text();


#endif
