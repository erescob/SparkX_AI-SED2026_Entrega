/* test_eds.c — prueba de la 2a barrera: el ciclo EDS aborta si la AT no sube
 * (agua/fuga), si cae a media limpieza, o si la corriente de AT se dispara. */
#include "../sx_eds.h"
#include "../sx_hal.h"
#include "../sx_placa.h"
#include <stdio.h>
#include <math.h>
static uint32_t t; static uint8_t pin[SX_PIN_N], fon; static float hz; static uint32_t t_f;
static int escenario; static float hv; static uint32_t t_m;
uint32_t sx_hal_ms(void) { return t; }
void sx_hal_delay_ms(uint32_t d) { (void)d; }
static void modelo(void);
void sx_hal_gpio(uint8_t p, uint8_t v) { modelo(); pin[p] = v; }
void sx_hal_fases(uint8_t on, float h) { fon = on; hz = h; t_f = t; }
uint32_t sx_hal_fases_ciclos(void) { return fon ? (uint32_t)((t - t_f) * hz / 1000.0f) : 0; }
static void modelo(void)
{
    float obj = 0, i = 0;
    if (pin[SX_PIN_ON_OFF_FLYBACK]) {
        obj = 1250; i = 0.15f;
        if (escenario == 1) { obj = 550; i = 3; }                       /* agua desde el inicio */
        if (escenario == 2 && fon && t - t_f > 300) { obj = 600; i = 2; } /* rocío a media limpieza */
    }
    hv = obj + (hv - obj) * expf(-(float)(t - t_m) / 15.0f); t_m = t;
    if (escenario == 3 && pin[SX_PIN_ON_OFF_FLYBACK] && fon) i = 5;       /* fuga sin caída de AT */
    extern float g_i; g_i = i;
}
float g_i;
uint16_t sx_hal_adc(uint8_t c, uint8_t p)
{
    (void)p; modelo();
    float v = c == SX_ADC_HV_MON ? hv / SX_K_MEASURE_V : c == SX_ADC_I_AT ? g_i / SX_K_MEASURE_C : 0;
    if (c == SX_ADC_HV_MON && !pin[SX_PIN_ACTIVE_V]) v = 0;
    float k = v / SX_VREF * 4095.0f; return (uint16_t)(k > 4095 ? 4095 : k);
}
int main(void)
{
    printf("SX_TIENE_I_AT=%d\n", SX_TIENE_I_AT);
    const char *nom[] = {"normal", "agua al arrancar", "rocio a media limpieza", "fuga sin caida de AT"};
    int fallas = 0;
#if SX_TIENE_I_AT
    int esperado[] = {0, -1, -2, -2};
#else
    int esperado[] = {0, -1, -2, 0};   /* sin sensor de corriente, una fuga que no baja la AT no se ve */
#endif
    for (escenario = 0; escenario < 4; escenario++) {
        sx_eds_t e; t = 0; t_m = 0; hv = 0; fon = 0;
        for (int i = 0; i < SX_PIN_N; i++) pin[i] = 0;
        sx_eds_iniciar(&e, t, 100);
        while (sx_eds_tick(&e, t) && t < 20000) t++;
        int ok = e.rc == esperado[escenario] && !pin[SX_PIN_ON_OFF_FLYBACK] && !fon;
        fallas += !ok;
        printf("%-24s rc=%d (esperado %d) AT_final=%.0f V  flyback=%u fases=%u  %s\n", nom[escenario], e.rc,
               esperado[escenario], hv, pin[SX_PIN_ON_OFF_FLYBACK], fon, ok ? "OK" : "FALLA");
    }
    return fallas;
}
int sx_hal_sht40(float *a, float *b) { (void)a; (void)b; return -1; }
int sx_hal_afe(float *a, float *b) { (void)a; (void)b; return -1; }
