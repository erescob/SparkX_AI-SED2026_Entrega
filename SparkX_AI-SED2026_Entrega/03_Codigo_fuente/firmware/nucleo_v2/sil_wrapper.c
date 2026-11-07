/* sil_wrapper.c — Exporta tamaños para la prueba software-in-the-loop (ctypes) */
#include <stddef.h>
#include "sx_controlador.h"

size_t sx_tam_estado(void)   { return sizeof(sx_estado_t); }
size_t sx_tam_lecturas(void) { return sizeof(sx_lecturas_t); }
size_t sx_tam_salidas(void)  { return sizeof(sx_salidas_t); }
size_t sx_tam_params(void)   { return sizeof(sx_params_t); }
