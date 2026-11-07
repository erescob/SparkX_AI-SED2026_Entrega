/* =========================================================================
 * sim_red.c — Varias tarjetas Spark-X corriendo el firmware REAL en la PC,
 *             conectadas por enlaces de 1 hilo simulados BIT POR BIT.
 *
 * Arreglo de 2 x 3 paneles (nodo 1 = coordinador, en la esquina):
 *
 *        [1]---[2]---[3]
 *         |     |     |
 *        [4]---[5]---[6]
 *
 * Cada tarjeta lee su propio CSV (datos/red_nodoN.csv): mismo clima, sensores y
 * fallas propios. El HAL convierte las lecturas a CÓDIGOS DE ADC y modela la AT
 * (sube con el flyback, se cae si hay agua en el vidrio, se descarga al apagar).
 *
 * Eventos del escenario (para probar la red, no solo el modelo):
 *   día 12 03:00-09:00  se APAGA el coordinador (los nodos deben bloquear la AT)
 *   día 16 12:00        se CORTA el cable 1-2 (la red debe rehacer el camino por 4-5)
 *   día 22 08:00        el operador avisa LAVADO del nodo 2
 * ========================================================================= */
#include "../sx_hal.h"
#include "../sx_nodo.h"
#include "../sx_uart1h.h"
#include "sx_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#define NT 6
#define TICKS_S (SX_BAUD_ENLACE * SX_SOBREMUESTREO)
#define MAXF 3000

typedef struct { float ta, hr, tv, isc_s, isc_l, c, g, vr, vc, ic; int lav, agua; } fila_t;

typedef struct {
    int id, encendida;
    sx_nodo_t nodo;
    fila_t *F; int nf; float c_ref, k_sr;
    int fila;
    uint8_t pin[SX_PIN_N];
    u1h_t u[SX_N_PUERTOS];
    uint8_t sal[SX_N_PUERTOS];
    float hv, i_at; uint32_t t_hv;
    uint8_t fases; float hz; uint32_t t_fases;
    FILE *log;
    char rxq[256]; int rq_r, rq_w;
    uint8_t cfg[64]; int cfg_ok;
    uint32_t azar;
    sx_cfg_t hw;
    /* auditoría */
    uint32_t eds_ms_con_agua, eds_inicios, eds_con_amb_viejo;
} tarjeta_t;

static tarjeta_t T[NT];
static int completo;                    /* 0 = PCB actual (sin sensores nuevos), 1 = con el hardware del v2 */
static tarjeta_t *g;                    /* tarjeta "actual" para el HAL */
static uint32_t g_ms;

/* vecinos: T[i].puerto p <-> (otra tarjeta, su puerto) ; -1 = sin cable */
static int vec_t[NT][4], vec_p[NT][4];
static int cable_ok[NT][4];

static void conectar(int a, int pa, int b, int pb)
{
    vec_t[a][pa] = b; vec_p[a][pa] = pb; vec_t[b][pb] = a; vec_p[b][pb] = pa;
    cable_ok[a][pa] = cable_ok[b][pb] = 1;
}
static void cortar(int a, int pa)
{
    int b = vec_t[a][pa], pb = vec_p[a][pa];
    cable_ok[a][pa] = 0; if (b >= 0) cable_ok[b][pb] = 0;
}

/* ------------------------------------------------------------------ HAL */
static uint16_t codigo(float v)
{
    float c = v / SX_VREF * SX_ADC_MAX + 0.5f;
    if (c < 0) c = 0;
    if (c > 4095) c = 4095;
    return (uint16_t)c;
}
static float ntc_v(float t)
{
    if (!isfinite(t) || t <= -39.0f) return SX_VREF;
    float r = 10000.0f * expf(3950.0f * (1.0f / (t + 273.15f) - 1.0f / 298.15f));
    return SX_VREF * r / (r + 10000.0f);
}
static const fila_t *fila(void) { return &g->F[g->fila]; }

static void modelo_at(void)
{
    float dt = (float)(g_ms - g->t_hv);
    g->t_hv = g_ms;
    const fila_t *f = fila();
    float obj, tau;
    if (g->pin[SX_PIN_ON_OFF_FLYBACK]) {
        obj = f->agua ? 550.0f : 1250.0f;                   /* con agua la fuga tira la AT */
        g->i_at = f->agua ? 3.0f : 0.15f;
        float dia = (float)g->fila / 96.0f;                  /* nodo 2: aislamiento que se degrada */
        if (g->id == 2 && dia > 15.0f && !f->agua) g->i_at += 0.06f * (dia - 15.0f);
        obj -= 400.0f * (g->i_at - 0.15f);                   /* flyback sin regulación: la fuga baja la AT */
        tau = 15.0f;
    } else {
        obj = 0.0f; g->i_at = 0.0f;
        int descarga = g->pin[SX_PIN_PHASE_A] != SX_FASE_ACTIVA && g->pin[SX_PIN_PHASE_B] != SX_FASE_ACTIVA && !g->fases;
        tau = descarga ? 40.0f : 400.0f;
    }
    g->hv = obj + (g->hv - obj) * expf(-dt / tau);
}

void sx_hal_iniciar(void) {}
uint32_t sx_hal_ms(void) { return g_ms; }
void sx_hal_delay_ms(uint32_t ms) { (void)ms; }
void sx_hal_wdt(void) {}
uint32_t sx_hal_aleatorio(void) { uint32_t x = g->azar; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return g->azar = x; }
uint32_t sx_hal_uid(void) { return 0x5A000000u + (uint32_t)g->id; }
void sx_hal_aviso_paso(int32_t k) { g->fila = (int)(((k % g->nf) + g->nf) % g->nf); }

uint16_t sx_hal_adc(uint8_t canal, uint8_t prom)
{
    (void)prom;
    const fila_t *f = fila();
    modelo_at();
    switch (canal) {
    case SX_ADC_HV_MON: return g->pin[SX_PIN_ACTIVE_V] ? codigo(g->hv / SX_K_MEASURE_V) : 0;
    case SX_ADC_I_AT:   return g->pin[SX_PIN_ACTIVE_C] ? codigo(g->i_at / SX_K_MEASURE_C) : 0;
    case SX_ADC_NTC:    return codigo(ntc_v(f->tv));
    case SX_ADC_ISC_S:  return codigo(f->isc_s / SX_K_ISC_MA);
    case SX_ADC_ISC_L:  return codigo(f->isc_l / SX_K_ISC_MA);
    case SX_ADC_V12:    return codigo((g->pin[SX_PIN_CARGA_PRUEBA] ? f->vc : f->vr) / SX_K_V12);
    case SX_ADC_I12:    return codigo((g->pin[SX_PIN_CARGA_PRUEBA] ? f->ic : 0.0f) / SX_K_I12);
    }
    return 0;
}
void sx_hal_gpio(uint8_t p, uint8_t v) { if (p < SX_PIN_N) { modelo_at(); g->pin[p] = v; } }
int sx_hal_sht40(float *t, float *h) { *t = fila()->ta; *h = fila()->hr; return 0; }
int sx_hal_afe(float *c, float *gg) { *c = fila()->c; *gg = fila()->g; return 0; }
void sx_hal_fases(uint8_t on, float hz)
{
    modelo_at();
    g->fases = on; g->hz = hz; g->t_fases = g_ms;
    if (on) {
        g->eds_inicios++;
        if (g->nodo.hw & SX_F_AMB_VIEJO) g->eds_con_amb_viejo++;
    }
}
uint32_t sx_hal_fases_ciclos(void) { return g->fases ? (uint32_t)((float)(g_ms - g->t_fases) * g->hz / 1000.0f) : 0; }
uint8_t sx_hal_enlace_enviar(uint8_t p, const uint8_t *b, uint8_t n) { return u1h_encolar(&g->u[p], b, n); }
int sx_hal_enlace_leer(uint8_t p) { return u1h_leer(&g->u[p]); }
void sx_hal_gateway_tx(const char *s) { if (g->log) fputs(s, g->log); }
int sx_hal_gateway_rx(void) { if (g->rq_r == g->rq_w) return -1; char c = g->rxq[g->rq_r]; g->rq_r = (g->rq_r + 1) & 255; return (unsigned char)c; }
int sx_hal_cfg_leer(void *b, uint32_t n) { if (!g->cfg_ok) { /* primera vez: configuración de fábrica de esta tarjeta */ memcpy(b, &g->hw, n); return 0; } memcpy(b, g->cfg, n); return 0; }
int sx_hal_cfg_escribir(const void *b, uint32_t n) { memcpy(g->cfg, b, n); g->cfg_ok = 1; return 0; }

static void comando(tarjeta_t *t, const char *s) { for (; *s; s++) { t->rxq[t->rq_w] = *s; t->rq_w = (t->rq_w + 1) & 255; } }

/* ------------------------------------------------------------------ datos */
static int cargar(tarjeta_t *t, const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (!f) { fprintf(stderr, "no encuentro %s\n", ruta); return 0; }
    char l[512];
    if (!fgets(l, sizeof l, f) || sscanf(l, "# %f,%f", &t->c_ref, &t->k_sr) != 2) return 0;
    if (!fgets(l, sizeof l, f)) return 0;
    t->F = calloc(MAXF, sizeof(fila_t)); t->nf = 0;
    while (fgets(l, sizeof l, f) && t->nf < MAXF) {
        fila_t *r = &t->F[t->nf]; int k;
        if (sscanf(l, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%d", &k, &r->ta, &r->hr, &r->tv, &r->isc_s, &r->isc_l,
                   &r->c, &r->g, &r->vr, &r->vc, &r->ic, &r->lav, &r->agua) == 13) t->nf++;
    }
    fclose(f);
    return t->nf;
}

/* ------------------------------------------------------------------ simulación */
static uint8_t linea(int i, int p)
{
    if (!T[i].encendida) return 1;
    uint8_t v = T[i].sal[p];
    int j = vec_t[i][p];
    if (j >= 0 && cable_ok[i][p] && T[j].encendida) v &= T[j].sal[vec_p[i][p]];
    return v;
}

static int actividad(void)
{
    for (int i = 0; i < NT; i++) {
        if (!T[i].encendida) continue;
        for (int p = 0; p < 4; p++) {
            u1h_t *u = &T[i].u[p];
            if (u->tx_activo || u->rx_est || u->q_r != u->q_w || !T[i].sal[p]) return 1;
        }
    }
    return 0;
}

static void encender(int i)
{
    T[i].encendida = 1;
    for (int p = 0; p < 4; p++) { u1h_iniciar(&T[i].u[p], 0x9E3779B9u * (uint32_t)(T[i].id * 4 + p + 1) + g_ms); T[i].sal[p] = 1; }
    g = &T[i];
    memset(T[i].pin, 0, sizeof T[i].pin);
    sx_nodo_iniciar(&T[i].nodo);
}

static int en_eds(int i) { return T[i].encendida && T[i].nodo.eds_activo; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../datos";
    const char *hwenv = getenv("SX_SIM_HW");
    completo = hwenv && strcmp(hwenv, "completo") == 0;
    const char *out = argc > 2 ? argv[2] : "../resultados/sim_red";
    char ruta[300];
    memset(vec_t, -1, sizeof vec_t);
    /* cuadrícula 2x3: puertos 0=UP 1=RIGHT 2=DOWN 3=LEFT */
    conectar(0, 1, 1, 3); conectar(1, 1, 2, 3);          /* 1-2, 2-3 */
    conectar(3, 1, 4, 3); conectar(4, 1, 5, 3);          /* 4-5, 5-6 */
    conectar(0, 2, 3, 0); conectar(1, 2, 4, 0); conectar(2, 2, 5, 0);   /* verticales */

    for (int i = 0; i < NT; i++) {
        tarjeta_t *t = &T[i];
        t->id = i + 1;
        t->azar = 0xC0FFEEu * (uint32_t)(i + 7);
        snprintf(ruta, sizeof ruta, "%s/red_nodo%d.csv", dir, t->id);
        if (!cargar(t, ruta)) return 1;
        if (t->id == 1) snprintf(ruta, sizeof ruta, "%s/gateway_nodo1.log", out);
        else snprintf(ruta, sizeof ruta, "%s/nodo%d.log", out, t->id);
        t->log = fopen(ruta, "w");
        /* hardware de cada tarjeta: el coordinador tiene todo; el resto NO tiene SHT40
         * (usa el ambiente de la red); el nodo 6 tampoco tiene NTC (usa la T de referencia). */
        sx_cfg_t *c = &t->hw;
        memset(c, 0, sizeof *c);
        c->magia = SX_CFG_MAGIA; c->id = (uint8_t)t->id;
        if (completo) {
            c->tiene_sht40 = (t->id == 1); c->tiene_ntc = (t->id != 6);
            c->tiene_minic = 1; c->tiene_afe = 1; c->tiene_prueba = 1;
        } /* si no: PCB actual, sin ningún sensor nuevo (todo en 0) */
        /* prueba de la segunda barrera: SX_SIM_DEGRADADO=n deja al nodo n sin AFE ni NTC */
        const char *dg = getenv("SX_SIM_DEGRADADO");
        if (dg && atoi(dg) == t->id) { c->tiene_afe = 0; c->tiene_ntc = 0; }
        c->max_simult = 1; c->c_ref_nf = t->c_ref; c->k_sr = t->k_sr;
        c->crc = sx_crc16((const uint8_t *)c, (uint16_t)offsetof(sx_cfg_t, crc));
    }
    for (int i = 0; i < NT; i++) encender(i);

    const uint32_t pasos = (uint32_t)T[0].nf - SX_K0;
    const uint64_t fin_ms = (uint64_t)pasos * SX_PASO_MS + 2u * SX_PASO_MS;
    uint32_t acc = 0, max_simult = 0, ms_simult_viol = 0;
    int ev_off = 0, ev_on = 0, ev_corte = 0, ev_lav = 0;
    int k_off = 12 * 96 + 3 * 4, k_on = 12 * 96 + 9 * 4, k_corte = 16 * 96 + 48, k_lav = 22 * 96 + 32;
    uint64_t ticks = 0, ticks_sim = 0;

    for (g_ms = 0; (uint64_t)g_ms < fin_ms; g_ms++) {
        int32_t k = T[1].nodo.k;                          /* reloj de referencia: nodo 2 */
        if (!ev_off && k >= k_off) { ev_off = 1; T[0].encendida = 0; fprintf(T[0].log, "### coordinador APAGADO (k=%d)\n", k); }
        if (!ev_on && k >= k_on) { ev_on = 1; encender(0); fprintf(T[0].log, "### coordinador ENCENDIDO (k=%d)\n", k); }
        if (!ev_corte && k >= k_corte) { ev_corte = 1; cortar(0, 1); fprintf(T[0].log, "### cable 1-2 CORTADO (k=%d)\n", k); }
        if (!ev_lav && k >= k_lav && T[0].encendida) { ev_lav = 1; comando(&T[0], "LAVADO 2\nRED\n"); }

        /* SCADA -> coordinador: datos de la estación de la planta para el paso siguiente
         * (T aire, HR y T de módulo; aquí se usan los del CSV del nodo 1 como "estación") */
        static int32_t k_amb = -99;
        if (T[0].encendida && T[0].nodo.k != k_amb) {
            k_amb = T[0].nodo.k;
            int r = (int)(((k_amb + 1) % T[0].nf + T[0].nf) % T[0].nf);
            char l[80];
            snprintf(l, sizeof l, "AMB %.2f %.2f %.2f\n", T[0].F[r].ta, T[0].F[r].hr, T[0].F[r].tv);
            comando(&T[0], l);
        }
        /* ticks de los enlaces en este milisegundo */
        acc += TICKS_S;
        uint32_t nt = acc / 1000u; acc %= 1000u;
        ticks += nt;
        if (actividad()) {
            for (uint32_t s = 0; s < nt; s++) {
                uint8_t lv[NT][4];
                for (int i = 0; i < NT; i++) for (int p = 0; p < 4; p++) lv[i][p] = linea(i, p);
                for (int i = 0; i < NT; i++) if (T[i].encendida) for (int p = 0; p < 4; p++) T[i].sal[p] = u1h_tick(&T[i].u[p], lv[i][p]);
                ticks_sim++;
            }
        } else {
            for (int i = 0; i < NT; i++) for (int p = 0; p < 4; p++) { u1h_t *u = &T[i].u[p]; uint32_t v = u->inactivo + nt; u->inactivo = v > 0xFFFF ? 0xFFFF : (uint16_t)v; }
        }
        /* firmware */
        uint32_t act = 0;
        for (int i = 0; i < NT; i++) {
            if (!T[i].encendida) continue;
            g = &T[i];
            sx_nodo_ciclo(&T[i].nodo);
            if (en_eds(i)) {
                act++;
                if (fila()->agua && T[i].pin[SX_PIN_ON_OFF_FLYBACK]) T[i].eds_ms_con_agua++;
            }
        }
        if (act > max_simult) max_simult = act;
        if (act > 1) ms_simult_viol++;
    }

    /* ---------------- resumen ---------------- */
    snprintf(ruta, sizeof ruta, "%s/resumen.json", out);
    FILE *r = fopen(ruta, "w");
    fprintf(r, "{\n \"hardware\": \"%s\",\n \"pasos\": %u, \"paso_ms\": %u, \"tarjetas\": %d,\n", completo ? "completo (v2)" : "PCB actual sin componentes nuevos", pasos, (unsigned)SX_PASO_MS, NT);
    fprintf(r, " \"max_limpiezas_simultaneas\": %u, \"ms_con_mas_de_1\": %u,\n", max_simult, ms_simult_viol);
    fprintf(r, " \"ticks_totales\": %llu, \"ticks_simulados_bit_a_bit\": %llu,\n", (unsigned long long)ticks, (unsigned long long)ticks_sim);
    fprintf(r, " \"nodos\": [\n");
    for (int i = 0; i < NT; i++) {
        tarjeta_t *t = &T[i]; sx_nodo_t *n = &t->nodo;
        uint32_t col = 0, mar = 0, crc = n->red.rx_crc;
        for (int p = 0; p < 4; p++) { col += t->u[p].colisiones; mar += t->u[p].err_marco; }
        fprintf(r, "  {\"id\":%d,\"alerta_eds\":%u,\"limpiezas\":%u,\"abortadas\":%u,\"sin_turno\":%u,\"inicios_eds\":%u,"
                   "\"ms_eds_con_agua\":%u,\"eds_con_ambiente_viejo\":%u,\"saltos\":%u,\"padre\":%u,"
                   "\"tramas_ok\":%u,\"crc_malo\":%u,\"colisiones\":%u,\"err_marco\":%u,\"reenviadas\":%u,"
                   "\"alerta_conector\":%u,\"alerta_termica\":%u,\"alerta_lavado\":%u,\"falla_sensor\":%u}%s\n",
                t->id, n->alerta_eds, n->limpiezas, n->abortadas, n->denegadas, t->eds_inicios, t->eds_ms_con_agua, t->eds_con_amb_viejo,
                n->red.saltos, n->red.padre, n->red.rx_ok, crc, col, mar, n->red.reenviadas,
                n->O.alerta_conector, n->O.alerta_termica, n->O.alerta_lavado, n->O.falla_sensor, i < NT - 1 ? "," : "");
    }
    fprintf(r, " ]\n}\n");
    fclose(r);
    for (int i = 0; i < NT; i++) fclose(T[i].log);
    printf("simulación terminada: %u pasos, máx. limpiezas simultáneas = %u\n", pasos, max_simult);
    return 0;
}
