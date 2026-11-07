/* =========================================================================
 * sx_nodo.h — Aplicación de cada tarjeta (una por panel). La misma en todas;
 * la del ID 1 además hace de COORDINADOR del arreglo.
 *
 * Cada paso de 15 min:
 *   coordinador  lee SHT40 + NTC  ->  AMBIENTE(k, T aire, HR, T vidrio ref) a todos
 *   cada nodo    recibe AMBIENTE  ->  adquiere sus sensores  ->  sx_paso() (núcleo v2:
 *                seguridad, estimadores, IA de rocío, política, alertas)
 *                -> si la política pide limpiar: SOLICITUD al coordinador
 *                -> ESTADO al coordinador (telemetría)
 *   coordinador  reparte TURNOS (máx. N limpiezas a la vez, primero las de pre-rocío)
 *   nodo         TURNO -> ciclo EDS -> FIN(rc)
 *
 * Seguridad en red (además de la del v2):
 *   - el ambiente compartido CADUCA: con más de 2 pasos sin AMBIENTE las lecturas
 *     del aire quedan en NAN y la capa de seguridad del v2 bloquea la AT;
 *   - sin coordinador no hay turnos, así que no se limpia (SX_LIMPIEZA_SIN_COORD = 0);
 *   - un TURNO solo se ejecuta si es del paso actual y el permiso sigue vigente.
 * ========================================================================= */
#ifndef SX_NODO_H
#define SX_NODO_H
#include <stdint.h>
#include "sx_placa.h"
#include "sx_controlador.h"
#include "sx_adquisicion.h"
#include "sx_red.h"
#include "sx_eds.h"

/* banderas de hardware / estado que viajan en el ESTADO */
#define SX_F_SIN_SHT40   0x01u
#define SX_F_SIN_NTC     0x02u
#define SX_F_SIN_MINIC   0x04u
#define SX_F_SIN_AFE     0x08u
#define SX_F_SIN_PRUEBA  0x10u
#define SX_F_AMB_VIEJO   0x20u   /* ambiente del coordinador caducado            */
#define SX_F_EN_EDS      0x40u
#define SX_F_SIN_COORD   0x80u   /* no hay camino al coordinador                 */

/* bit extra de bloqueo (los del v2 son 0x01-0x20) */
#define SX_B_RED_SECO    0x40u   /* sin AFE: el vidrio aún no lleva 1 h claramente seco */
#define SX_MARGEN_SIN_AFE     4.0f
#define SX_HR_SIN_AFE         80.0f
#define SX_PASOS_SECO_SIN_AFE 4u

/* códigos de rc de una limpieza (además de los de sx_eds: 0, -1, -2, -3) */
#define SX_RC_RECHAZADO  (-8)    /* turno llegó pero ya no había permiso / no era del paso */
#define SX_RC_SIN_TURNO  (-9)    /* el turno no llegó a tiempo                    */

typedef struct { uint8_t id, prio; int32_t k; } sx_sol_t;
typedef struct { uint8_t id; uint32_t limite_ms; } sx_activo_t;
typedef struct {
    uint8_t visto, saltos;
    uint32_t t_ms;
    int32_t k;
    uint8_t bits, flags, alertas, hw, p_rocio_x250;
    float margen, suelto, costra, t_vidrio, dias_falla, horas;
    int8_t rc; uint16_t hv_max, limpiezas, abortadas, denegadas;
} sx_reg_t;

typedef struct {
    sx_cfg_t cfg;
    sx_red_t red;
    sx_params_t P;
    sx_estado_t E;
    sx_lecturas_t L;
    sx_salidas_t O;
    sx_eds_t eds;

    /* tiempo */
    int32_t  k;
    uint32_t t_paso_ms, t_ejecutar_ms, t_latido_ms;
    uint8_t  paso_pendiente, arrancado, k_oido;
    int32_t  k_oido_val;

    /* ambiente compartido */
    float    amb_ta, amb_hr, amb_tv;
    int32_t  amb_k;
    uint8_t  amb_ok, amb_recibido;

    /* estación de la planta (por el gateway, solo coordinador) */
    float    gw_ta, gw_hr, gw_tv;
    int32_t  gw_k;
    uint8_t  gw_ok;

    /* limpieza */
    uint8_t  quiere_limpiar, prio, eds_activo, ref_activo, lavado_pendiente;
    int32_t  k_solicitud;
    uint32_t t_solicitud_ms, t_reintento_ms, t_ref_ms;
    int8_t   rc_ultimo;
    uint16_t limpiezas, abortadas, denegadas;
    float    hv_ultimo, i_ultimo, hv_min_ultimo;
    int32_t  k_prerocio, dia_fijo;
    uint8_t  pasos_secos;
    /* salud del EDS (solo con MEASURE_V / MEASURE_C, que ya están en la PCB) */
    float    ibase[7], i_base;
    uint8_t  n_ibase, cnt_ialta, cnt_aborto, alerta_eds;
    uint8_t  hw;

    /* coordinador */
    sx_sol_t    cola[SX_MAX_NODOS]; uint8_t n_cola;
    sx_activo_t act[8];             uint8_t n_act;
    sx_reg_t    reg[SX_MAX_NODOS + 1];
    uint32_t    turnos_dados;

    /* consola / gateway */
    char     lin[80]; uint8_t n_lin;
} sx_nodo_t;

void sx_nodo_iniciar(sx_nodo_t *n);
void sx_nodo_ciclo(sx_nodo_t *n);      /* llamar continuamente desde el lazo principal */
uint32_t sx_eds_duracion_max_ms(void);

#endif
