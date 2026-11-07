/* =========================================================================
 * sx_adquisicion.c — Sensores en unidades físicas (ventana segura con la AT apagada).
 * Basado en el sx_adquisicion.c del v2, adaptado a los pines de la PCB final.
 * ========================================================================= */
#include "sx_adquisicion.h"
#include "sx_hal.h"
#include "sx_placa.h"
#include <math.h>

static float a_volts(uint16_t c) { return (float)c * SX_VREF / SX_ADC_MAX; }

float sx_conv_hv(uint16_t c)     { return a_volts(c) * SX_K_MEASURE_V; }
float sx_conv_i_at(uint16_t c)   { return a_volts(c) * SX_K_MEASURE_C; }
float sx_conv_isc_ma(uint16_t c) { return a_volts(c) * SX_K_ISC_MA; }
float sx_conv_v12(uint16_t c)    { return a_volts(c) * SX_K_V12; }
float sx_conv_i12(uint16_t c)    { return a_volts(c) * SX_K_I12; }

float sx_conv_ntc(uint16_t c)
{
    float v = a_volts(c);
    /* NTC abierto o en corto: -40 °C, que la capa de seguridad marca como fuera de rango */
    if (v > SX_VREF - 0.02f || v < 0.02f) return -40.0f;
    float r = 10000.0f * v / (SX_VREF - v);
    float inv_t = 1.0f / 298.15f + logf(r / 10000.0f) / 3950.0f;
    return 1.0f / inv_t - 273.15f;
}

float sx_leer_hv(void)
{
    sx_hal_gpio(SX_PIN_ACTIVE_V, 1);
    sx_hal_delay_ms(2);                         /* establecimiento del canal [VERIFICAR] */
    return sx_conv_hv(sx_hal_adc(SX_ADC_HV_MON, 4));
}

float sx_leer_i_at(void)
{
    sx_hal_gpio(SX_PIN_ACTIVE_C, 1);
    sx_hal_delay_ms(2);
    return sx_conv_i_at(sx_hal_adc(SX_ADC_I_AT, 4));
}

int sx_adquirir(const sx_cfg_t *cfg, sx_lecturas_t *L, uint8_t hacer_prueba_carga)
{
    int err = 0;
    sx_hal_gpio(SX_PIN_ON_OFF_FLYBACK, 0);
    sx_hal_delay_ms(5);

    /* --- electrodos: AFE solo si la AT está descargada (break-before-make) --- */
    float hv = sx_leer_hv();
    sx_hal_gpio(SX_PIN_ACTIVE_V, 0);
    if (!cfg->tiene_afe) {
        L->c_nf = cfg->c_ref_nf;                /* neutro: SIN medición de agua en el vidrio */
        L->g_us = 0.0f;
    } else if (hv > SX_V_SEGURA) {
        L->c_nf = NAN; L->g_us = NAN;           /* AT aún cargada: NO conectar */
        err |= 1;
    } else {
        sx_hal_gpio(SX_PIN_SECURE_MEASURE, 1);
        sx_hal_delay_ms(2);
        if (sx_hal_afe(&L->c_nf, &L->g_us) != 0) { L->c_nf = NAN; L->g_us = NAN; err |= 2; }
        sx_hal_gpio(SX_PIN_SECURE_MEASURE, 0);
        sx_hal_delay_ms(2);
    }

    /* --- aire y vidrio --- */
    if (cfg->tiene_sht40) {
        if (sx_hal_sht40(&L->t_aire, &L->hr) != 0) { L->t_aire = NAN; L->hr = NAN; err |= 4; }
    } else {
        L->t_aire = NAN; L->hr = NAN;           /* lo completa el nodo con el AMBIENTE */
    }
    L->t_vidrio = cfg->tiene_ntc ? sx_conv_ntc(sx_hal_adc(SX_ADC_NTC, 16)) : NAN;

    /* --- suciedad --- */
    if (cfg->tiene_minic) {
        L->isc_s = sx_conv_isc_ma(sx_hal_adc(SX_ADC_ISC_S, 16));
        L->isc_l = sx_conv_isc_ma(sx_hal_adc(SX_ADC_ISC_L, 16));
    } else {
        L->isc_s = L->isc_l = 0.0f;             /* el estimador lo trata como noche */
    }

    /* --- conector (una vez al día, al mediodía) --- */
    L->prueba_valida = 0;
    L->v_reposo = L->v_carga = L->i_carga = 0.0f;
    if (hacer_prueba_carga && cfg->tiene_prueba) {
        L->v_reposo = sx_conv_v12(sx_hal_adc(SX_ADC_V12, 16));
        sx_hal_gpio(SX_PIN_CARGA_PRUEBA, 1);    /* 1 A durante ~20 ms */
        sx_hal_delay_ms(20);
        L->v_carga = sx_conv_v12(sx_hal_adc(SX_ADC_V12, 16));
        L->i_carga = sx_conv_i12(sx_hal_adc(SX_ADC_I12, 16));
        sx_hal_gpio(SX_PIN_CARGA_PRUEBA, 0);
        L->prueba_valida = 1;
    }
    L->lavado_realizado = 0;
    return err;
}
