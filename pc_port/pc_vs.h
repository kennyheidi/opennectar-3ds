#ifndef _PC_VS_H
#define _PC_VS_H

#include "types.h"

/*
 * Partida VS "Carrera de piezas": reglas, marcador, reloj, cuenta atrás y
 * cierre. El montaje del mapa está en GameCoreSection (pcVsSetupBases) y la
 * arena en mods/pc_vs_arena.
 */

/// Reglas de la partida (menú previo, se guardan en la configuración).
struct PcVsRules {
	f32 matchSeconds;    ///< duración
	bool rocketWin;      ///< el cohete se puede asediar y destruirlo gana
	f32 rocketHp;        ///< vida de cada cohete
	f32 bigPieceSeconds; ///< cuándo sale la gorda (0 = desde el inicio, <0 = nunca)
	int fieldLimit;      ///< Pikmin en el campo por jugador
	f32 pelletSeconds;   ///< cada cuánto reaparecen las pastillas (<=0 = nunca)
};
void pc_vs_set_rules(const PcVsRules& rules);
const PcVsRules& pc_vs_rules(void);

/// Asedio al cohete: radio, daño por Pikmin y reparación por pieza.
#define PC_VS_SIEGE_RADIUS     200.0f
#define PC_VS_SIEGE_DPS        0.125f /* con vida normal: 20 Pikmin lo tiran en 40 s */
#define PC_VS_REPAIR_PER_PIECE 10.0f

void pc_vs_match_reset(void);
/// Avanza el reloj (solo mientras se juega).
void pc_vs_match_update(f32 dt);
f32 pc_vs_match_elapsed(void);
f32 pc_vs_match_time_left(void);
bool pc_vs_match_over(void);

int pc_vs_score(int player);
void pc_vs_add_score(int player, int points);
/// Piezas entregadas por cada jugador (para el resumen final).
int pc_vs_pieces(int player);
/// Pikmin vivos de cada jugador (lo actualiza la partida cada fotograma).
void pc_vs_set_alive(int player, int count);
int pc_vs_alive(int player);
/// Cambia en cada partida nueva (para reiniciar estado de la interfaz).
int pc_vs_match_serial(void);

/// Puntos de cada pieza, por id de pastilla (0 si no es una pieza del VS).
void pc_vs_set_piece_points(u32 pelletId, int points);
int pc_vs_piece_points(u32 pelletId);

/// Eventos: cada uno devuelve true una sola vez cuando toca.
bool pc_vs_take_big_piece_event(void);
bool pc_vs_take_pellet_event(void);

f32 pc_vs_rocket_hp(int player);
/// Vida en porcentaje de la máxima.
int pc_vs_rocket_percent(int player);
void pc_vs_damage_rocket(int player, f32 amount, int attacker);
/// Daño a los dos cohetes en el mismo paso: si caen a la vez, empate.
void pc_vs_damage_rockets(f32 amount0, f32 amount1);
void pc_vs_repair_rocket(int player, f32 amount);
/// -1 sin decidir; 0/1 ganador; 2 empate.
int pc_vs_winner(void);
/// True si la partida acabó por cohete destruido.
bool pc_vs_won_by_rocket(void);

/// Cuenta atrás con el mundo en pausa. Se arma al montar el mapa y empieza
/// con el primer fotograma que se ve; va por tiempo real.
void pc_vs_countdown_arm(void);
/// -1 sin cuenta atrás (o ya acabó); 3, 2, 1; 0 = "START".
int pc_vs_countdown_phase(void);
/// True mientras el mundo tiene que seguir en pausa (antes del START).
bool pc_vs_countdown_holding(void);
/// Congela la cuenta atrás (menús del port abiertos encima).
void pc_vs_countdown_set_frozen(bool frozen);

/// Aviso grande en pantalla (texto y segundos que quedan de mostrarlo).
void pc_vs_announce(const char* text, f32 seconds);
/// No hay piezas de la nave suficientes en los datos del juego.
void pc_vs_set_missing_pieces(bool missing);
bool pc_vs_missing_pieces(void);
const char* pc_vs_announcement(void);

/// Salida de la partida pedida desde la pantalla final o la pausa.
enum { PC_VS_EXIT_NONE = 0, PC_VS_EXIT_REMATCH = 1, PC_VS_EXIT_TITLE = 2 };
void pc_vs_request_exit(int kind);
int pc_vs_take_exit_request(void);

/// Opción --vs-map del editor de mapas: el juego va directo a una partida VS
/// con ese map.json, sin título ni menús (J1 teclado + mando 1, J2 mando 2).
void pc_vs_direct_request(void);
bool pc_vs_direct_requested(void); ///< pedida y aún sin llegar al título
bool pc_vs_direct_take_title(void); ///< en el título: true una vez, para salir hacia el VS
bool pc_vs_direct_take_setup(void); ///< en CardSelect: true una vez, para saltar los menús

#if defined(PIKI_PC_PORT)
/*
 * Netplay (deterministic mode): the VS pause and the end screen are sim
 * state, decided on sim ticks, and the HUD only reads them. Outside
 * deterministic mode the HUD keeps deciding them as before.
 */
/// pc_settings_note_gameplay_frame: a world tick ran this tick.
void pc_vs_note_gameplay_tick(void);
/// End of a deterministic tick (PlugPikiApp::idle, before waitRetrace): the
/// pause during the countdown and after the match.
void pc_vs_det_tick_end(void);
/// The HUD's liveness in deterministic mode: a world tick this tick, or the VS pause.
bool pc_vs_det_live(void);
/// Deterministic mode: the end screen, 2.5 s of ticks after the match ended.
bool pc_vs_det_end_screen_shown(void);
/// End screen cursor: 0 rematch, 1 title.
int pc_vs_end_choice(void);
/// Deterministic mode, RunningModeState: both players' synced presses
/// (Controller::mInputPressed). On the end screen either player moves the
/// choice or confirms it, so every peer decides on the same tick.
void pc_vs_end_screen_sim_input(u32 pressedP1, u32 pressedP2);
/// Every VS global the sim reads (the state hash's mode column).
u64 pc_vs_state_hash(void);
/// Netplay: --vs-map boots an online session; each PC keeps its own device
/// assignment and the session's captains.
void pc_vs_direct_set_online(bool online);
bool pc_vs_direct_online(void);
/// A VS match left for the title (end screen or pause menu). Counts up; an
/// online VS session ends there.
void pc_vs_note_left_to_title(void);
int pc_vs_left_to_title(void);
/// Netplay test knob: the match length in seconds (0 = the rules').
void pc_vs_set_test_match_seconds(f32 seconds);
#endif

#endif
