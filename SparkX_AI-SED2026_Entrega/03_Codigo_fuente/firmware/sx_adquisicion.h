#ifndef SX_ADQUISICION_H
#define SX_ADQUISICION_H
#include <stdint.h>
#include "sx_controlador.h"

typedef struct {             /* configuración de la tarjeta (se guarda en flash) */
    uint32_t magia;
    uint8_t  id;
    uint8_t  tiene_sht40, tiene_ntc, tiene_minic, tiene_afe, tiene_prueba;
    uint8_t  max_simult;
    uint8_t  reservado;
    float    c_ref_nf;        /* calibración de instalación (vidrio limpio y seco) */
    float    k_sr;
    uint32_t crc;
} sx_cfg_t;

#define SX_CFG_MAGIA 0x53583033u   /* "SX03" */

float sx_conv_hv(uint16_t c);      /* V de AT          */
float sx_conv_i_at(uint16_t c);    /* mA de AT         */
float sx_conv_ntc(uint16_t c);     /* °C               */
float sx_conv_isc_ma(uint16_t c);  /* mA mini-celda    */
float sx_conv_v12(uint16_t c);     /* V riel 12 V      */
float sx_conv_i12(uint16_t c);     /* A riel 12 V      */

float sx_leer_hv(void);            /* enciende ACTIVE_V, mide MEASURE_V */
float sx_leer_i_at(void);          /* enciende ACTIVE_C, mide MEASURE_C */

/* Lee los sensores LOCALES en la ventana segura (AT apagada).
 * Los que la tarjeta no tiene quedan en NAN (ambiente) o en valor neutro, y el
 * nodo los completa con lo que reparte el coordinador. */
int   sx_adquirir(const sx_cfg_t *cfg, sx_lecturas_t *L, uint8_t hacer_prueba_carga);

#endif
