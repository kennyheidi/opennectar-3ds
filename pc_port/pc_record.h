#ifndef PC_RECORD_H
#define PC_RECORD_H

/*
 * Atajos para grabar vídeo (tráiler). Van con Cheats > Debug Keys, como F5-F7,
 * y usan Ctrl+número para no pisar las teclas F, casi todas ya ocupadas:
 *
 *   Ctrl+1  ocultar / mostrar el HUD y el cursor
 *   Ctrl+2  gráficos originales / mejorados (sin tocar el .conf)
 *   Ctrl+3  luz de amanecer     Ctrl+4  mediodía     Ctrl+5  atardecer
 *   Ctrl+6  +100 Pikmin en el grupo del capitán (hasta el límite de Pikmin)
 *   Ctrl+7  guardar el encuadre del modo foto (F3) en record_camera.txt
 *   Ctrl+8  volver a ese encuadre, también tras reiniciar el juego
 *
 * La hora solo mueve el reloj visual (luz, sol, cielo): el día no avanza ni
 * se acaba por usarla.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Flanco de Ctrl+<digit> (1-9). Llamar una sola vez por fotograma y dígito.
int pc_record_ctrl_pressed(int digit);

int pc_record_hud_hidden(void);
void pc_record_toggle_hud(void);

/// Guarda / recupera la pose del modo foto. Solo con el modo foto activo.
void pc_record_save_camera(void);
void pc_record_load_camera(void);

#ifdef __cplusplus
}
#endif

#endif
