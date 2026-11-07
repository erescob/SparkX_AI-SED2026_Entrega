/* main.c — firmware de cada tarjeta Spark-X (STM32F411RC). Igual en todas;
 * el ID (flash o -DSX_ID_DEFECTO) decide si además es el coordinador. */
#include "sx_hal.h"
#include "sx_nodo.h"

static sx_nodo_t nodo;

int main(void)
{
    sx_hal_iniciar();
    sx_nodo_iniciar(&nodo);
    for (;;) sx_nodo_ciclo(&nodo);
}
