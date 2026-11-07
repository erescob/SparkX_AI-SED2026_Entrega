/* startup_stm32f411.c — tabla de vectores y arranque mínimo (C, sin ensamblador) */
#include <stdint.h>
#include "stm32f4xx.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
extern void __libc_init_array(void);

void Reset_Handler(void);
void Default_Handler(void) { for (;;) { } }
void HardFault_Handler(void) { for (;;) { } }   /* el IWDG reinicia la tarjeta */
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void)    __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void)  __attribute__((weak, alias("Default_Handler")));

/* 16 excepciones del núcleo + 86 interrupciones del STM32F411 */
__attribute__((section(".isr_vector"), used))
void (*const tabla_vectores[16 + 86])(void) = {
    (void (*)(void))(&_estack), Reset_Handler, NMI_Handler, HardFault_Handler,
    MemManage_Handler, BusFault_Handler, UsageFault_Handler, 0, 0, 0, 0,
    SVC_Handler, 0, 0, PendSV_Handler, SysTick_Handler,
    [16 + TIM2_IRQn] = TIM2_IRQHandler,
    [16 + USART1_IRQn] = USART1_IRQHandler,
};

void Reset_Handler(void)
{
    /* FPU antes de cualquier operación con float */
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    __DSB(); __ISB();
    uint32_t *s = &_sidata, *d = &_sdata;
    while (d < &_edata) *d++ = *s++;
    for (d = &_sbss; d < &_ebss; ) *d++ = 0;
    __libc_init_array();
    main();
    for (;;) { }
}

/* newlib-nano: no se usa memoria dinámica, pero printf la pide para el buffer de float */
extern char _end, _heap_limit;
void *_sbrk(int inc)
{
    static char *p = &_end;
    char *prev = p;
    if (p + inc > &_heap_limit) return (void *)-1;
    p += inc;
    return prev;
}

/* stubs de newlib (no hay sistema de archivos; la salida va por sx_hal_gateway_tx) */
void _init(void) { }
void _fini(void) { }
int _close(int f) { (void)f; return -1; }
int _lseek(int f, int p, int d) { (void)f; (void)p; (void)d; return 0; }
int _read(int f, char *p, int n) { (void)f; (void)p; (void)n; return 0; }
int _write(int f, const char *p, int n) { (void)f; (void)p; return n; }
int _isatty(int f) { (void)f; return 1; }
struct stat;
int _fstat(int f, struct stat *s) { (void)f; (void)s; return -1; }
int _getpid(void) { return 1; }
int _kill(int p, int s) { (void)p; (void)s; return -1; }
void _exit(int c) { (void)c; for (;;) { } }
