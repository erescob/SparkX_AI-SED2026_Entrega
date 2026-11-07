/* =========================================================================
 * sx_eds.h — Ciclo de limpieza EDS como máquina de estados (no bloquea: la red
 * sigue funcionando mientras la tarjeta limpia).
 *
 *   ARRANQUE  medición desconectada (SECURE_MEASURE=0) antes de la AT
 *   SUBIDA    flyback encendido; a los 50 ms la AT debe pasar del 70 % (y la corriente
 *             de AT baja, si SX_TIENE_I_AT): si no, agua/fuga -> rc = -1
 *   CORRIENDO 6 pasos trifásicos por timer; cada 100 ms se vigila AT y corriente
 *             (caída o fuga -> rc = -2)
 *   DESCARGA  flyback apagado y fases en 0 (electrodos a tierra por Q1/Q3/Q5);
 *             espera < 50 V (si no baja en 2 s -> rc = -3)
 * ========================================================================= */
#ifndef SX_EDS_H
#define SX_EDS_H
#include <stdint.h>

enum { SX_EDS_REPOSO = 0, SX_EDS_ARRANQUE, SX_EDS_SUBIDA, SX_EDS_CORRIENDO, SX_EDS_DESCARGA, SX_EDS_FIN };

typedef struct {
    uint8_t  est;
    uint32_t t0, t_vig;
    uint32_t ciclos_obj;
    int      rc;
    float    hv_max, hv_min, i_max;
} sx_eds_t;

void    sx_eds_iniciar(sx_eds_t *e, uint32_t ahora_ms, uint32_t ciclos);
void    sx_eds_abortar(sx_eds_t *e, uint32_t ahora_ms, int rc);
/* Devuelve 1 mientras está ocupado; al terminar queda en SX_EDS_FIN con e->rc. */
uint8_t sx_eds_tick(sx_eds_t *e, uint32_t ahora_ms);

#endif
