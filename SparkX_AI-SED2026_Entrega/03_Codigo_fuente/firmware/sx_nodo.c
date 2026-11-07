/* sx_nodo.c — ver sx_nodo.h */
#include "sx_nodo.h"
#include "sx_hal.h"
#include "sx_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* ------------------------------------------------------------------ util */
static void p16(uint8_t *b, int i, uint16_t v) { b[i] = (uint8_t)v; b[i + 1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *b, int i, uint32_t v) { p16(b, i, (uint16_t)v); p16(b, i + 2, (uint16_t)(v >> 16)); }
static uint16_t g16(const uint8_t *b, int i) { return (uint16_t)(b[i] | (b[i + 1] << 8)); }
static uint32_t g32(const uint8_t *b, int i) { return (uint32_t)g16(b, i) | ((uint32_t)g16(b, i + 2) << 16); }

static int16_t  f2i16(float v, float e) { if (!isfinite(v)) return INT16_MIN; float x = v * e; if (x > 32767.f) x = 32767.f; if (x < -32767.f) x = -32767.f; return (int16_t)lrintf(x); }
static uint16_t f2u16(float v, float e) { if (!isfinite(v) || v < 0) return 0xFFFFu; float x = v * e; if (x > 65534.f) x = 65534.f; return (uint16_t)lrintf(x); }
static float    i162f(int16_t v, float e) { return v == INT16_MIN ? NAN : (float)v / e; }
static float    u162f(uint16_t v, float e) { return v == 0xFFFFu ? NAN : (float)v / e; }

static void consola(const char *s) { sx_hal_gateway_tx(s); }

/* número para JSON: "null" si no es finito */
static const char *jn(char *b, float v, int dec)
{
    if (!isfinite(v)) { strcpy(b, "null"); return b; }
    snprintf(b, 16, dec == 1 ? "%.1f" : dec == 2 ? "%.2f" : "%.3f", (double)v);
    return b;
}

uint32_t sx_eds_duracion_max_ms(void)
{
    return 2500u + (uint32_t)((float)SX_EDS_CICLOS * 1000.0f / SX_EDS_HZ);
}

static uint32_t jitter_ms(uint8_t id)
{
    uint32_t paso = SX_PASO_MS / 200u;         /* 4.5 s con pasos de 15 min */
    if (paso > 2000u) paso = 2000u;
    if (paso < 20u) paso = 20u;
    return 30u + (uint32_t)(id % 16u) * paso;
}

static uint8_t soy_coord(const sx_nodo_t *n) { return n->cfg.id == SX_ID_COORD; }

/* ------------------------------------------------------------ config */
static uint32_t crc_cfg(const sx_cfg_t *c) { return sx_crc16((const uint8_t *)c, (uint16_t)offsetof(sx_cfg_t, crc)); }

static void cfg_defecto(sx_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magia = SX_CFG_MAGIA;
    c->id = SX_ID_DEFECTO;
    c->tiene_sht40 = SX_TIENE_SHT40;     /* sin SHT40 el coordinador usa la estación de la planta */
    c->tiene_ntc = SX_TIENE_NTC;
    c->tiene_minic = SX_TIENE_MINICELDAS;
    c->tiene_afe = SX_TIENE_AFE;
    c->tiene_prueba = SX_TIENE_PRUEBA_CARGA;
    c->max_simult = SX_MAX_SIMULT_DEF;
    c->c_ref_nf = 100.0f;
    c->k_sr = 1.0f;
}

static void cfg_guardar(sx_nodo_t *n)
{
    n->cfg.crc = crc_cfg(&n->cfg);
    sx_hal_cfg_escribir(&n->cfg, sizeof(n->cfg));
}

/* ------------------------------------------------------------ init */
void sx_nodo_iniciar(sx_nodo_t *n)
{
    memset(n, 0, sizeof(*n));
    sx_cfg_t c;
    if (sx_hal_cfg_leer(&c, sizeof(c)) == 0 && c.magia == SX_CFG_MAGIA && c.crc == crc_cfg(&c) && c.id != 0)
        n->cfg = c;
    else
        cfg_defecto(&n->cfg);

    sx_red_iniciar(&n->red, n->cfg.id);
    sx_params_defecto(&n->P);
    if (!n->cfg.tiene_minic) n->P.politica = SX_POL_NUNCA;  /* sin polvo medido: la política la lleva sx_nodo */
    sx_iniciar(&n->E, &n->P, n->cfg.c_ref_nf, n->cfg.k_sr);

    n->hw = (uint8_t)((n->cfg.tiene_sht40 ? 0 : SX_F_SIN_SHT40) | (n->cfg.tiene_ntc ? 0 : SX_F_SIN_NTC) |
                      (n->cfg.tiene_minic ? 0 : SX_F_SIN_MINIC) | (n->cfg.tiene_afe ? 0 : SX_F_SIN_AFE) |
                      (n->cfg.tiene_prueba ? 0 : SX_F_SIN_PRUEBA));
    n->amb_k = -1000;
    n->k_prerocio = -100000;
    n->dia_fijo = -1;
    n->k = (int32_t)SX_K0 - 1;
    n->t_paso_ms = sx_hal_ms();
    n->rc_ultimo = 0;

    char b[120];
    snprintf(b, sizeof b, "\r\n=== Spark-X red | nodo %u%s | paso %lu ms | HW sht%u ntc%u minic%u afe%u prueba%u ===\r\n",
             n->cfg.id, soy_coord(n) ? " (COORDINADOR)" : "", (unsigned long)SX_PASO_MS,
             n->cfg.tiene_sht40, n->cfg.tiene_ntc, n->cfg.tiene_minic, n->cfg.tiene_afe, n->cfg.tiene_prueba);
    consola(b);
}

/* ------------------------------------------------------------ envío de tramas */
static void enviar_ambiente(sx_nodo_t *n, float ta, float hr, float tv, uint8_t ok)
{
    uint8_t b[16];
    p32(b, 0, (uint32_t)n->k);
    p16(b, 4, (uint16_t)f2i16(ta, 100.f));
    p16(b, 6, f2u16(hr, 100.f));
    p16(b, 8, (uint16_t)f2i16(tv, 100.f));
    b[10] = ok;
    b[11] = n->cfg.max_simult;
    sx_red_enviar(&n->red, SX_T_AMBIENTE, SX_BCAST, b, 12);
    if (soy_coord(n)) {
        char s[100], a1[16], a2[16], a3[16];
        snprintf(s, sizeof s, "{\"t\":\"amb\",\"k\":%ld,\"ta\":%s,\"hr\":%s,\"tv\":%s,\"ok\":%u}\r\n",
                 (long)n->k, jn(a1, ta, 2), jn(a2, hr, 1), jn(a3, tv, 2), ok);
        consola(s);
    }
}

static uint8_t armar_estado(const sx_nodo_t *n, uint8_t *b)
{
    const sx_salidas_t *o = &n->O;
    p32(b, 0, (uint32_t)n->k);
    b[4] = o->bits;
    b[5] = (uint8_t)(o->permiso | (o->limpiar << 1) | (o->limpiar_ref << 2) | (n->eds_activo << 3));
    b[6] = (uint8_t)(o->alerta_lavado | (o->alerta_conector << 1) | (o->alerta_termica << 2) |
                     (o->falla_sensor << 3) | (o->alerta_v1_conector << 4) | (n->alerta_eds << 5));
    b[7] = n->hw;
    b[8] = (uint8_t)lrintf((o->p_rocio < 0 ? 0 : o->p_rocio > 1 ? 1 : o->p_rocio) * 250.f);
    p16(b, 9, (uint16_t)f2i16(o->margen, 100.f));
    p16(b, 11, f2u16(o->suelto, 10000.f));
    p16(b, 13, f2u16(o->costra, 10000.f));
    p16(b, 15, (uint16_t)f2i16(n->L.t_vidrio, 100.f));
    p16(b, 17, f2u16(o->dias_falla_conector, 10.f));
    p16(b, 19, f2u16(o->horas_a_limpieza, 10.f));
    b[21] = (uint8_t)n->rc_ultimo;
    p16(b, 22, f2u16(n->hv_ultimo, 1.f));
    p16(b, 24, n->limpiezas);
    p16(b, 26, n->abortadas);
    p16(b, 28, n->denegadas);
    b[30] = n->red.saltos;
    b[31] = n->red.padre;
    return 32;
}

/* ------------------------------------------------------------ registro del coordinador */
static const char *txt_alertas(uint8_t a, char *s)
{
    s[0] = 0;
    if (a & 1) strcat(s, "PEDIR_LAVADO ");
    if (a & 2) strcat(s, "REVISAR_CONECTOR ");
    if (a & 4) strcat(s, "ANOMALIA_TERMICA ");
    if (a & 8) strcat(s, "FALLA_SENSOR ");
    if (a & 32) strcat(s, "REVISAR_EDS ");
    size_t l = strlen(s);
    if (l) s[l - 1] = 0;
    return s;
}

static void registrar_estado(sx_nodo_t *n, uint8_t id, const uint8_t *b, uint8_t saltos)
{
    if (id == 0 || id > SX_MAX_NODOS) return;
    sx_reg_t *r = &n->reg[id];
    r->visto = 1; r->t_ms = sx_hal_ms(); r->saltos = saltos;
    r->k = (int32_t)g32(b, 0); r->bits = b[4]; r->flags = b[5]; r->alertas = b[6]; r->hw = b[7];
    r->p_rocio_x250 = b[8];
    r->margen = i162f((int16_t)g16(b, 9), 100.f);
    r->suelto = u162f(g16(b, 11), 10000.f);
    r->costra = u162f(g16(b, 13), 10000.f);
    r->t_vidrio = i162f((int16_t)g16(b, 15), 100.f);
    r->dias_falla = u162f(g16(b, 17), 10.f);
    r->horas = u162f(g16(b, 19), 10.f);
    r->rc = (int8_t)b[21]; r->hv_max = g16(b, 22);
    r->limpiezas = g16(b, 24); r->abortadas = g16(b, 26); r->denegadas = g16(b, 28);
    char al[80], s[360], a1[16], a2[16], a3[16], a4[16], a5[16];
    snprintf(s, sizeof s,
             "{\"t\":\"est\",\"n\":%u,\"k\":%ld,\"saltos\":%u,\"at\":\"%s\",\"bits\":%u,\"p_rocio\":%.2f,"
             "\"marg\":%s,\"tv\":%s,\"polvo\":%s,\"costra\":%s,\"dias_conector\":%s,"
             "\"limpiar\":%u,\"limpiezas\":%u,\"abortadas\":%u,\"sin_turno\":%u,\"hw\":%u,\"alertas\":\"%s\"}\r\n",
             id, (long)r->k, saltos, (r->flags & 1) ? "PERMITIDA" : "BLOQUEADA", r->bits, r->p_rocio_x250 / 250.0,
             jn(a1, r->margen, 1), jn(a2, r->t_vidrio, 1), jn(a3, 100.0f * r->suelto, 3), jn(a4, 100.0f * r->costra, 3),
             jn(a5, r->dias_falla, 1), (r->flags >> 1) & 1u, r->limpiezas, r->abortadas, r->denegadas, r->hw,
             txt_alertas(r->alertas, al));
    consola(s);
}

/* ------------------------------------------------------------ árbitro de turnos (coordinador) */
static void dar_turno(sx_nodo_t *n, uint8_t id);

static void coord_solicitud(sx_nodo_t *n, uint8_t id, int32_t k, uint8_t prio)
{
    if (k < n->k - 1) return;                                   /* vieja */
    for (uint8_t i = 0; i < n->n_act; i++)
        if (n->act[i].id == id) { dar_turno(n, id); return; }   /* reintento: reenviar el turno */
    for (uint8_t i = 0; i < n->n_cola; i++)
        if (n->cola[i].id == id) { n->cola[i].k = k; n->cola[i].prio = prio; return; }
    if (n->n_cola < SX_MAX_NODOS) { n->cola[n->n_cola].id = id; n->cola[n->n_cola].k = k; n->cola[n->n_cola].prio = prio; n->n_cola++; }
}

static void coord_fin(sx_nodo_t *n, uint8_t id, int rc, uint16_t hv, float i_ma)
{
    for (uint8_t i = 0; i < n->n_act; i++)
        if (n->act[i].id == id) { n->act[i] = n->act[--n->n_act]; break; }
    for (uint8_t i = 0; i < n->n_cola; i++)                     /* también si estaba esperando */
        if (n->cola[i].id == id) { n->cola[i] = n->cola[--n->n_cola]; break; }
    char s[110], a1[16];
    snprintf(s, sizeof s, "{\"t\":\"fin\",\"n\":%u,\"k\":%ld,\"rc\":%d,\"hv_max\":%u,\"i_at_max_ma\":%s}\r\n",
             id, (long)n->k, rc, hv, jn(a1, i_ma, 2));
    consola(s);
}

static void local_turno(sx_nodo_t *n, int32_t k);

static void dar_turno(sx_nodo_t *n, uint8_t id)
{
    if (id == n->cfg.id) { local_turno(n, n->k); return; }
    uint8_t b[4];
    p32(b, 0, (uint32_t)n->k);
    sx_red_enviar(&n->red, SX_T_TURNO, id, b, 4);
}

static void coord_arbitrar(sx_nodo_t *n, uint32_t ahora)
{
    for (uint8_t i = 0; i < n->n_act;) {                        /* turnos vencidos sin FIN */
        if ((int32_t)(ahora - n->act[i].limite_ms) >= 0) n->act[i] = n->act[--n->n_act];
        else i++;
    }
    for (uint8_t i = 0; i < n->n_cola;) {                       /* solicitudes de pasos viejos */
        if (n->cola[i].k < n->k) n->cola[i] = n->cola[--n->n_cola];
        else i++;
    }
    while (n->n_act < n->cfg.max_simult && n->n_act < 8 && n->n_cola > 0) {
        uint8_t m = 0;                                          /* mayor prioridad; empate: la más antigua */
        for (uint8_t i = 1; i < n->n_cola; i++) if (n->cola[i].prio > n->cola[m].prio) m = i;
        uint8_t id = n->cola[m].id;
        for (uint8_t i = m; i + 1 < n->n_cola; i++) n->cola[i] = n->cola[i + 1];
        n->n_cola--;
        n->act[n->n_act].id = id;
        n->act[n->n_act].limite_ms = ahora + sx_eds_duracion_max_ms() + 3000u;
        n->n_act++;
        n->turnos_dados++;
        char s[60];
        snprintf(s, sizeof s, "{\"t\":\"turno\",\"n\":%u,\"k\":%ld}\r\n", id, (long)n->k);
        consola(s);
        dar_turno(n, id);
    }
}

/* ------------------------------------------------------------ limpieza del nodo */
static uint8_t ambiente_vigente(const sx_nodo_t *n, int32_t holgura)
{
    if (n->cfg.tiene_sht40 && n->cfg.tiene_ntc) return 1;
    return n->amb_ok && (n->k - n->amb_k) <= holgura;
}

/* Mantenimiento con lo que la PCB YA mide durante cada limpieza:
 *   - sin sensor de corriente (PCB actual): la AT mínima del ciclo (MEASURE_V). Si el
 *     aislamiento o los electrodos se degradan, la fuga carga al flyback y la AT baja.
 *   - con sensor de corriente (SX_TIENE_I_AT): la corriente máxima de AT.
 * Línea base = mediana de las primeras 7 limpiezas buenas. Alerta REVISAR_EDS si 3
 * limpiezas seguidas se salen de la banda, o si 3 seguidas abortan con la capa de
 * seguridad diciendo que el vidrio estaba seco. */
static void salud_eds(sx_nodo_t *n, int rc, float i_max, float hv_min)
{
    float x = SX_TIENE_I_AT ? i_max : hv_min;
    if (rc == 0) {
        n->cnt_aborto = 0;
        if (n->n_ibase < 7) {
            n->ibase[n->n_ibase++] = x;
            if (n->n_ibase == 7) {
                float v[7]; memcpy(v, n->ibase, sizeof v);
                for (int i = 1; i < 7; i++) { float y = v[i]; int j = i - 1; while (j >= 0 && v[j] > y) { v[j + 1] = v[j]; j--; } v[j + 1] = y; }
                n->i_base = v[3];
            }
        } else {
            uint8_t fuera;
            if (SX_TIENE_I_AT) { float lim = 2.0f * n->i_base; if (lim < n->i_base + 0.3f) lim = n->i_base + 0.3f; fuera = x > lim; }
            else { float caida = 0.05f * n->i_base; if (caida < 30.0f) caida = 30.0f; fuera = x < n->i_base - caida; }
            n->cnt_ialta = fuera ? (uint8_t)(n->cnt_ialta + 1) : 0;
        }
    } else if (rc == -1 || rc == -2) {
        n->cnt_aborto++;
    }
    if (n->cnt_ialta >= 3 || n->cnt_aborto >= 3) n->alerta_eds = 1;
}

static void terminar_limpieza(sx_nodo_t *n, int rc)
{
    if (rc == 0 || rc == -1 || rc == -2 || rc == -3) salud_eds(n, rc, n->i_ultimo, n->hv_min_ultimo);
    n->quiere_limpiar = 0;
    n->rc_ultimo = (int8_t)rc;
    if (rc == 0) n->limpiezas++;
    else if (rc == SX_RC_SIN_TURNO || rc == SX_RC_RECHAZADO) n->denegadas++;
    else n->abortadas++;
    if (soy_coord(n)) coord_fin(n, n->cfg.id, rc, (uint16_t)n->hv_ultimo, n->i_ultimo);
    else {
        uint8_t b[9];
        p32(b, 0, (uint32_t)n->k); b[4] = (uint8_t)(int8_t)rc; p16(b, 5, f2u16(n->hv_ultimo, 1.f));
        p16(b, 7, f2u16(n->i_ultimo, 100.f));
        sx_red_enviar(&n->red, SX_T_FIN, SX_ID_COORD, b, 9);
    }
}

static void local_turno(sx_nodo_t *n, int32_t k)
{
    if (n->eds_activo) return;                                  /* turno repetido */
    if (!n->quiere_limpiar || k != n->k || !n->O.permiso || !ambiente_vigente(n, 1)) {
        if (n->quiere_limpiar) terminar_limpieza(n, SX_RC_RECHAZADO);
        else if (!soy_coord(n)) {                               /* liberar el turno en el coordinador */
            uint8_t b[9]; p32(b, 0, (uint32_t)n->k); b[4] = (uint8_t)(int8_t)SX_RC_RECHAZADO; p16(b, 5, 0); p16(b, 7, 0);
            sx_red_enviar(&n->red, SX_T_FIN, SX_ID_COORD, b, 9);
        }
        return;
    }
    n->eds_activo = 1;
    sx_hal_gpio(SX_PIN_LED, 1);
    sx_eds_iniciar(&n->eds, sx_hal_ms(), SX_EDS_CICLOS);
}

static void pedir_turno(sx_nodo_t *n)
{
    if (soy_coord(n)) { coord_solicitud(n, n->cfg.id, n->k, n->prio); return; }
    uint8_t b[5];
    p32(b, 0, (uint32_t)n->k); b[4] = n->prio;
    sx_red_enviar(&n->red, SX_T_SOLICITUD, SX_ID_COORD, b, 5);
}

/* ------------------------------------------------------------ el paso de 15 min */
static void ejecutar_paso(sx_nodo_t *n)
{
    uint32_t ahora = sx_hal_ms();
    sx_hal_aviso_paso(n->k);
    sx_adquirir(&n->cfg, &n->L, (uint8_t)((n->k % SX_PASOS_DIA) == SX_PASO_PRUEBA));

    /* completar con el ambiente compartido (caduca a los SX_PASOS_AMB_MAX pasos) */
    uint8_t vigente = n->amb_ok && (n->k - n->amb_k) <= SX_PASOS_AMB_MAX;
    n->hw &= (uint8_t)~(SX_F_AMB_VIEJO | SX_F_SIN_COORD);
    if (!n->cfg.tiene_sht40) { n->L.t_aire = vigente ? n->amb_ta : NAN; n->L.hr = vigente ? n->amb_hr : NAN; }
    if (!n->cfg.tiene_ntc)   { n->L.t_vidrio = vigente ? n->amb_tv : NAN; }
    if (!vigente && !(n->cfg.tiene_sht40 && n->cfg.tiene_ntc)) n->hw |= SX_F_AMB_VIEJO;
    if (!soy_coord(n) && n->red.padre == SX_SIN_PUERTO) n->hw |= SX_F_SIN_COORD;

    if ((n->hw & SX_F_AMB_VIEJO) != 0) {
        /* Sin ambiente vigente (coordinador caído o sin camino): AT bloqueada y NO se corre
         * el núcleo, para no ensuciar su historia ni encender FALLA_SENSOR (que en el v2 es
         * permanente) por un problema de comunicación. Se reporta como SX_F_AMB_VIEJO. */
        n->O.permiso = 0; n->O.limpiar = 0; n->O.limpiar_ref = 0; n->O.bits = SX_B_SENSOR;
        n->O.margen = NAN; n->O.p_rocio = 0.0f;
    } else {
        n->L.lavado_realizado = n->lavado_pendiente;
        n->lavado_pendiente = 0;
        sx_paso(&n->E, n->k, &n->L, &n->O);                     /* NÚCLEO v2 (sin cambios) */
    }

    /* Sin AFE no se ve el agua que queda en el vidrio después del rocío: se exige además
     * que el vidrio lleve 1 h "claramente seco" (margen >= 4 °C y HR <= 80 %). */
    if (!n->cfg.tiene_afe && !(n->hw & SX_F_AMB_VIEJO)) {
        uint8_t seco = isfinite(n->O.margen) && n->O.margen >= SX_MARGEN_SIN_AFE && n->L.hr <= SX_HR_SIN_AFE;
        n->pasos_secos = seco ? (uint8_t)(n->pasos_secos < 250 ? n->pasos_secos + 1 : 250) : 0;
        if (n->pasos_secos < SX_PASOS_SECO_SIN_AFE) { n->O.permiso = 0; n->O.limpiar = 0; n->O.bits |= SX_B_RED_SECO; }
    }
    /* Sin mini-celdas no hay medición de polvo: la política es 1 limpieza al día en la ventana
     * de 07:00 a 11:00 (horario fijo del v2) MÁS la parte predictiva: limpiar ANTES del rocío
     * que predice la IA (máx. 1 vez cada 12 h) para que el polvo no se cemente. */
    if (!n->cfg.tiene_minic && n->O.permiso && !(n->hw & SX_F_AMB_VIEJO)) {
        int32_t paso = n->k % SX_PASOS_DIA, dia = n->k / SX_PASOS_DIA;
        if (paso >= 28 && paso < 44 && dia != n->dia_fijo) { n->O.limpiar = 1; n->dia_fijo = dia; }
        else if ((double)n->O.p_rocio >= n->P.p_rocio && (n->k - n->k_prerocio) >= SX_PASOS_DIA / 2) {
            n->O.limpiar = 1; n->k_prerocio = n->k;
        }
    }

    if (n->quiere_limpiar && !n->eds_activo) terminar_limpieza(n, SX_RC_SIN_TURNO);  /* la del paso anterior */
    if (n->O.limpiar && !n->eds_activo) {
        uint8_t puede_pedir = soy_coord(n) || (n->red.padre != SX_SIN_PUERTO) || SX_LIMPIEZA_SIN_COORD;
        n->prio = (uint8_t)((double)n->O.p_rocio >= n->P.p_rocio);            /* pre-rocío primero */
        if (puede_pedir) {
            n->quiere_limpiar = 1;
            n->k_solicitud = n->k;
            n->t_solicitud_ms = n->t_reintento_ms = ahora;
            pedir_turno(n);
        } else {
            n->denegadas++;
            n->rc_ultimo = SX_RC_SIN_TURNO;
        }
    }
    if (n->O.limpiar_ref && n->cfg.tiene_minic && !n->ref_activo) {
        sx_hal_gpio(SX_PIN_MINI_EDS_REF, 1);                      /* mini-EDS de la celda limpia, 5 s */
        n->ref_activo = 1; n->t_ref_ms = ahora;
    }

    /* telemetría */
    uint8_t b[32];
    uint8_t m = armar_estado(n, b);
    if (soy_coord(n)) registrar_estado(n, n->cfg.id, b, 0);
    else sx_red_enviar(&n->red, SX_T_ESTADO, SX_ID_COORD, b, m);

    char s[200];
    int dia = (int)(n->k / 96), hh = (int)((n->k % 96) / 4), mm = (int)((n->k % 4) * 15);
    snprintf(s, sizeof s, "[N%u] d%03d %02d:%02d Ta=%.1f HR=%.0f Tv=%.1f marg=%.1f AT:%s(0x%02X) p=%.2f polvo=%.2f%% %s%s hw=0x%02X saltos=%u\r\n",
             n->cfg.id, dia, hh, mm, n->L.t_aire, n->L.hr, n->L.t_vidrio, n->O.margen,
             n->O.permiso ? "OK" : "BLOQ", n->O.bits, n->O.p_rocio, 100.0f * n->O.suelto,
             n->quiere_limpiar ? "PIDE_TURNO " : "", n->O.alerta_conector ? "CONECTOR " : "", n->hw, n->red.saltos);
    if (!soy_coord(n)) consola(s);
}

/* ------------------------------------------------------------ tramas recibidas */
static void atender(sx_nodo_t *n, const sx_trama_t *t)
{
    uint32_t ahora = sx_hal_ms();
    switch (t->tipo) {
    case SX_T_AMBIENTE:
        if (soy_coord(n) || t->n < 12) break;
        {
            int32_t k = (int32_t)g32(t->carga, 0);
            n->amb_ta = i162f((int16_t)g16(t->carga, 4), 100.f);
            n->amb_hr = u162f(g16(t->carga, 6), 100.f);
            n->amb_tv = i162f((int16_t)g16(t->carga, 8), 100.f);
            n->amb_ok = t->carga[10] && isfinite(n->amb_ta) && isfinite(n->amb_hr) && isfinite(n->amb_tv);
            n->amb_k = k;
            n->cfg.max_simult = t->carga[11];
            n->amb_recibido = 1;
            if (k != n->k) {                                    /* nuevo paso: alinearse al coordinador */
                n->k = k;
                n->t_paso_ms = ahora;
                n->paso_pendiente = 1;
                n->t_ejecutar_ms = ahora + jitter_ms(n->cfg.id);
            }
        }
        break;
    case SX_T_ESTADO:
        if (soy_coord(n) && t->n >= 32) {
            int32_t kn = (int32_t)g32(t->carga, 0);
            if (!n->arrancado && kn > n->k) { n->k_oido = 1; n->k_oido_val = kn; }
            registrar_estado(n, t->origen, t->carga, t->saltos);
        }
        break;
    case SX_T_SOLICITUD:
        if (soy_coord(n) && t->n >= 5) coord_solicitud(n, t->origen, (int32_t)g32(t->carga, 0), t->carga[4]);
        break;
    case SX_T_TURNO:
        if (t->destino == n->cfg.id && t->n >= 4) local_turno(n, (int32_t)g32(t->carga, 0));
        break;
    case SX_T_FIN:
        if (soy_coord(n) && t->n >= 9) coord_fin(n, t->origen, (int8_t)t->carga[4], g16(t->carga, 5), u162f(g16(t->carga, 7), 100.f));
        break;
    case SX_T_COMANDO:
        if (t->destino != n->cfg.id || t->n < 1) break;
        if (t->carga[0] == 1) { n->lavado_pendiente = 1; consola("[nodo] LAVADO recibido\r\n"); }  /* la cuadrilla lavó */
        else if (t->carga[0] == 2 && t->n >= 9) {               /* calibración de instalación */
            memcpy(&n->cfg.c_ref_nf, &t->carga[1], 4);
            memcpy(&n->cfg.k_sr, &t->carga[5], 4);
            n->E.c_ref = n->cfg.c_ref_nf; n->E.k_sr = n->cfg.k_sr;
            cfg_guardar(n);
        }
        break;
    }
}

/* ------------------------------------------------------------ comandos del gateway */
static void comando_gateway(sx_nodo_t *n, char *l)
{
    char s[160];
    int a, b2, c;
    float f1, f2, f3;
    if (sscanf(l, "HORA %d %d:%d", &a, &b2, &c) == 3 && soy_coord(n)) {
        n->k = a * SX_PASOS_DIA + b2 * 4 + c / 15 - 1;          /* el siguiente paso será esa hora */
        n->t_paso_ms = sx_hal_ms() - SX_PASO_MS;
        n->arrancado = 1;
        snprintf(s, sizeof s, "{\"t\":\"ok\",\"cmd\":\"HORA\",\"k\":%ld}\r\n", (long)(n->k + 1));
    } else if (sscanf(l, "AMB %f %f %f", &f1, &f2, &f3) == 3) {
        n->gw_ta = f1; n->gw_hr = f2; n->gw_tv = f3; n->gw_k = n->k; n->gw_ok = 1;
        return;                                                 /* llega cada paso: sin eco */
    } else if (sscanf(l, "LAVADO %d", &a) == 1) {
        if (a == n->cfg.id) n->lavado_pendiente = 1;
        else { uint8_t m = 1; sx_red_enviar(&n->red, SX_T_COMANDO, (uint8_t)a, &m, 1); }
        snprintf(s, sizeof s, "{\"t\":\"ok\",\"cmd\":\"LAVADO\",\"n\":%d}\r\n", a);
    } else if (sscanf(l, "MAXSIM %d", &a) == 1 && a >= 1 && a <= 8) {
        n->cfg.max_simult = (uint8_t)a; cfg_guardar(n);
        snprintf(s, sizeof s, "{\"t\":\"ok\",\"cmd\":\"MAXSIM\",\"v\":%d}\r\n", a);
    } else if (sscanf(l, "CAL %d %f %f", &a, &f1, &f2) == 3) {
        uint8_t m[9]; m[0] = 2; memcpy(&m[1], &f1, 4); memcpy(&m[5], &f2, 4);
        if (a == n->cfg.id) { n->cfg.c_ref_nf = f1; n->cfg.k_sr = f2; n->E.c_ref = f1; n->E.k_sr = f2; cfg_guardar(n); }
        else sx_red_enviar(&n->red, SX_T_COMANDO, (uint8_t)a, m, 9);
        snprintf(s, sizeof s, "{\"t\":\"ok\",\"cmd\":\"CAL\",\"n\":%d}\r\n", a);
    } else if (sscanf(l, "ID %d", &a) == 1 && a >= 1 && a <= SX_MAX_NODOS) {
        n->cfg.id = (uint8_t)a; cfg_guardar(n);
        snprintf(s, sizeof s, "{\"t\":\"ok\",\"cmd\":\"ID\",\"v\":%d,\"nota\":\"reiniciar\"}\r\n", a);
    } else if (strncmp(l, "RED", 3) == 0) {
        snprintf(s, sizeof s, "{\"t\":\"red\",\"n\":%u,\"padre\":%u,\"saltos\":%u,\"vecinos\":[%u,%u,%u,%u],\"rx\":%lu,\"crc\":%lu,\"reenv\":%lu}\r\n",
                 n->cfg.id, n->red.padre, n->red.saltos, n->red.vecino[0], n->red.vecino[1], n->red.vecino[2], n->red.vecino[3],
                 (unsigned long)n->red.rx_ok, (unsigned long)n->red.rx_crc, (unsigned long)n->red.reenviadas);
    } else {
        snprintf(s, sizeof s, "{\"t\":\"error\",\"cmd\":\"%.30s\"}\r\n", l);
    }
    consola(s);
}

/* ------------------------------------------------------------ lazo */
void sx_nodo_ciclo(sx_nodo_t *n)
{
    uint32_t ahora = sx_hal_ms();
    sx_hal_wdt();

    /* 1. red */
    sx_trama_t t;
    while (sx_red_recibir(&n->red, &t, ahora)) atender(n, &t);

    /* 2. reloj del paso */
    if (soy_coord(n)) {
        /* Al arrancar, el coordinador no sabe la hora (no hay RTC con cristal): espera
         * 1.6 pasos por si los nodos ya llevan la cuenta (adopta su k), o un HORA del gateway. */
        uint8_t toca = n->arrancado ? (ahora - n->t_paso_ms) >= SX_PASO_MS
                                    : (n->k_oido || (ahora - n->t_paso_ms) >= (SX_PASO_MS * 8u) / 5u);
        if (toca) {
            if (!n->arrancado && n->k_oido) n->k = n->k_oido_val;
            n->t_paso_ms = n->arrancado ? n->t_paso_ms + SX_PASO_MS : ahora;
            n->arrancado = 1;
            n->k++;
            /* ambiente para todo el arreglo: SHT40 + NTC del coordinador */
            float ta = NAN, hr = NAN, tv = NAN;
            sx_hal_aviso_paso(n->k);
            /* Fuente del ambiente: sensores propios si la tarjeta los tiene; si no, la estación
             * meteorológica de la planta (T aire, HR, T de módulo) que manda el SCADA por el
             * gateway con "AMB ta hr tmod". Vale SX_PASOS_AMB_MAX pasos. */
            uint8_t gw = n->gw_ok && (n->k - n->gw_k) <= SX_PASOS_AMB_MAX;
            uint8_t ok = 1;
            if (n->cfg.tiene_sht40) ok = (sx_hal_sht40(&ta, &hr) == 0);
            else if (gw) { ta = n->gw_ta; hr = n->gw_hr; }
            else ok = 0;
            if (n->cfg.tiene_ntc) tv = sx_conv_ntc(sx_hal_adc(SX_ADC_NTC, 16));
            else if (gw) tv = n->gw_tv;
            ok = (uint8_t)(ok && isfinite(ta) && isfinite(hr) && isfinite(tv) && tv > -39.0f);
            n->amb_ta = ta; n->amb_hr = hr; n->amb_tv = tv; n->amb_k = n->k; n->amb_ok = ok;
            enviar_ambiente(n, ta, hr, tv, ok);
            n->paso_pendiente = 1;
            n->t_ejecutar_ms = ahora + jitter_ms(n->cfg.id);
        }
    } else if ((ahora - n->t_paso_ms) >= (SX_PASO_MS + SX_PASO_MS / 2u)) {
        /* sin AMBIENTE: el nodo sigue con su propio reloj (el ambiente caducará) */
        n->t_paso_ms += SX_PASO_MS;
        n->k++;
        n->paso_pendiente = 1;
        n->t_ejecutar_ms = ahora + jitter_ms(n->cfg.id);
        if (n->amb_recibido && n->red.padre != SX_SIN_PUERTO && (n->k - n->amb_k) > SX_PASOS_AMB_MAX)
            n->red.padre = SX_SIN_PUERTO;                         /* el camino ya no sirve */
    }
    if (n->paso_pendiente && (int32_t)(ahora - n->t_ejecutar_ms) >= 0 && !n->eds_activo && n->k >= 0) {
        n->paso_pendiente = 0;
        ejecutar_paso(n);
    }

    /* 3. limpieza en curso / reintentos de la solicitud */
    if (n->eds_activo) {
        if (!sx_eds_tick(&n->eds, ahora)) {
            n->eds_activo = 0;
            sx_hal_gpio(SX_PIN_LED, 0);
            n->hv_ultimo = n->eds.hv_max;
            n->i_ultimo = n->eds.i_max;
            n->hv_min_ultimo = n->eds.hv_min;
            terminar_limpieza(n, n->eds.rc);
        }
    } else if (n->quiere_limpiar) {
        uint32_t espera = SX_PASO_MS / 10u; if (espera < 500u) espera = 500u;
        if (ahora - n->t_solicitud_ms > (SX_PASO_MS * 6u) / 10u) terminar_limpieza(n, SX_RC_SIN_TURNO);
        else if (ahora - n->t_reintento_ms > espera) { n->t_reintento_ms = ahora; pedir_turno(n); }
    }
    if (n->ref_activo && ahora - n->t_ref_ms >= 5000u) { sx_hal_gpio(SX_PIN_MINI_EDS_REF, 0); n->ref_activo = 0; }

    /* 4. árbitro */
    if (soy_coord(n)) coord_arbitrar(n, ahora);

    /* 5. consola / gateway */
    int c;
    while ((c = sx_hal_gateway_rx()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (n->n_lin) { n->lin[n->n_lin] = 0; comando_gateway(n, n->lin); n->n_lin = 0; }
        } else if (n->n_lin < sizeof(n->lin) - 1) n->lin[n->n_lin++] = (char)c;
    }

    /* 6. latido */
    if (!n->eds_activo && ahora - n->t_latido_ms >= 500u) {
        n->t_latido_ms = ahora;
        sx_hal_gpio(SX_PIN_LED, (uint8_t)((ahora / 500u) & 1u));
    }
}
