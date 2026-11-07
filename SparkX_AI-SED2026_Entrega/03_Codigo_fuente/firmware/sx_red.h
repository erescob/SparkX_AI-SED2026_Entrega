/* =========================================================================
 * sx_red.h — Red entre las tarjetas del arreglo (una por panel).
 *
 * Topología: malla en cuadrícula; cada tarjeta habla solo con sus 4 vecinas
 * (UP/RIGHT/DOWN/LEFT). El nodo 1 es el COORDINADOR.
 *
 * Trama (bytes):
 *   0xA5 | LEN | TIPO | ORIGEN | DESTINO | SEQ | TTL | SALTOS | carga... | CRC16
 *   LEN = bytes desde TIPO hasta el final de la carga. CRC-16/CCITT sobre LEN..carga.
 *
 * Ruteo:
 *   - Hacia abajo (coordinador -> todos, o a un nodo): inundación con TTL y
 *     filtro de duplicados (ORIGEN, SEQ).
 *   - Hacia arriba (nodo -> coordinador): por el "puerto padre", el puerto por
 *     donde llegó primero el último AMBIENTE (camino con menos saltos).
 * ========================================================================= */
#ifndef SX_RED_H
#define SX_RED_H
#include <stdint.h>
#include "sx_placa.h"

#define SX_SYNC          0xA5u
#define SX_BCAST         0xFFu
#define SX_CARGA_MAX     48
#define SX_TTL           16
#define SX_SIN_PUERTO    0xFFu
#define U1H_TRAMA_MAX_RED (10 + SX_CARGA_MAX)

enum {                      /* tipos de trama */
    SX_T_AMBIENTE = 1,      /* coord -> todos: paso k, T/HR del aire, T vidrio ref, permisos */
    SX_T_ESTADO   = 2,      /* nodo -> coord: resultado de sx_paso() y alertas               */
    SX_T_SOLICITUD= 3,      /* nodo -> coord: quiero limpiar en este paso                    */
    SX_T_TURNO    = 4,      /* coord -> nodo: puedes limpiar ahora                           */
    SX_T_FIN      = 5,      /* nodo -> coord: terminé (rc)                                   */
    SX_T_COMANDO  = 6       /* coord -> nodo: lavado realizado, calibración, etc.            */
};

typedef struct {
    uint8_t tipo, origen, destino, seq, ttl, saltos, n;
    uint8_t puerto;          /* por dónde llegó */
    uint8_t carga[SX_CARGA_MAX];
} sx_trama_t;

typedef struct {             /* analizador de bytes por puerto */
    uint8_t est, len, i;
    uint8_t buf[2 + 6 + SX_CARGA_MAX + 2];
} sx_rx_t;

typedef struct {
    uint8_t id;
    uint8_t seq;
    uint8_t padre;           /* puerto hacia el coordinador */
    uint8_t saltos;          /* distancia al coordinador     */
    uint32_t t_padre_ms;
    sx_rx_t rx[SX_N_PUERTOS];
    uint16_t dup_o[32]; uint8_t dup_i;   /* (origen<<8 | seq) recientes */
    uint32_t rx_ok, rx_crc, reenviadas, descartadas;
    uint8_t vecino[SX_N_PUERTOS];        /* último ID oído en cada puerto (0 = nadie) */
} sx_red_t;

uint16_t sx_crc16(const uint8_t *p, uint16_t n);
void sx_red_iniciar(sx_red_t *r, uint8_t id);
/* Envía una trama nueva originada aquí. Devuelve 1 si se encoló en algún puerto. */
uint8_t sx_red_enviar(sx_red_t *r, uint8_t tipo, uint8_t destino, const void *carga, uint8_t n);
/* Lee bytes de los puertos, reenvía lo que no es para mí y entrega lo que sí.
 * Devuelve 1 y llena *t cuando hay una trama para esta tarjeta. */
uint8_t sx_red_recibir(sx_red_t *r, sx_trama_t *t, uint32_t ahora_ms);

#endif
