/* =========================================================================
 * sx_hal_stm32.c — HAL de Spark-X para el STM32F411RC, por registros (CMSIS).
 *
 *  Reloj    HSE 8 MHz si arranca (recomendado para los enlaces), si no HSI 16 MHz;
 *           PLL a 84 MHz. SysTick a 1 kHz.
 *  TIM2     interrupción a 4800 x 8 = 38.4 kHz: UART de 1 hilo de los 4 enlaces
 *           y secuencia trifásica de 6 pasos del EDS (frecuencia exacta).
 *  ADC1     conversión simple, 480 ciclos de muestreo (divisores de alta impedancia).
 *  USART1   PA9/PA10 a 115200: gateway del coordinador / consola de cada nodo.
 *  I2C      SHT40 por software en PB6 (SCL) / PB7 (SDA).
 *  Flash    configuración en el sector 5 (0x08020000).
 *  IWDG     ~8 s.
 *
 * Con -DSX_MODO_DEMO=1 los sensores salen de sx_demo_datos.h (Proteus sin sensores).
 * ========================================================================= */
#include "stm32f4xx.h"
#include "sx_hal.h"
#include "sx_placa.h"
#include "sx_uart1h.h"
#include "sx_adquisicion.h"
#include "sx_red.h"
#include <string.h>
#include <math.h>
#include <stddef.h>

#ifndef SX_MODO_DEMO
#define SX_MODO_DEMO 0
#endif

#define F_CPU     84000000u
#define F_TICK    (SX_BAUD_ENLACE * SX_SOBREMUESTREO)

/* ------------------------------------------------------------------ pines */
typedef struct { GPIO_TypeDef *p; uint8_t n; } pin_t;

static const pin_t SAL[SX_PIN_N] = {
    [SX_PIN_SECURE_MEASURE] = {GPIOB, 1},  [SX_PIN_ON_OFF_FLYBACK] = {GPIOB, 4},
    [SX_PIN_ACTIVE_V]       = {GPIOB, 0},  [SX_PIN_ACTIVE_C]       = {GPIOB, 3},
    [SX_PIN_PHASE_A]        = {GPIOA, 2},  [SX_PIN_PHASE_B]        = {GPIOA, 3},
    [SX_PIN_PHASE_C]        = {GPIOA, 4},  [SX_PIN_CARGA_PRUEBA]   = {GPIOB, 5},
    [SX_PIN_MINI_EDS_REF]   = {GPIOB, 8},  [SX_PIN_LED]            = {GPIOC, 13},
};
/* enlaces: 0=UP PB9, 1=RIGHT PB13, 2=DOWN PC6, 3=LEFT PC10 */
static const pin_t ENL[SX_N_PUERTOS] = { {GPIOB, 9}, {GPIOB, 13}, {GPIOC, 6}, {GPIOC, 10} };
__attribute__((unused)) static const uint8_t CANAL_ADC[SX_ADC_N] = {
    [SX_ADC_HV_MON] = 0, [SX_ADC_I_AT] = 1, [SX_ADC_NTC] = 5, [SX_ADC_ISC_S] = 6,
    [SX_ADC_ISC_L] = 7, [SX_ADC_V12] = 10, [SX_ADC_I12] = 11,
};

static void modo(GPIO_TypeDef *g, uint8_t n, uint32_t m)      /* 0 in, 1 out, 2 AF, 3 analógico */
{ g->MODER = (g->MODER & ~(3u << (2 * n))) | (m << (2 * n)); }
static void pull(GPIO_TypeDef *g, uint8_t n, uint32_t m)      /* 0 nada, 1 arriba */
{ g->PUPDR = (g->PUPDR & ~(3u << (2 * n))) | (m << (2 * n)); }
static void af(GPIO_TypeDef *g, uint8_t n, uint32_t a)
{ volatile uint32_t *r = &g->AFR[n >> 3]; uint8_t s = (uint8_t)(4 * (n & 7)); *r = (*r & ~(0xFu << s)) | (a << s); }
static inline void escribir(const pin_t *p, uint8_t v) { p->p->BSRR = v ? (1u << p->n) : (1u << (p->n + 16)); }
static inline uint8_t leer(const pin_t *p) { return (uint8_t)((p->p->IDR >> p->n) & 1u); }

/* ------------------------------------------------------------------ tiempo */
static volatile uint32_t ms;
void SysTick_Handler(void) { ms++; }
uint32_t sx_hal_ms(void) { return ms; }
void sx_hal_delay_ms(uint32_t d) { uint32_t t = ms; while (ms - t < d) { } }
void sx_hal_wdt(void) { IWDG->KR = 0xAAAA; }

static uint32_t semilla = 0x2545F491u;
uint32_t sx_hal_aleatorio(void) { uint32_t x = semilla; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return semilla = x; }
uint32_t sx_hal_uid(void)
{
    const uint32_t *u = (const uint32_t *)UID_BASE;
    return u[0] ^ (u[1] * 2654435761u) ^ (u[2] * 40503u);
}

/* ------------------------------------------------------------------ reloj */
static void reloj(void)
{
    RCC->CR |= RCC_CR_HSEON;
    uint32_t t = 0;
    while (!(RCC->CR & RCC_CR_HSERDY) && ++t < 200000u) { }
    uint8_t hse = (RCC->CR & RCC_CR_HSERDY) != 0;
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    FLASH->ACR = FLASH_ACR_LATENCY_2WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    /* VCO = entrada/M * 336 = 336 MHz ; SYSCLK = VCO/4 = 84 MHz ; USB/SDIO = VCO/7 = 48 MHz */
    uint32_t m = hse ? 8u : 16u;
    RCC->PLLCFGR = (m << RCC_PLLCFGR_PLLM_Pos) | (336u << RCC_PLLCFGR_PLLN_Pos) | (1u << RCC_PLLCFGR_PLLP_Pos) |
                   (7u << RCC_PLLCFGR_PLLQ_Pos) | (hse ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI);
    if (!hse) RCC->CR &= ~RCC_CR_HSEON;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }
}

/* ------------------------------------------------------------------ USART1 con buffers */
static volatile uint8_t gtx[2048]; static volatile uint16_t gtx_w, gtx_r;
static volatile uint8_t grx[128];  static volatile uint8_t grx_w, grx_r;

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;
    if (sr & USART_SR_RXNE) { uint8_t c = (uint8_t)USART1->DR; uint8_t s = (uint8_t)((grx_w + 1u) & 127u); if (s != grx_r) { grx[grx_w] = c; grx_w = s; } }
    if ((sr & USART_SR_TXE) && (USART1->CR1 & USART_CR1_TXEIE)) {
        if (gtx_r != gtx_w) { USART1->DR = gtx[gtx_r]; gtx_r = (uint16_t)((gtx_r + 1u) & 2047u); }
        else USART1->CR1 &= ~USART_CR1_TXEIE;
    }
}

void sx_hal_gateway_tx(const char *s)
{
    while (*s) {
        uint16_t sig = (uint16_t)((gtx_w + 1u) & 2047u);
        if (sig == gtx_r) break;                 /* buffer lleno: se pierde el resto (no bloquear) */
        gtx[gtx_w] = (uint8_t)*s++;
        gtx_w = sig;
    }
    USART1->CR1 |= USART_CR1_TXEIE;
}
int sx_hal_gateway_rx(void)
{
    if (grx_r == grx_w) return -1;
    uint8_t c = grx[grx_r]; grx_r = (uint8_t)((grx_r + 1u) & 127u);
    return c;
}

/* ------------------------------------------------------------------ TIM2: enlaces y fases */
static u1h_t enl[SX_N_PUERTOS];
static volatile uint8_t fases_on;
static volatile uint16_t fases_ticks_paso, fases_cnt;
static volatile uint8_t fases_paso;
static volatile uint32_t fases_ciclos;
static const uint8_t SEC[6][3] = {{1,0,1},{1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1}};

void TIM2_IRQHandler(void)
{
    TIM2->SR = ~TIM_SR_UIF;
    for (uint8_t p = 0; p < SX_N_PUERTOS; p++) {
        uint8_t o = u1h_tick(&enl[p], leer(&ENL[p]));
        escribir(&ENL[p], o);                    /* drenador abierto: 1 = soltar */
    }
    if (fases_on && ++fases_cnt >= fases_ticks_paso) {
        fases_cnt = 0;
        if (++fases_paso >= 6) { fases_paso = 0; fases_ciclos++; }
        for (uint8_t f = 0; f < 3; f++) {
            uint8_t v = SEC[fases_paso][f] ? SX_FASE_ACTIVA : !SX_FASE_ACTIVA;
            escribir(&SAL[SX_PIN_PHASE_A + f], v);
        }
    }
}

void sx_hal_fases(uint8_t on, float hz)
{
    __disable_irq();
    if (on && hz > 0.5f) {
        float t = (float)F_TICK / (6.0f * hz);
        fases_ticks_paso = (uint16_t)(t < 1.0f ? 1.0f : t > 65000.0f ? 65000.0f : t);
        fases_cnt = 0; fases_paso = 5; fases_ciclos = 0;
        fases_on = 1;
    } else {
        fases_on = 0;
        for (uint8_t f = 0; f < 3; f++) escribir(&SAL[SX_PIN_PHASE_A + f], !SX_FASE_ACTIVA);
    }
    __enable_irq();
}
uint32_t sx_hal_fases_ciclos(void) { return fases_ciclos; }

uint8_t sx_hal_enlace_enviar(uint8_t p, const uint8_t *b, uint8_t n) { return p < SX_N_PUERTOS ? u1h_encolar(&enl[p], b, n) : 0; }
int sx_hal_enlace_leer(uint8_t p) { return p < SX_N_PUERTOS ? u1h_leer(&enl[p]) : -1; }

/* ------------------------------------------------------------------ GPIO / ADC */
#if SX_MODO_DEMO
static void demo_gpio(uint8_t p, uint8_t v);
static uint16_t demo_adc(uint8_t c);
#endif

void sx_hal_gpio(uint8_t p, uint8_t v)
{
    if (p >= SX_PIN_N) return;
#if SX_MODO_DEMO
    demo_gpio(p, v);
#endif
    escribir(&SAL[p], v);
}

static uint16_t adc_una(uint8_t ch)
{
    ADC1->SQR3 = ch;
    ADC1->SR = 0;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    uint32_t t = 0;
    while (!(ADC1->SR & ADC_SR_EOC) && ++t < 100000u) { }
    return (uint16_t)ADC1->DR;
}

uint16_t sx_hal_adc(uint8_t canal, uint8_t prom)
{
    if (canal >= SX_ADC_N) return 0;
#if SX_MODO_DEMO
    (void)prom; (void)adc_una;
    return demo_adc(canal);
#else
    if (prom == 0) prom = 1;
    uint32_t s = 0;
    for (uint8_t i = 0; i < prom; i++) s += adc_una(CANAL_ADC[canal]);
    return (uint16_t)((s + prom / 2u) / prom);
#endif
}

/* ------------------------------------------------------------------ SHT40 por I2C de software */
#define SCL_P GPIOB
#define SCL_N 6
#define SDA_N 7
static void i2c_esp(void) { for (volatile int i = 0; i < 60; i++) { } }          /* ~100 kHz a 84 MHz */
static void scl(uint8_t v) { SCL_P->BSRR = v ? (1u << SCL_N) : (1u << (SCL_N + 16)); i2c_esp(); }
static void sda(uint8_t v) { SCL_P->BSRR = v ? (1u << SDA_N) : (1u << (SDA_N + 16)); i2c_esp(); }
static uint8_t sda_lee(void) { return (uint8_t)((SCL_P->IDR >> SDA_N) & 1u); }
__attribute__((unused)) static void i2c_ini(void) { sda(1); scl(1); sda(0); scl(0); }
__attribute__((unused)) static void i2c_fin(void) { sda(0); scl(1); sda(1); }
__attribute__((unused)) static uint8_t i2c_tx(uint8_t b)
{
    for (int i = 7; i >= 0; i--) { sda((b >> i) & 1u); scl(1); scl(0); }
    sda(1); scl(1); uint8_t nack = sda_lee(); scl(0);
    return nack;
}
__attribute__((unused)) static uint8_t i2c_rx(uint8_t ack)
{
    uint8_t b = 0; sda(1);
    for (int i = 0; i < 8; i++) { scl(1); b = (uint8_t)((b << 1) | sda_lee()); scl(0); }
    sda(!ack); scl(1); scl(0); sda(1);
    return b;
}
__attribute__((unused)) static uint8_t crc8(const uint8_t *d)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < 2; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1); }
    return c;
}

#if SX_MODO_DEMO
static int demo_sht40(float *t, float *h);
static int demo_afe(float *c, float *g);
#endif

int sx_hal_sht40(float *t_c, float *hr)
{
#if SX_MODO_DEMO
    return demo_sht40(t_c, hr);
#else
    uint8_t d[6];
    i2c_ini();
    if (i2c_tx(0x44 << 1)) { i2c_fin(); return -1; }
    i2c_tx(0xFD);                                 /* medición de alta repetibilidad */
    i2c_fin();
    sx_hal_delay_ms(10);
    i2c_ini();
    if (i2c_tx((0x44 << 1) | 1)) { i2c_fin(); return -2; }
    for (int i = 0; i < 6; i++) d[i] = i2c_rx(i < 5);
    i2c_fin();
    if (crc8(&d[0]) != d[2] || crc8(&d[3]) != d[5]) return -3;
    float rt = (float)((d[0] << 8) | d[1]), rh = (float)((d[3] << 8) | d[4]);
    *t_c = -45.0f + 175.0f * rt / 65535.0f;
    float h = -6.0f + 125.0f * rh / 65535.0f;
    *hr = h < 0.0f ? 0.0f : h > 100.0f ? 100.0f : h;
    return 0;
#endif
}

int sx_hal_afe(float *c_nf, float *g_us)
{
#if SX_MODO_DEMO
    return demo_afe(c_nf, g_us);
#else
    /* El AFE de impedancia todavía no existe en la PCB. Cuando se agregue, aquí va la
     * excitación de baja tensión entre fases y la detección síncrona (ver informe v2). */
    *c_nf = NAN; *g_us = NAN;
    return -1;
#endif
}

/* ------------------------------------------------------------------ flash (sector 5) */
#define CFG_DIR   0x08020000u
#define CFG_SECT  5u
#if SX_MODO_DEMO
static int demo_cfg(void *b, uint32_t n);
#endif
int sx_hal_cfg_leer(void *b, uint32_t n)
{
#if SX_MODO_DEMO
    return demo_cfg(b, n);                        /* demo: calibración de la tabla simulada */
#else
    memcpy(b, (const void *)CFG_DIR, n);
    return 0;
#endif
}

int sx_hal_cfg_escribir(const void *b, uint32_t n)
{
#if SX_MODO_DEMO
    (void)b; (void)n; return -1;
#else
    __disable_irq();
    FLASH->KEYR = 0x45670123u; FLASH->KEYR = 0xCDEF89ABu;
    while (FLASH->SR & FLASH_SR_BSY) { }
    FLASH->CR = FLASH_CR_PSIZE_1 | (CFG_SECT << FLASH_CR_SNB_Pos) | FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY) { IWDG->KR = 0xAAAA; }
    FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
    const uint32_t *s = (const uint32_t *)b;
    for (uint32_t i = 0; i < (n + 3u) / 4u; i++) {
        ((volatile uint32_t *)CFG_DIR)[i] = s[i];
        while (FLASH->SR & FLASH_SR_BSY) { }
    }
    FLASH->CR = FLASH_CR_LOCK;
    __enable_irq();
    return 0;
#endif
}

/* ------------------------------------------------------------------ arranque */
void sx_hal_iniciar(void)
{
    reloj();
    SysTick_Config(F_CPU / 1000u);
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN | RCC_APB2ENR_USART1EN;
    __DSB();

    /* salidas: todo apagado antes de poner los pines como salida (AT apagada) */
    for (uint8_t i = 0; i < SX_PIN_N; i++) {
        uint8_t v = (i >= SX_PIN_PHASE_A && i <= SX_PIN_PHASE_C) ? !SX_FASE_ACTIVA : 0;
        escribir(&SAL[i], v);
        modo(SAL[i].p, SAL[i].n, 1);
    }
    /* enlaces: drenador abierto + pull-up (poner además 4.7k externo y TVS en el conector) */
    for (uint8_t p = 0; p < SX_N_PUERTOS; p++) {
        escribir(&ENL[p], 1);
        ENL[p].p->OTYPER |= (1u << ENL[p].n);
        pull(ENL[p].p, ENL[p].n, 1);
        modo(ENL[p].p, ENL[p].n, 1);
        u1h_iniciar(&enl[p], sx_hal_uid() * (p + 1u) + 0x9E3779B9u);
    }
    /* I2C de software */
    GPIOB->OTYPER |= (1u << SCL_N) | (1u << SDA_N);
    pull(GPIOB, SCL_N, 1); pull(GPIOB, SDA_N, 1);
    GPIOB->BSRR = (1u << SCL_N) | (1u << SDA_N);
    modo(GPIOB, SCL_N, 1); modo(GPIOB, SDA_N, 1);
    /* analógicos */
    modo(GPIOA, 0, 3); modo(GPIOA, 1, 3); modo(GPIOA, 5, 3); modo(GPIOA, 6, 3); modo(GPIOA, 7, 3);
    modo(GPIOC, 0, 3); modo(GPIOC, 1, 3);
    ADC->CCR = ADC_CCR_ADCPRE_0;                  /* PCLK2/4 = 21 MHz */
    ADC1->SMPR2 = 0x3FFFFFFFu;                    /* 480 ciclos en los canales 0-9 */
    ADC1->SMPR1 = 0x07FFFFFFu;                    /* y 10-18 */
    ADC1->CR2 = ADC_CR2_ADON;
    /* USART1 PA9/PA10 */
    af(GPIOA, 9, 7); af(GPIOA, 10, 7); modo(GPIOA, 9, 2); modo(GPIOA, 10, 2); pull(GPIOA, 10, 1);
    USART1->BRR = (45u << 4) | 9u;                /* 84 MHz / (16 x 45.5625) = 115226 baud */
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_SetPriority(USART1_IRQn, 3);
    NVIC_EnableIRQ(USART1_IRQn);
    /* TIM2 a 38.4 kHz (reloj de TIM2 = 2 x PCLK1 = 84 MHz) */
    TIM2->PSC = 0;
    TIM2->ARR = (F_CPU + F_TICK / 2u) / F_TICK - 1u;
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->CR1 = TIM_CR1_CEN;
    NVIC_SetPriority(TIM2_IRQn, 0);
    NVIC_EnableIRQ(TIM2_IRQn);
    /* semilla y watchdog */
    semilla ^= sx_hal_uid();
    if (!semilla) semilla = 1;
    IWDG->KR = 0x5555; IWDG->PR = 4; IWDG->RLR = 4000; IWDG->KR = 0xCCCC;   /* 32 kHz/64 -> 8 s */
}

void sx_hal_aviso_paso(int32_t k);

/* ================================================================== MODO DEMO */
#if SX_MODO_DEMO
#include "sx_demo_datos.h"
#include "sx_adquisicion.h"

static int32_t demo_k;
static float demo_hv, demo_iat;
static uint32_t demo_t;
static uint8_t demo_pin[SX_PIN_N];

static const int16_t (*tabla(void))[7]
{
    return (SX_ID_DEFECTO == 3) ? SX_DEMO_B : SX_DEMO_A;
}
static const int16_t *fila(void) { return tabla()[demo_k]; }
static int agua(void) { return (float)fila()[6] / 1000.0f > 0.30f; }     /* fuga alta = agua */

#include <stdio.h>
void sx_hal_gateway_tx(const char *s);
void sx_hal_aviso_paso(int32_t k)
{
    static int32_t k_prev = -1000000;
    demo_k = (int32_t)(((k % SX_DEMO_PASOS) + SX_DEMO_PASOS) % SX_DEMO_PASOS);
#if !SX_TIENE_SHT40
    /* Coordinador sin SHT40: simula al SCADA mandando la estación de la planta por USART1
     * ("AMB ta hr tmod" del paso siguiente), igual que en la instalación real. */
    if (SX_ID_DEFECTO == SX_ID_COORD && k != k_prev) {
        k_prev = k;
        const int16_t *f = tabla()[(demo_k + 1) % SX_DEMO_PASOS];
        char l[48];
        snprintf(l, sizeof l, "AMB %.2f %.2f %.2f\n", f[0] / 100.0, f[1] / 100.0, f[2] / 100.0);
        for (const char *c = l; *c; c++) { uint8_t sg = (uint8_t)((grx_w + 1u) & 127u); if (sg != grx_r) { grx[grx_w] = (uint8_t)*c; grx_w = sg; } }
    }
#else
    (void)k_prev;
#endif
}

static void modelo_at(void)
{
    float dt = (float)(ms - demo_t); demo_t = ms;
    float obj, tau;
    if (demo_pin[SX_PIN_ON_OFF_FLYBACK]) { obj = agua() ? 550.0f : 1250.0f; demo_iat = agua() ? 3.0f : 0.15f; tau = 15.0f; }
    else { obj = 0.0f; demo_iat = 0.0f; tau = 60.0f; }
    demo_hv = obj + (demo_hv - obj) * expf(-dt / tau);
}
static void demo_gpio(uint8_t p, uint8_t v) { modelo_at(); demo_pin[p] = v; }
static uint16_t cod(float v) { float c = v / SX_VREF * SX_ADC_MAX + 0.5f; return (uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : c); }
static float ntc_v(float t)
{
    float r = 10000.0f * expf(3950.0f * (1.0f / (t + 273.15f) - 1.0f / 298.15f));
    return SX_VREF * r / (r + 10000.0f);
}
static uint16_t demo_adc(uint8_t c)
{
    const int16_t *f = fila();
    float ganancia = 1.0f + 0.002f * (float)((int)SX_ID_DEFECTO - 2);   /* pequeñas diferencias por tarjeta */
    const int16_t *pr = (SX_ID_DEFECTO == 3 ? SX_DEMO_B_PRUEBA : SX_DEMO_A_PRUEBA)[(demo_k / 96) % 28];
    modelo_at();
    switch (c) {
    case SX_ADC_HV_MON: return demo_pin[SX_PIN_ACTIVE_V] ? cod(demo_hv / SX_K_MEASURE_V) : 0;
    case SX_ADC_I_AT:   return demo_pin[SX_PIN_ACTIVE_C] ? cod(demo_iat / SX_K_MEASURE_C) : 0;
    case SX_ADC_NTC:    return cod(ntc_v((float)f[2] / 100.0f));
    case SX_ADC_ISC_S:  return cod((float)f[3] / 100.0f * ganancia / SX_K_ISC_MA);
    case SX_ADC_ISC_L:  return cod((float)f[4] / 100.0f / SX_K_ISC_MA);
    case SX_ADC_V12:    return cod((float)(demo_pin[SX_PIN_CARGA_PRUEBA] ? pr[1] : pr[0]) / 1000.0f / SX_K_V12);
    case SX_ADC_I12:    return cod((demo_pin[SX_PIN_CARGA_PRUEBA] ? (float)pr[2] / 1000.0f : 0.0f) / SX_K_I12);
    }
    return 0;
}
static int demo_sht40(float *t, float *h) { *t = (float)fila()[0] / 100.0f; *h = (float)fila()[1] / 100.0f; return 0; }
#include "sx_red.h"
#include <stddef.h>
static int demo_cfg(void *b, uint32_t n)
{
    sx_cfg_t c;
    memset(&c, 0, sizeof c);
    c.magia = SX_CFG_MAGIA; c.id = SX_ID_DEFECTO;
    c.tiene_sht40 = SX_TIENE_SHT40 && (SX_ID_DEFECTO == SX_ID_COORD);
    c.tiene_ntc = SX_TIENE_NTC; c.tiene_minic = SX_TIENE_MINICELDAS; c.tiene_afe = SX_TIENE_AFE;
    c.tiene_prueba = SX_TIENE_PRUEBA_CARGA; c.max_simult = SX_MAX_SIMULT_DEF;
    c.c_ref_nf = (SX_ID_DEFECTO == 3) ? SX_DEMO_B_C_REF : SX_DEMO_A_C_REF;
    c.k_sr = (SX_ID_DEFECTO == 3) ? SX_DEMO_B_K_SR : SX_DEMO_A_K_SR;
    c.crc = sx_crc16((const uint8_t *)&c, (uint16_t)offsetof(sx_cfg_t, crc));
    memcpy(b, &c, n < sizeof c ? n : sizeof c);
    return 0;
}
static int demo_afe(float *c, float *g) { *c = (float)fila()[5] / 10.0f; *g = (float)fila()[6] / 1000.0f; return 0; }
#else
void sx_hal_aviso_paso(int32_t k) { (void)k; }
#endif
