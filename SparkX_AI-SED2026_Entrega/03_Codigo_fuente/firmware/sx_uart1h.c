/* sx_uart1h.c — ver sx_uart1h.h */
#include "sx_uart1h.h"
#include "sx_placa.h"
#include <string.h>

#define OS        SX_SOBREMUESTREO
#define BARRERA() __asm__ volatile("" ::: "memory")   /* el ISR y el lazo comparten las colas */
#define LBT_TICKS (20u * OS)          /* 2 bytes en reposo antes de transmitir */

static uint32_t azar(u1h_t *u)
{
    uint32_t x = u->lfsr;             /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    u->lfsr = x;
    return x;
}

void u1h_iniciar(u1h_t *u, uint32_t semilla)
{
    memset(u, 0, sizeof(*u));
    u->salida = 1;
    u->linea_prev = 1;
    u->lfsr = semilla ? semilla : 0x1234567u;
    u->espera = (uint16_t)(azar(u) % (16u * OS));
}

uint8_t u1h_encolar(u1h_t *u, const uint8_t *t, uint8_t n)
{
    if (n == 0 || n > U1H_TRAMA_MAX) return 0;
    uint8_t sig = (uint8_t)((u->q_w + 1u) % U1H_COLA);
    if (sig == u->q_r) { u->desbordes++; return 0; }
    memcpy(u->cola[u->q_w], t, n);
    u->cola_n[u->q_w] = n;
    BARRERA();
    u->q_w = sig;
    return 1;
}

int u1h_leer(u1h_t *u)
{
    if (u->rx_r == u->rx_w) return -1;
    BARRERA();
    uint8_t b = u->rxb[u->rx_r];
    u->rx_r = (uint8_t)(u->rx_r + 1u);
    return b;
}

static void cargar_byte(u1h_t *u)
{
    uint8_t b = u->cola[u->q_r][u->tx_pos];
    u->tx_sh = (uint16_t)((1u << 9) | ((uint16_t)b << 1));   /* stop | dato | start(0) */
    u->tx_bit = 0;
    u->tx_cnt = 0;
}

uint8_t u1h_tick(u1h_t *u, uint8_t linea)
{
    /* ---------------- transmisión ---------------- */
    if (u->tx_activo) {
        uint8_t bit = (uint8_t)((u->tx_sh >> u->tx_bit) & 1u);
        /* colisión: a mitad de un bit en 1, la línea está en 0 */
        if (bit && u->tx_cnt == OS / 2 && !linea) {
            u->colisiones++;
            u->tx_activo = 0;
            u->salida = 1;
            u->inactivo = 0;
            u->espera = (uint16_t)(OS * (20u + azar(u) % 60u));
            u->linea_prev = linea;
            return 1;
        }
        if (++u->tx_cnt >= OS) {
            u->tx_cnt = 0;
            if (++u->tx_bit >= 10) {
                u->bytes_tx++;
                if (++u->tx_pos >= u->cola_n[u->q_r]) {        /* trama completa */
                    u->q_r = (uint8_t)((u->q_r + 1u) % U1H_COLA);
                    u->tx_activo = 0;
                    u->salida = 1;
                    u->inactivo = 0;
                    u->espera = (uint16_t)(azar(u) % (8u * OS));
                    u->linea_prev = 1;
                    return 1;
                }
                cargar_byte(u);
            }
        }
        u->salida = (uint8_t)((u->tx_sh >> u->tx_bit) & 1u);
        return u->salida;
    }

    /* ---------------- recepción ---------------- */
    if (u->rx_est == 0) {
        if (u->linea_prev && !linea) {        /* flanco de bajada: posible bit de inicio */
            u->rx_est = 1; u->rx_cnt = 0; u->rx_nbit = 0; u->rx_dato = 0;
        }
    } else {
        u->rx_cnt++;
        if (u->rx_nbit == 0) {
            if (u->rx_cnt == OS / 2 - 1) {        /* centro del bit de inicio (el flanco se ve ~0.5 tick tarde) */
                if (linea) u->rx_est = 0;         /* ruido: no era inicio     */
                else { u->rx_nbit = 1; u->rx_cnt = 0; }
            }
        } else if (u->rx_cnt == OS) {             /* centro de los bits siguientes */
            u->rx_cnt = 0;
            if (u->rx_nbit <= 8) {
                u->rx_dato = (uint8_t)((u->rx_dato >> 1) | (linea ? 0x80u : 0u));
                u->rx_nbit++;
            } else {                              /* bit de paro */
                if (linea) {
                    uint8_t sig = (uint8_t)(u->rx_w + 1u);
                    if (sig != u->rx_r) { u->rxb[u->rx_w] = u->rx_dato; BARRERA(); u->rx_w = sig; u->bytes_rx++; }
                    else u->desbordes++;
                } else {
                    u->err_marco++;
                }
                u->rx_est = 0;
            }
        }
    }

    /* reposo de la línea y arranque de una transmisión */
    if (linea && u->rx_est == 0) { if (u->inactivo < 0xFFFFu) u->inactivo++; }
    else u->inactivo = 0;
    u->linea_prev = linea;

    if (u->q_r != u->q_w && u->rx_est == 0 && u->inactivo >= LBT_TICKS + u->espera) {
        u->tx_activo = 1;
        u->tx_pos = 0;
        cargar_byte(u);
        u->salida = 0;                            /* bit de inicio */
        return 0;
    }
    u->salida = 1;
    return 1;
}
