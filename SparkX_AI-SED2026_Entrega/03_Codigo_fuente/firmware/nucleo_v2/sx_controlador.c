/* =========================================================================
 * sx_controlador.c — Implementación (espejo de python/controlador.py)
 * ========================================================================= */
#include "sx_controlador.h"
#include "sx_config.h"
#include "sx_modelo_rocio.h"
#include <math.h>
#include <string.h>


/* ------------------------------------------------------------------------- */
float sx_punto_rocio(float t, float hr)
{
    /* Magnus en doble precisión y redondeo final a float: así el STM32 y la PC
     * coinciden aunque sus logf() difieran en el último bit. Corre 1 vez cada 15 min. */
    if (hr < 0.1f) hr = 0.1f;
    if (hr > 100.0f) hr = 100.0f;
    double a = 17.62, b = 243.12, td = (double)t, h = (double)hr;
    double g = log(h / 100.0) + a * td / (b + td);
    return (float)(b * g / (a - g));
}

float sx_rocio_prob(const float *x)
{
    int16_t n = 0;
    while (SX_ARBOL_FEAT[n] >= 0)
        n = (x[SX_ARBOL_FEAT[n]] <= SX_ARBOL_THR[n]) ? SX_ARBOL_IZQ[n] : SX_ARBOL_DER[n];
    return SX_ARBOL_PROB[n];
}

static void ordenar(float *v, int n)          /* inserción: n <= 48 */
{
    for (int i = 1; i < n; i++) {
        float x = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

static float mediana(const float *src, int n)
{
    float v[SX_BUF_T];
    memcpy(v, src, (size_t)n * sizeof(float));
    ordenar(v, n);
    return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static void empujar(float *h, uint8_t *n, uint8_t max, float v)
{
    if (*n < max) { h[(*n)++] = v; return; }
    memmove(h, h + 1, (size_t)(max - 1) * sizeof(float));
    h[max - 1] = v;
}

static float rezago(const float *h, uint8_t n, int k)
{
    int i = (int)n - 1 - k;
    return h[i < 0 ? 0 : i];
}

/* ------------------------------------------------------------------------- */
void sx_params_defecto(sx_params_t *p)
{
    p->politica = SX_POL_IA;
    p->umbral_suelto = SX_PAR_UMBRAL_SUELTO;
    p->umbral_pre = SX_PAR_UMBRAL_PRE;
    p->margen_regla = SX_PAR_MARGEN_REGLA;
    p->umbral_costra = SX_PAR_UMBRAL_COSTRA;
    p->alfa_sr = SX_PAR_ALFA_SR;
    p->p_rocio = SX_PAR_P_ROCIO;
    p->hora_fija_ini = SX_PAR_HORA_FIJA_INI;
    p->hora_fija_fin = SX_PAR_HORA_FIJA_FIN;
    p->seguridad_v1 = 0;
}

void sx_iniciar(sx_estado_t *e, const sx_params_t *p, float c_ref_nf, float k_sr)
{
    memset(e, 0, sizeof(*e));
    e->P = *p;
    e->c_ref = c_ref_nf;
    e->k_sr = k_sr;
    e->ult_limpieza = -1000000; e->ult_ref = -1000000; e->dia_fijo = -1;
    e->sr_f = 1.0f;
    e->suelto_mediodia = -1.0f;
    e->dias_falla = 999.0f;
}

/* ------------------------------------------------------------------------- */
static uint8_t seguridad(sx_estado_t *e, const sx_lecturas_t *L, float margen)
{
    if (e->P.seguridad_v1)             /* réplica del v1, solo para comparar */
        return (L->t_vidrio <= 20.0f) ? SX_B_MARGEN : 0;

    uint8_t bits = 0;
    float ta = L->t_aire, hr = L->hr, tv = L->t_vidrio, c = L->c_nf, g = L->g_us;
    if (!isfinite(ta) || !isfinite(hr) || !isfinite(tv) || !isfinite(c) || !isfinite(g)) {
        e->falla_sensor = 1;
        return SX_B_SENSOR;
    }
    if (!((double)ta >= SX_T_AIRE_MIN && (double)ta <= SX_T_AIRE_MAX)) bits |= SX_B_SENSOR;
    if (!(hr >= 0.0f && hr <= 100.0f)) bits |= SX_B_SENSOR;
    if (!((double)tv >= SX_T_VIDRIO_MIN && (double)tv <= SX_T_VIDRIO_MAX)) bits |= SX_B_SENSOR;
    if ((double)fabsf(tv - ta) > SX_DIF_VIDRIO_AIRE_MAX) bits |= SX_B_SENSOR;
    float c_rel = c / e->c_ref;
    if (!((double)c_rel >= SX_C_REL_MIN && (double)c_rel <= SX_C_REL_MAX)) bits |= SX_B_SENSOR;
    if (g < 0.0f) bits |= SX_B_SENSOR;

    int igual_sht = e->prev_ok && ta == e->prev_ta && hr == e->prev_hr;
    int igual_tv = e->prev_ok && tv == e->prev_tv;
    e->cnt_sht = igual_sht ? (uint16_t)(e->cnt_sht + 1) : 0;
    e->cnt_tv = igual_tv ? (uint16_t)(e->cnt_tv + 1) : 0;
    e->prev_ta = ta; e->prev_hr = hr; e->prev_tv = tv; e->prev_ok = 1;
    if (e->cnt_sht >= SX_PASOS_CONGELADO || e->cnt_tv >= SX_PASOS_CONGELADO) bits |= SX_B_CONGELADO;
    if (bits & (SX_B_SENSOR | SX_B_CONGELADO)) {
        e->falla_sensor = 1;
        return bits;
    }
    if ((double)margen < SX_MARGEN_ROCIO_MIN) bits |= SX_B_MARGEN;
    if ((double)hr > SX_HR_MAX) bits |= SX_B_HR;
    if ((double)g > SX_G_FUGA_MAX) bits |= SX_B_FUGA;
    if ((double)(c_rel - 1.0f) > SX_DC_MAX) bits |= SX_B_CAP;
    return bits;
}

static void capacitancia(sx_estado_t *e, const sx_lecturas_t *L, int valido, int seco)
{
    /* filtro SOLO con vidrio seco; al secarse se reinicia sin retraso (no "recuerda" el agua) */
    float c_rel = L->c_nf / e->c_ref;
    if (!valido) return;
    if (!seco) { e->c_seco_prev = 0; return; }
    if (!e->c_ok || e->rebase || !e->c_seco_prev) {
        e->c_f = c_rel; e->c_ok = 1;
        if (e->rebase) { e->c_base = c_rel; e->c_base_ok = 1; e->extra_c = 0.0f; e->rebase = 0; }
    } else {
        e->c_f = e->c_f + SX_ALFA_C * (c_rel - e->c_f);
    }
    e->c_seco_prev = 1;
}

static void estimar_suciedad(sx_estado_t *e, const sx_lecturas_t *L, int seco)
{
    int valida = L->isc_l >= SX_ISC_MIN_VALIDA;
    if (valida) {
        float sr_m = (L->isc_s / L->isc_l) / e->k_sr;
        if (e->pend_residuo > 0) {
            float r = 1.0f - sr_m;
            if (r < 0.0f) r = 0.0f;
            if (r > 0.5f) r = 0.5f;
            e->costra = r;
            e->sr_f = sr_m;
            e->pend_residuo = 0;
        } else if (!e->sr_ini) {
            e->sr_f = sr_m;
        } else {
            e->sr_f = e->sr_f + e->P.alfa_sr * (sr_m - e->sr_f);
        }
        e->sr_ini = 1;
        if (e->c_ok) { e->c_base = e->c_f; e->c_base_ok = 1; }
        e->extra_c = 0.0f;
    } else {
        if (e->pend_residuo > 0) e->pend_residuo--;
        /* sin luz: polvo nuevo por aumento de capacitancia, solo con vidrio claramente seco */
        if (seco && e->c_base_ok && e->c_ok) {
            float dc = (e->c_f - e->c_base) - SX_BANDA_C;
            e->extra_c = (dc > 0.0f) ? (float)(1.0 - exp(-SX_K_C_PERDIDA * (double)dc)) : 0.0f;
        }
    }
    float perdida = (1.0f - e->sr_f) + e->extra_c;
    float pmax = perdida > 0.0f ? perdida : 0.0f;
    if (pmax < e->costra) e->costra = pmax;
    float s = perdida - e->costra;
    e->suelto = s > 0.0f ? s : 0.0f;
    if (e->costra >= e->P.umbral_costra) e->alerta_lavado = 1;
}

static void tasa(sx_estado_t *e, int32_t k)
{
    if (k % SX_PASOS_DIA == SX_PASO_PRUEBA) {
        if (e->suelto_mediodia >= 0.0f && !e->limpio_desde_mediodia) {
            float inc = e->suelto - e->suelto_mediodia;
            if (inc < 0.0f) inc = 0.0f;
            e->tasa_dia = 0.7f * e->tasa_dia + 0.3f * inc;
        }
        e->suelto_mediodia = e->suelto;
        e->limpio_desde_mediodia = 0;
    }
}

static void conector(sx_estado_t *e, const sx_lecturas_t *L)
{
    if (!L->prueba_valida || L->i_carga < 0.5f) return;
    float r = (L->v_reposo - L->v_carga) / L->i_carga;
    if (L->v_carga <= 11.1265f) e->alerta_v1_conector = 1;
    if (!e->r_base_ok) {
        e->r_cal[e->n_rcal++] = r;
        if (e->n_rcal >= SX_DIAS_CALIBRACION) {
            e->r_base = mediana(e->r_cal, e->n_rcal);
            e->r_base_ok = 1;
        }
        return;
    }
    float dr = r - e->r_base;
    empujar(e->r_hist, &e->n_rhist, SX_DIAS_TENDENCIA, dr);
    e->cnt_r = (dr >= SX_DELTA_R_ALERTA) ? (uint8_t)(e->cnt_r + 1) : 0;
    if (e->cnt_r >= 2) e->alerta_conector = 1;
    int k = e->n_rhist;
    if (k >= 4) {
        /* regresión lineal en float32, sumas secuenciales (igual que numpy con n < 8) */
        float sx = 0.0f, sy = 0.0f;
        for (int i = 0; i < k; i++) { sx += (float)i; sy += e->r_hist[i]; }
        float xm = sx / (float)k, ym = sy / (float)k;
        float num = 0.0f, den = 0.0f;
        for (int i = 0; i < k; i++) {
            float dx = (float)i - xm;
            num += dx * (e->r_hist[i] - ym);
            den += dx * dx;
        }
        float pend = num / den;
        float ult = ym + pend * ((float)(k - 1) - xm);
        if (pend > 1e-4f) {
            float d = (SX_DELTA_R_FALLA - ult) / pend;
            e->dias_falla = d > 0.0f ? d : 0.0f;
        } else {
            e->dias_falla = 999.0f;
        }
    }
}

static void termica(sx_estado_t *e, const sx_lecturas_t *L, int32_t k, int valido)
{
    float g_est = L->isc_l / SX_ISC_POR_WM2;
    if (valido && g_est >= SX_G_MIN_TERMICA && e->n_buf < SX_BUF_T) {
        float esperado = L->t_aire + g_est / SX_FAIMAN_U_TOTAL;
        e->buf_t[e->n_buf++] = L->t_vidrio - esperado;
    }
    if (k % SX_PASOS_DIA == SX_PASOS_DIA - 1) {
        if (e->n_buf >= 8) {
            float med = mediana(e->buf_t, e->n_buf);
            if (!e->off_ok) {
                e->cal_t[e->n_cal++] = med;
                if (e->n_cal >= SX_DIAS_CALIBRACION) {
                    e->off_t = mediana(e->cal_t, e->n_cal);
                    e->off_ok = 1;
                }
            } else {
                e->ultimo_residuo = med - e->off_t;
                /* ventana de 5 días en bits: alerta si >= 3 de los últimos 5 superan el umbral */
                uint8_t alto = (e->ultimo_residuo >= SX_RESIDUO_TERMICO) ? 1u : 0u;
                e->cnt_t = (uint8_t)(((e->cnt_t << 1) | alto) & 0x1Fu);
                int unos = 0;
                for (int b = 0; b < 5; b++) unos += (e->cnt_t >> b) & 1u;
                if (unos >= SX_DIAS_TERMICA) e->alerta_termica = 1;
            }
        }
        e->n_buf = 0;
    }
}

/* ------------------------------------------------------------------------- */
void sx_paso(sx_estado_t *e, int32_t k, const sx_lecturas_t *L, sx_salidas_t *o)
{
    if (L->lavado_realizado) { e->costra = 0.0f; e->sr_f = 1.0f; e->alerta_lavado = 0; }
    float hora = (float)((k % SX_PASOS_DIA) * SX_DT_MIN) / 60.0f;

    /* --- características (historia de 3 h) --- */
    float td = sx_punto_rocio(L->t_aire, L->hr);
    float m = L->t_vidrio - td;
    uint8_t n0 = e->n_hist, n1 = e->n_hist, n2 = e->n_hist;
    empujar(e->h_m, &n0, SX_HIST, m);
    empujar(e->h_hr, &n1, SX_HIST, L->hr);
    empujar(e->h_ta, &n2, SX_HIST, L->t_aire);
    e->n_hist = n0;
    int ih = (int)(k % SX_PASOS_DIA);
    float x[SX_N_CARACT] = {
        m, m - rezago(e->h_m, e->n_hist, 4), m - rezago(e->h_m, e->n_hist, 12),
        L->hr, L->hr - rezago(e->h_hr, e->n_hist, 4),
        L->t_aire, L->t_aire - rezago(e->h_ta, e->n_hist, 4),
        L->t_aire - L->t_vidrio, SX_SIN_HORA[ih], SX_COS_HORA[ih]
    };

    /* --- 1. seguridad --- */
    uint8_t bits = seguridad(e, L, m);
    uint8_t permiso = (bits == 0);
    int valido = !(bits & (SX_B_SENSOR | SX_B_CONGELADO));
    /* --- 3. IA --- */
    float p = valido ? sx_rocio_prob(x) : 0.0f;
    /* --- 2. estimadores y 5. alertas --- */
    int seco = (double)m >= SX_MARGEN_C_MIN && (double)L->g_us <= SX_G_C_MAX;
    capacitancia(e, L, valido, seco);
    estimar_suciedad(e, L, seco);
    tasa(e, k);
    conector(e, L);
    termica(e, L, k, valido);

    /* --- 4. política --- */
    int puede = permiso && (k - e->ult_limpieza) >= SX_PASOS_MIN_CICLO;
    float u = e->P.umbral_suelto, upre = e->P.umbral_pre;
    int limpiar = 0;
    switch (e->P.politica) {
    case SX_POL_NUNCA: limpiar = 0; break;
    case SX_POL_FIJO: {
        int32_t dia = k / SX_PASOS_DIA;
        limpiar = puede && (double)hora >= e->P.hora_fija_ini && (double)hora < e->P.hora_fija_fin && dia != e->dia_fijo;
        if (limpiar) e->dia_fijo = dia;
    } break;
    case SX_POL_UMBRAL: limpiar = puede && e->suelto >= u; break;
    case SX_POL_IA:
        limpiar = puede && (e->suelto >= u || ((double)p >= e->P.p_rocio && e->suelto >= upre)); break;
    case SX_POL_REGLA_ROCIO:
        limpiar = puede && (e->suelto >= u || (m <= e->P.margen_regla && e->suelto >= upre)); break;
    default: limpiar = puede; break;
    }
    if (limpiar) {
        e->ult_limpieza = k;
        e->pend_residuo = 2;
        e->limpio_desde_mediodia = 1;
        e->sr_f = 1.0f - (e->costra + e->suelto * 0.3f);
        e->suelto = e->suelto * 0.3f;
        e->extra_c = 0.0f;
        e->rebase = 1;
    }
    int limpiar_ref = permiso && (k - e->ult_ref) >= SX_PASOS_REF;
    if (limpiar_ref) e->ult_ref = k;

    float tasa_h = e->tasa_dia / 24.0f;
    float horas;
    if (tasa_h <= 1e-6f) horas = 999.0f;
    else { horas = (u - e->suelto) / tasa_h; if (horas < 0.0f) horas = 0.0f; }
    if (horas > 999.0f) horas = 999.0f;

    o->limpiar = (uint8_t)limpiar; o->limpiar_ref = (uint8_t)limpiar_ref;
    o->permiso = permiso; o->bits = bits; o->p_rocio = p; o->margen = m;
    o->suelto = e->suelto; o->costra = e->costra; o->horas_a_limpieza = horas;
    o->alerta_lavado = e->alerta_lavado; o->alerta_conector = e->alerta_conector;
    o->dias_falla_conector = e->dias_falla; o->alerta_termica = e->alerta_termica;
    o->falla_sensor = e->falla_sensor; o->alerta_v1_conector = e->alerta_v1_conector;
}
