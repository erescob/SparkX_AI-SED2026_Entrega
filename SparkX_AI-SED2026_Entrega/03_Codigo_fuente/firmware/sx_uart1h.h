/* =========================================================================
 * sx_uart1h.h — UART por software de UN SOLO HILO (half-duplex) por puerto.
 *
 * Cada enlace UP/RIGHT/DOWN/LEFT es un solo cable entre dos tarjetas: drenador
 * abierto con pull-up, reposo en 1. Formato 8N1. La misma máquina de estados
 * corre en el STM32 (interrupción del TIM2 a BAUD x SOBREMUESTREO) y en la PC.
 *
 *  - Escuchar antes de hablar: solo transmite si la línea lleva >= 2 bytes en reposo
 *    más una espera aleatoria (evita que las dos tarjetas empiecen juntas).
 *  - Detección de colisión: si suelto la línea (1) y la leo en 0, alguien más está
 *    hablando: corto, la trama se queda en la cola y reintento más tarde.
 *  - No escucha su propio eco.
 * ========================================================================= */
#ifndef SX_UART1H_H
#define SX_UART1H_H
#include <stdint.h>

#define U1H_TRAMA_MAX  64
#define U1H_COLA       8          /* tramas en espera por puerto */

typedef struct {
    /* recepción */
    uint8_t  rx_est, rx_cnt, rx_nbit, rx_dato;
    uint8_t  rxb[256];
    volatile uint8_t rx_w, rx_r;
    /* transmisión */
    uint8_t  cola[U1H_COLA][U1H_TRAMA_MAX];
    uint8_t  cola_n[U1H_COLA];
    volatile uint8_t q_w, q_r;
    uint8_t  tx_activo, tx_pos, tx_bit, tx_cnt, salida;
    uint16_t tx_sh;
    uint16_t inactivo, espera;
    uint8_t  linea_prev;
    uint32_t lfsr;
    /* estadística */
    uint16_t err_marco, colisiones, desbordes;
    uint32_t bytes_rx, bytes_tx;
} u1h_t;

void    u1h_iniciar(u1h_t *u, uint32_t semilla);
/* Llamar una vez por tick con el nivel leído de la línea. Devuelve el nivel a escribir
 * (1 = soltar / alta impedancia, 0 = llevar a tierra). */
uint8_t u1h_tick(u1h_t *u, uint8_t linea);
uint8_t u1h_encolar(u1h_t *u, const uint8_t *trama, uint8_t n);   /* 1 = aceptada */
int     u1h_leer(u1h_t *u);                                      /* byte o -1     */

#endif
