/* sx_eds.c — ver sx_eds.h */
#include "sx_eds.h"
#include "sx_hal.h"
#include "sx_placa.h"
#include "sx_adquisicion.h"

static void apagar_todo(void)
{
    sx_hal_fases(0, 0.0f);
    sx_hal_gpio(SX_PIN_ON_OFF_FLYBACK, 0);
}

void sx_eds_iniciar(sx_eds_t *e, uint32_t ahora, uint32_t ciclos)
{
    e->est = SX_EDS_ARRANQUE;
    e->t0 = ahora;
    e->ciclos_obj = ciclos;
    e->rc = 0;
    e->hv_max = 0.0f; e->hv_min = 1e9f; e->i_max = 0.0f;
    sx_hal_gpio(SX_PIN_SECURE_MEASURE, 0);          /* 1. break-before-make */
}

void sx_eds_abortar(sx_eds_t *e, uint32_t ahora, int rc)
{
    if (e->est == SX_EDS_REPOSO || e->est == SX_EDS_FIN || e->est == SX_EDS_DESCARGA) return;
    apagar_todo();
    e->rc = rc;
    e->est = SX_EDS_DESCARGA;
    e->t0 = ahora;
}

static void vigilar(sx_eds_t *e, float hv, float i)
{
    if (hv > e->hv_max) e->hv_max = hv;
    if (hv < e->hv_min) e->hv_min = hv;
    if (i > e->i_max) e->i_max = i;
}

uint8_t sx_eds_tick(sx_eds_t *e, uint32_t ahora)
{
    uint32_t dt = ahora - e->t0;
    switch (e->est) {
    case SX_EDS_REPOSO:
    case SX_EDS_FIN:
        return 0;

    case SX_EDS_ARRANQUE:
        if (dt >= 10) {                                  /* PhotoMOS ya abierto */
            sx_hal_gpio(SX_PIN_ACTIVE_V, 1);
            sx_hal_gpio(SX_PIN_ACTIVE_C, 1);
            sx_hal_gpio(SX_PIN_ON_OFF_FLYBACK, 1);       /* 2. encender flyback */
            e->est = SX_EDS_SUBIDA; e->t0 = ahora;
        }
        return 1;

    case SX_EDS_SUBIDA:
        if (dt >= 50) {
            float hv = sx_leer_hv(), i = SX_TIENE_I_AT ? sx_leer_i_at() : 0.0f;
            vigilar(e, hv, i);
            if (hv < SX_EDS_FRAC_MIN * SX_EDS_V_OBJETIVO || (SX_TIENE_I_AT && i > SX_I_FUGA_MAX_MA)) {
                sx_eds_abortar(e, ahora, -1);            /* no sube o fuga: agua/sales */
                return 1;
            }
            sx_hal_fases(1, SX_EDS_HZ);                  /* 3. onda viajera */
            e->est = SX_EDS_CORRIENDO; e->t0 = ahora; e->t_vig = ahora;
        }
        return 1;

    case SX_EDS_CORRIENDO:
        if (ahora - e->t_vig >= 100) {
            e->t_vig = ahora;
            float hv = sx_leer_hv(), i = SX_TIENE_I_AT ? sx_leer_i_at() : 0.0f;
            vigilar(e, hv, i);
            if (hv < SX_EDS_FRAC_MIN * SX_EDS_V_OBJETIVO || (SX_TIENE_I_AT && i > SX_I_FUGA_MAX_MA)) {
                sx_eds_abortar(e, ahora, -2);
                return 1;
            }
        }
        if (sx_hal_fases_ciclos() >= e->ciclos_obj) {
            apagar_todo();
            e->est = SX_EDS_DESCARGA; e->t0 = ahora;
        }
        return 1;

    case SX_EDS_DESCARGA:
        /* 4. descarga: flyback apagado y fases en 0 -> Q1/Q3/Q5 conducen y llevan los
         *    electrodos a tierra; el riel de 1.2 kV se descarga por R36/R10/R13 y el divisor. */
        if (dt < 300) {
            sx_hal_gpio(SX_PIN_PHASE_A, !SX_FASE_ACTIVA);
            sx_hal_gpio(SX_PIN_PHASE_B, !SX_FASE_ACTIVA);
            sx_hal_gpio(SX_PIN_PHASE_C, !SX_FASE_ACTIVA);
            return 1;
        }
        {
            float hv = sx_leer_hv();
            if (hv <= SX_V_SEGURA || dt >= 2000) {
                if (hv > SX_V_SEGURA && e->rc == 0) e->rc = -3;
                sx_hal_gpio(SX_PIN_ACTIVE_V, 0);
                sx_hal_gpio(SX_PIN_ACTIVE_C, 0);
                e->est = SX_EDS_FIN;
                return 0;
            }
        }
        return 1;
    }
    return 0;
}
