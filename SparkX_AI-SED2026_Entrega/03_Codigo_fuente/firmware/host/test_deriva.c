/* test_deriva.c — tolerancia del UART de 1 hilo a la diferencia de reloj entre tarjetas.
 * Sin cristal (HSI) el STM32F411 puede desviarse varios % con la temperatura. */
#include "../sx_uart1h.h"
#include "../sx_placa.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    printf("diferencia de reloj   tramas OK (de 200, 40 bytes c/u)\n");
    for (int pm = -70; pm <= 70; pm += 10) {
        double e = pm / 1000.0;
        int ok = 0;
        for (int f = 0; f < 200; f++) {
            u1h_t tx, rx; u1h_iniciar(&tx, 7 + f); u1h_iniciar(&rx, 99 + f);
            uint8_t m[40]; for (int i = 0; i < 40; i++) m[i] = (uint8_t)(f * 31 + i * 17);
            u1h_encolar(&tx, m, 40);
            double ttx = 0, trx = 0, dtx = 1.0, drx = 1.0 / (1.0 + e);
            uint8_t lin = 1; int n = 0; uint8_t got[64];
            while (ttx < 40 * 10 * SX_SOBREMUESTREO + 2000) {
                if (ttx <= trx) { lin = u1h_tick(&tx, lin); ttx += dtx; }
                else { u1h_tick(&rx, lin); trx += drx; }
            }
            int c; while ((c = u1h_leer(&rx)) >= 0 && n < 64) got[n++] = (uint8_t)c;
            ok += (n == 40 && memcmp(got, m, 40) == 0);
        }
        printf("      %4.1f %%            %3d\n", pm / 10.0, ok);
    }
    return 0;
}
