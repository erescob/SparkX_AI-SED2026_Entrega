/* =========================================================================
 * sx_controlador.h — Firmware Spark-X v2: seguridad, estimadores, IA,
 *                     política de limpieza y alertas de mantenimiento.
 *
 * Se llama sx_paso() cada 15 minutos con las lecturas ya convertidas a
 * unidades físicas. Es espejo EXACTO de python/controlador.py: la prueba
 * software-in-the-loop (python/sil_validacion.py) lo verifica paso a paso.
 *
 * Sin memoria dinámica. Todo en float32 (FPU del Cortex-M4F).
 * Compilar con -ffp-contract=off para que el STM32 redondee igual que la PC.
 * ========================================================================= */
#ifndef SX_CONTROLADOR_H
#define SX_CONTROLADOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Políticas de limpieza (para comparar; en producción se usa SX_POL_IA) */
enum { SX_POL_NUNCA = 0, SX_POL_FIJO, SX_POL_UMBRAL, SX_POL_IA, SX_POL_REGLA_ROCIO, SX_POL_CADA_HORA };

/* Motivos de bloqueo de alta tensión (bits) */
#define SX_B_SENSOR     0x01u   /* lectura fuera de rango / no finita        */
#define SX_B_MARGEN     0x02u   /* vidrio a menos de 2 °C del punto de rocío  */
#define SX_B_HR         0x04u   /* humedad relativa > 85 %                    */
#define SX_B_FUGA       0x08u   /* conductancia de fuga alta (agua/sales)     */
#define SX_B_CAP        0x10u   /* capacitancia +8 % (película de agua)       */
#define SX_B_CONGELADO  0x20u   /* sensor con lectura idéntica 6 h            */

#define SX_HIST 13
#define SX_NCAL 7
#define SX_BUF_T 48

typedef struct {
    float t_aire;        /* °C   SHT40                                   */
    float hr;            /* %    SHT40                                   */
    float t_vidrio;      /* °C   NTC en el vidrio                        */
    float isc_s;         /* mA   mini-celda bajo el vidrio EDS (sucia)   */
    float isc_l;         /* mA   mini-celda de referencia (limpia)       */
    float c_nf;          /* nF   AFE: capacitancia entre fases (AT off)  */
    float g_us;          /* µS   AFE: conductancia de fuga (AT off)      */
    uint8_t prueba_valida;  /* 1 si en este paso se hizo la prueba de carga */
    float v_reposo;      /* V    riel 12 V sin carga                     */
    float v_carga;       /* V    riel 12 V con la carga de prueba de 1 A */
    float i_carga;       /* A    corriente medida por el shunt           */
    uint8_t lavado_realizado; /* 1 si la cuadrilla lavó (lo confirma el operador por RS-485) */
} sx_lecturas_t;

typedef struct {
    uint8_t limpiar;          /* ejecutar ciclo EDS trifásico ahora                 */
    uint8_t limpiar_ref;      /* ejecutar mini-EDS de la celda de referencia        */
    uint8_t permiso;          /* capa de seguridad permite AT                       */
    uint8_t bits;             /* motivos de bloqueo                                 */
    float p_rocio;            /* P(agua en el vidrio en las próximas 8 h)            */
    float margen;             /* T_vidrio - T_rocío [°C]                             */
    float suelto;             /* pérdida estimada por polvo suelto [fracción]       */
    float costra;             /* pérdida estimada por costra cementada [fracción]   */
    float horas_a_limpieza;   /* pronóstico                                         */
    uint8_t alerta_lavado;
    uint8_t alerta_conector;
    float dias_falla_conector;/* pronóstico                                         */
    uint8_t alerta_termica;
    uint8_t falla_sensor;
    uint8_t alerta_v1_conector;  /* regla del v1, solo para comparar                */
} sx_salidas_t;

typedef struct {
    uint8_t politica;
    float umbral_suelto, umbral_pre, margen_regla, umbral_costra, alfa_sr;
    double p_rocio;              /* comparado en doble precisión (igual que Python) */
    double hora_fija_ini, hora_fija_fin;
    uint8_t seguridad_v1;        /* SOLO para reproducir la falla del v1           */
} sx_params_t;

typedef struct {
    sx_params_t P;
    float c_ref, k_sr;
    /* historia para características */
    float h_m[SX_HIST], h_hr[SX_HIST], h_ta[SX_HIST];
    uint8_t n_hist;
    /* sensor congelado */
    float prev_ta, prev_hr, prev_tv;
    uint8_t prev_ok;
    uint16_t cnt_sht, cnt_tv;
    /* limpieza */
    int32_t ult_limpieza, ult_ref, dia_fijo;
    /* estimadores de suciedad */
    float sr_f; uint8_t sr_ini;
    float costra; uint8_t pend_residuo;
    float suelto, suelto_mediodia; uint8_t limpio_desde_mediodia;
    float tasa_dia;
    uint8_t alerta_lavado;
    /* conector */
    float r_cal[SX_NCAL]; uint8_t n_rcal; uint8_t r_base_ok; float r_base;
    float r_hist[SX_NCAL]; uint8_t n_rhist; uint8_t cnt_r;
    uint8_t alerta_conector, alerta_v1_conector; float dias_falla;
    /* térmica */
    float buf_t[SX_BUF_T]; uint8_t n_buf;
    float cal_t[SX_NCAL]; uint8_t n_cal; uint8_t off_ok; float off_t;
    uint8_t cnt_t, alerta_termica; float ultimo_residuo;
    uint8_t falla_sensor;
    /* polvo nocturno por capacitancia */
    float c_f, c_base, extra_c;
    uint8_t c_ok, c_base_ok, rebase, c_seco_prev;
} sx_estado_t;

/* Parámetros por defecto (los ajustados en simulación, ver sx_config.h) */
void sx_params_defecto(sx_params_t *p);
/* c_ref_nf y k_sr se miden en la instalación con el vidrio limpio y seco */
void sx_iniciar(sx_estado_t *e, const sx_params_t *p, float c_ref_nf, float k_sr);
/* k = número de paso de 15 min desde el arranque (k % 96 = hora del día) */
void sx_paso(sx_estado_t *e, int32_t k, const sx_lecturas_t *L, sx_salidas_t *out);

float sx_punto_rocio(float t, float hr);
float sx_rocio_prob(const float *x);

#ifdef __cplusplus
}
#endif
#endif
