/* sx_red.c — ver sx_red.h */
#include "sx_red.h"
#include "sx_hal.h"
#include <string.h>

uint16_t sx_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t c = 0xFFFFu;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
    }
    return c;
}

void sx_red_iniciar(sx_red_t *r, uint8_t id)
{
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->padre = SX_SIN_PUERTO;
    r->saltos = 0xFF;
    r->seq = (uint8_t)sx_hal_aleatorio();
    if (id == SX_ID_COORD) r->saltos = 0;
}

static uint8_t armar(const sx_trama_t *t, uint8_t *b)
{
    uint8_t len = (uint8_t)(6u + t->n);
    b[0] = SX_SYNC; b[1] = len;
    b[2] = t->tipo; b[3] = t->origen; b[4] = t->destino; b[5] = t->seq; b[6] = t->ttl; b[7] = t->saltos;
    memcpy(&b[8], t->carga, t->n);
    uint16_t c = sx_crc16(&b[1], (uint16_t)(len + 1u));
    b[8 + t->n] = (uint8_t)(c >> 8);
    b[9 + t->n] = (uint8_t)c;
    return (uint8_t)(10u + t->n);
}

static uint8_t visto(sx_red_t *r, uint8_t origen, uint8_t seq)
{
    uint16_t k = (uint16_t)((origen << 8) | seq);
    for (int i = 0; i < 32; i++) if (r->dup_o[i] == k) return 1;
    r->dup_o[r->dup_i] = k;
    r->dup_i = (uint8_t)((r->dup_i + 1u) & 31u);
    return 0;
}

/* inunda por todos los puertos menos 'excepto' */
static uint8_t inundar(const sx_trama_t *t, uint8_t excepto)
{
    uint8_t b[U1H_TRAMA_MAX_RED], ok = 0;
    uint8_t n = armar(t, b);
    for (uint8_t p = 0; p < SX_N_PUERTOS; p++)
        if (p != excepto) ok |= sx_hal_enlace_enviar(p, b, n);
    return ok;
}

static uint8_t hacia_coord(sx_red_t *r, const sx_trama_t *t, uint8_t excepto)
{
    if (r->padre != SX_SIN_PUERTO && r->padre != excepto) {
        uint8_t b[U1H_TRAMA_MAX_RED];
        uint8_t n = armar(t, b);
        return sx_hal_enlace_enviar(r->padre, b, n);
    }
    return inundar(t, excepto);            /* sin camino conocido: inundar */
}

uint8_t sx_red_enviar(sx_red_t *r, uint8_t tipo, uint8_t destino, const void *carga, uint8_t n)
{
    if (n > SX_CARGA_MAX) return 0;
    sx_trama_t t;
    t.tipo = tipo; t.origen = r->id; t.destino = destino; t.seq = ++r->seq;
    t.ttl = SX_TTL; t.saltos = 0; t.n = n;
    memcpy(t.carga, carga, n);
    visto(r, t.origen, t.seq);
    if (destino == SX_ID_COORD && r->id != SX_ID_COORD) return hacia_coord(r, &t, SX_SIN_PUERTO);
    return inundar(&t, SX_SIN_PUERTO);
}

/* Procesa una trama válida que llegó por 'p'. Devuelve 1 si es para mí. */
static uint8_t procesar(sx_red_t *r, sx_trama_t *t, uint32_t ahora)
{
    r->rx_ok++;
    if (t->saltos == 0) r->vecino[t->puerto] = t->origen;   /* vecino directo */
    if (t->origen == r->id || visto(r, t->origen, t->seq)) { r->descartadas++; return 0; }

    /* el AMBIENTE del coordinador define el camino de regreso */
    if (t->tipo == SX_T_AMBIENTE && t->origen == SX_ID_COORD && r->id != SX_ID_COORD) {
        r->padre = t->puerto;
        r->saltos = (uint8_t)(t->saltos + 1u);
        r->t_padre_ms = ahora;
    }
    uint8_t para_mi = (t->destino == r->id || t->destino == SX_BCAST);
    uint8_t reenviar = (t->destino != r->id) && t->ttl > 1;
    if (reenviar) {
        sx_trama_t f = *t;
        f.ttl--; f.saltos++;
        if (f.destino == SX_ID_COORD) hacia_coord(r, &f, t->puerto);
        else inundar(&f, t->puerto);
        r->reenviadas++;
    }
    return para_mi;
}

uint8_t sx_red_recibir(sx_red_t *r, sx_trama_t *t, uint32_t ahora)
{
    for (uint8_t p = 0; p < SX_N_PUERTOS; p++) {
        sx_rx_t *a = &r->rx[p];
        int c;
        while ((c = sx_hal_enlace_leer(p)) >= 0) {
            uint8_t b = (uint8_t)c;
            switch (a->est) {
            case 0: if (b == SX_SYNC) { a->est = 1; a->i = 0; } break;
            case 1:
                if (b < 6 || b > 6 + SX_CARGA_MAX) { a->est = (b == SX_SYNC); a->i = 0; break; }
                a->len = b; a->buf[0] = b; a->i = 1; a->est = 2; break;
            case 2:
                a->buf[a->i++] = b;
                if (a->i == (uint8_t)(a->len + 3u)) {          /* LEN + datos + 2 de CRC */
                    a->est = 0;
                    uint16_t crc = (uint16_t)((a->buf[a->len + 1] << 8) | a->buf[a->len + 2]);
                    if (sx_crc16(a->buf, (uint16_t)(a->len + 1u)) != crc) { r->rx_crc++; break; }
                    t->tipo = a->buf[1]; t->origen = a->buf[2]; t->destino = a->buf[3];
                    t->seq = a->buf[4]; t->ttl = a->buf[5]; t->saltos = a->buf[6];
                    t->n = (uint8_t)(a->len - 6u); t->puerto = p;
                    memcpy(t->carga, &a->buf[7], t->n);
                    if (procesar(r, t, ahora)) return 1;
                }
                break;
            }
        }
    }
    return 0;
}
