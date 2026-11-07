/* =========================================================================
 * sx_placa.h — Todo lo que depende de la PCB, en un solo lugar.
 *
 * Mapa de pines LEÍDO del esquemático final (SISTEMA_PCB,
 * U9 = STM32F411RC, 02-nov-2026). Los marcados NUEVO no existen todavía en la
 * PCB: son los pines libres propuestos para el hardware que pide el v2.
 *
 * Etiqueta del esquemático   Pin    Uso en el firmware
 * -------------------------  -----  ----------------------------------------
 * MEASURE_V                  PA0    ADC1_IN0  monitor de alta tensión  (ver nota *)
 * MEASURE_C                  PA1    ADC1_IN1  sin uso hasta que haya sensor de corriente (ver nota *)
 * PHASE_A / B / C            PA2-4  compuertas de las 3 fases del EDS
 * ACTIVE_V                   PB0    habilita el canal de medición de voltaje
 * SECURE_MEASURE             PB1    PhotoMOS U6/U7: conecta la medición a los electrodos
 * ACTIVE_C                   PB3    habilita el canal de medición de corriente
 * ON/OFF_FLYBACK             PB4    PhotoMOS U5: 12 V al flyback (UC3843)
 * UP_PANEL                   PB9    enlace con la tarjeta de ARRIBA   (1 hilo)
 * RIGHT_PANEL                PB13   enlace con la tarjeta de la DERECHA
 * DOWN_PANEL                 PC6    enlace con la tarjeta de ABAJO
 * LEFT_PANEL                 PC10   enlace con la tarjeta de la IZQUIERDA
 * NRST (MCLR)                NRST   botón S3
 * --- NUEVO (propuesto) ---
 * SWDIO / SWCLK              PA13/PA14  programación (hoy J2 es un conector de PIC)
 * NTC del vidrio             PA5    ADC1_IN5
 * mini-celda sucia / limpia  PA6/PA7 ADC1_IN6 / IN7
 * riel 12 V / corriente 12 V PC0/PC1 ADC1_IN10 / IN11
 * SHT40 (SCL / SDA)          PB6/PB7 I2C por software
 * gateway (solo coordinador) PA9/PA10 USART1 TX/RX -> RS-485 / ESP32 / USB-serie
 * carga de prueba 1 A        PB5
 * mini-EDS de referencia     PB8
 * LED de estado              PC13
 *
 * (*) En el esquemático del 02-nov las etiquetas MEASURE_V y MEASURE_C están en el
 *     MISMO nodo que GND (en -VIN de U8 y U4): el ADC lee 0 V siempre. Arreglo sin
 *     componentes: mover MEASURE_V al nodo R25.2 / U1.K (parte baja del divisor de AT:
 *     8.8 MΩ + 18.5 kΩ -> 2.52 V con 1.2 kV) y quitar MEASURE_C de tierra.
 * ========================================================================= */
#ifndef SX_PLACA_H
#define SX_PLACA_H

/* ---------- Identidad y red ---------------------------------------------- */
#ifndef SX_ID_DEFECTO
#define SX_ID_DEFECTO      1        /* se sobreescribe con -DSX_ID_DEFECTO=n o desde flash */
#endif
#define SX_ID_COORD        1        /* el nodo 1 es el coordinador del arreglo              */
#define SX_MAX_NODOS       32
#define SX_N_PUERTOS       4        /* 0=UP 1=RIGHT 2=DOWN 3=LEFT                           */
#define SX_BAUD_ENLACE     4800     /* enlaces de 1 hilo entre tarjetas                     */
#define SX_SOBREMUESTREO   8        /* ticks por bit -> interrupción a 38.4 kHz             */

/* ---------- Tiempo -------------------------------------------------------- */
#ifndef SX_PASO_MS
#define SX_PASO_MS         900000u  /* 15 min. En el modo demo se acelera (ver Makefile)   */
#endif

#ifndef SX_K0
#define SX_K0              0        /* paso con el que arranca el coordinador (demo: un día con rocío) */
#endif

/* ---------- Hardware presente (1 = montado en esta tarjeta) --------------- *
 * La PCB del 02-nov NO tiene ninguno de estos y el firmware POR DEFECTO funciona
 * así (todo en 0). Cada respaldo se reporta en la telemetría (bits SX_F_SIN_*).  */
#ifndef SX_TIENE_SHT40
#define SX_TIENE_SHT40     0        /* 0: nodos -> ambiente del coordinador; coordinador -> estación de la planta ("AMB" por USART1) */
#endif
#ifndef SX_TIENE_NTC
#define SX_TIENE_NTC       0        /* 0: T de módulo de la estación, repartida por el coordinador */
#endif
#ifndef SX_TIENE_MINICELDAS
#define SX_TIENE_MINICELDAS 0       /* 0: sin polvo medido -> 1 limpieza diaria + limpieza antes del rocío que predice la IA */
#endif
#ifndef SX_TIENE_AFE
#define SX_TIENE_AFE       0        /* 0: exige 1 h de vidrio claramente seco + 2a barrera (MEASURE_V/C) */
#endif
#ifndef SX_TIENE_PRUEBA_CARGA
#define SX_TIENE_PRUEBA_CARGA 0     /* 0: sin alerta de conector                            */
#endif

/* ---------- Conversiones analógicas [CALIBRAR en banco] ------------------- */
#ifndef SX_VREF
#define SX_VREF            3.5f     /* VDDA = riel +3.5 V del MC34063 (VDDA hoy NO está conectado) */
#endif
#define SX_ADC_MAX         4095.0f
/* MEASURE_V en el nodo R25.2: (R29+R31+R30+R24 + R25) / R25 = (4 x 2.2M + 18.5k) / 18.5k */
#define SX_K_MEASURE_V     476.7f   /* V de AT por cada V en PA0 [CALIBRAR con multímetro de AT] */
#define SX_K_MEASURE_C     1.0f     /* mA de AT por cada V en PA1 (solo si SX_TIENE_I_AT)    */
#ifndef SX_TIENE_I_AT
#define SX_TIENE_I_AT      0        /* 0: la PCB no mide corriente de AT; la 2a barrera usa la caída de AT */
#endif
#define SX_K_V12           5.7f     /* divisor 47k/10k                                      */
#define SX_K_I12           (1.0f / 2.5f)   /* A por V: shunt 50 mOhm x50                    */
#define SX_K_ISC_MA        (1.0f / 0.05f)  /* mA por V: shunt 10 Ohm x5                     */

/* ---------- EDS ----------------------------------------------------------- */
#define SX_EDS_HZ          50.0f    /* revisar contra tau = 450k * C_electrodos (README)    */
#ifndef SX_EDS_CICLOS
#define SX_EDS_CICLOS      3000u    /* ~60 s a 50 Hz (en el modo demo se acorta)            */
#endif
#define SX_EDS_V_OBJETIVO  1200.0f
#define SX_EDS_FRAC_MIN    0.70f    /* abortar si la AT cae bajo 70 % del objetivo          */
#define SX_I_FUGA_MAX_MA   1.0f     /* abortar si la corriente de AT supera esto [CALIBRAR] */
#define SX_V_SEGURA        50.0f    /* no conectar la medición con más de 50 V              */
/* PHASE_x = 1 -> Q7/Q2/Q4 conducen -> Q1/Q3/Q5 se apagan -> el electrodo sube a +1.2 kV por
 * R36/R10/R13. PHASE_x = 0 -> electrodo a tierra (reposo y descarga). */
#define SX_FASE_ACTIVA     1        /* nivel del GPIO que pone el electrodo en ALTA tensión */

/* ---------- Coordinación del arreglo ------------------------------------- */
#define SX_MAX_SIMULT_DEF  1        /* limpiezas a la vez (presupuesto de la fuente de 12 V) */
#define SX_PASOS_AMB_MAX   2        /* ambiente válido hasta 2 pasos (30 min) sin noticias  */
#define SX_LIMPIEZA_SIN_COORD 0     /* 0: sin coordinador no se limpia                      */

#endif
