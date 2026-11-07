/* =========================================================================
 * sx_hal.h — Capa de hardware. Dos implementaciones:
 *   stm32/sx_hal_stm32.c  -> STM32F411RC (registros, sin HAL de ST)
 *   host/sx_hal_host.c    -> PC: varias tarjetas simuladas con sus enlaces
 * El resto del firmware (núcleo v2, adquisición, EDS, red) es el mismo en ambos.
 * ========================================================================= */
#ifndef SX_HAL_H
#define SX_HAL_H
#include <stdint.h>

enum {                       /* canales analógicos                                   */
    SX_ADC_HV_MON = 0,       /* PA0 MEASURE_V (requiere ACTIVE_V = 1)                */
    SX_ADC_I_AT,             /* PA1 MEASURE_C (requiere ACTIVE_C = 1)                */
    SX_ADC_NTC,              /* PA5 NUEVO                                            */
    SX_ADC_ISC_S,            /* PA6 NUEVO                                            */
    SX_ADC_ISC_L,            /* PA7 NUEVO                                            */
    SX_ADC_V12,              /* PC0 NUEVO                                            */
    SX_ADC_I12,              /* PC1 NUEVO                                            */
    SX_ADC_N
};

enum {                       /* salidas digitales                                    */
    SX_PIN_SECURE_MEASURE = 0, SX_PIN_ON_OFF_FLYBACK, SX_PIN_ACTIVE_V, SX_PIN_ACTIVE_C,
    SX_PIN_PHASE_A, SX_PIN_PHASE_B, SX_PIN_PHASE_C,
    SX_PIN_CARGA_PRUEBA, SX_PIN_MINI_EDS_REF, SX_PIN_LED,
    SX_PIN_N
};

void     sx_hal_iniciar(void);
uint32_t sx_hal_ms(void);                       /* milisegundos desde el arranque      */
void     sx_hal_delay_ms(uint32_t ms);          /* solo esperas cortas (< 50 ms)        */
void     sx_hal_wdt(void);                      /* refrescar el watchdog                */
uint32_t sx_hal_aleatorio(void);

uint16_t sx_hal_adc(uint8_t canal, uint8_t promedios);
void     sx_hal_gpio(uint8_t pin, uint8_t valor);

int      sx_hal_sht40(float *t_c, float *hr_pct);      /* 0 = OK                       */
int      sx_hal_afe(float *c_nf, float *g_us);         /* 0 = OK (solo con SX_TIENE_AFE) */

/* Secuencia trifásica de 6 pasos generada por el timer (no bloquea).                  */
void     sx_hal_fases(uint8_t encender, float hz);
uint32_t sx_hal_fases_ciclos(void);             /* ciclos eléctricos completos          */

/* Enlaces de 1 hilo con las tarjetas vecinas (puerto 0..3 = UP, RIGHT, DOWN, LEFT).    */
uint8_t  sx_hal_enlace_enviar(uint8_t puerto, const uint8_t *trama, uint8_t n);   /* 1 = en cola */
int      sx_hal_enlace_leer(uint8_t puerto);    /* byte recibido o -1                   */

/* Puerto del gateway (USART1). En los nodos que no son coordinador es la consola.      */
void     sx_hal_gateway_tx(const char *s);
int      sx_hal_gateway_rx(void);               /* carácter o -1                        */

/* Configuración persistente (ID, calibración de instalación).                          */
int      sx_hal_cfg_leer(void *buf, uint32_t n);         /* 0 = OK                       */
int      sx_hal_cfg_escribir(const void *buf, uint32_t n);
uint32_t sx_hal_uid(void);
/* Aviso del paso k que se va a medir (modo demo / PC: elige el renglón de datos). */
void     sx_hal_aviso_paso(int32_t k);

#endif
