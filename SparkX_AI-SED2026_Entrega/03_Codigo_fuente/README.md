# Spark-X red — firmware de todas las tarjetas del arreglo (STM32F411RC)

Cada panel solar lleva una PCB con un STM32F411RC (Cortex-M4F). Todas las tarjetas corren
**el mismo firmware**; la que tiene el ID 1 además es el **coordinador** del arreglo.
La IA y el mantenimiento son el **núcleo del v2 sin cambios** (`firmware/nucleo_v2/`,
verificable con `make nucleo`).

```
        [1]---[2]---[3]          cada tarjeta: sensores -> núcleo v2 (seguridad, IA de rocío,
         |     |     |           política, alertas) -> ciclo EDS
        [4]---[5]---[6]          coordinador (1): ambiente para todos, turnos de limpieza,
                                 telemetría JSON hacia el SCADA / PC por USART1
```

> **Antes de simular o fabricar, lean `REVISION_ESQUEMATICOS.md`.** Ahí está la revisión
> completa de los 6 esquemáticos. Lo mínimo, solo con cables: H1 (MEASURE_V está en tierra),
> H2 (LED del optoacoplador en corto), H3 (VDDA/VREF+/BOOT0) y H4 (SWD en J2).

## Funciona con la PCB actual, sin componentes nuevos

Por defecto (`sx_placa.h`, todo en 0) el firmware no pide ningún sensor nuevo:

- **Ambiente:** el coordinador recibe la estación meteorológica de la planta (T aire, HR,
  T de módulo) por USART1 con `AMB ta hr tmod` y la reparte a todo el arreglo.
- **IA:** el árbol de rocío del v2 corre en cada M4 con esos datos.
- **Política sin medición de polvo:** 1 limpieza diaria de 07:00 a 11:00, más una limpieza
  antes del rocío que predice la IA.
- **Seguridad sin AFE:** además de margen ≥ 2 °C y HR ≤ 85 %, el vidrio debe llevar 1 h
  claramente seco (margen ≥ 4 °C, HR ≤ 80 %). Hay una 2ª barrera con MEASURE_V: si la AT no
  sube o cae, se aborta.
- **Mantenimiento por panel:** la alerta REVISAR_EDS compara la AT mínima de cada limpieza
  contra su línea base.

Comparación en la simulación de 28 días con 6 tarjetas (`make sim`):

| | PCB actual | Con el hardware del v2 |
|---|---|---|
| Limpiezas por panel | ~53 (≈ 1.9/día) | 7–13 |
| ms de AT con agua en el vidrio | 0 | 0 |
| Limpiezas abortadas | 0 | 0 |
| Alertas por panel | REVISAR_EDS (nodo 2, día 18.7) | conector (n3), térmica (n4), REVISAR_EDS (n2) |

Lo que se pierde sin los sensores: se limpia ~4 veces más (sin medición de polvo no se sabe
cuándo hace falta), no hay alertas de conector ni térmica, y la seguridad depende de que la
estación represente bien el vidrio de cada panel. Es una sola corrida simulada, no una prueba.

## Qué hace cada pieza

| Archivo | Qué es | Flash | RAM |
|---|---|---|---|
| `nucleo_v2/` | Núcleo v2 **idéntico** al validado: seguridad, estimadores, árbol de rocío (1.75 KB), política, alertas | 5.9 KB | dentro de `sx_nodo_t` |
| `sx_nodo.c/.h` | Aplicación: paso de 15 min, ambiente compartido con caducidad, solicitud/turno/fin, coordinador, comandos | 6.8 KB | 3.5 KB |
| `sx_red.c/.h` | Tramas con CRC-16, inundación con filtro de duplicados, ruta al coordinador por "puerto padre" | 0.9 KB | — |
| `sx_uart1h.c/.h` | UART de 1 hilo por software (4 enlaces), escucha antes de hablar, detección de colisión | 0.7 KB | 3.3 KB |
| `sx_eds.c/.h` | Ciclo EDS como máquina de estados (no bloquea la red); 2ª barrera: aborta si la AT no sube o hay fuga | 0.6 KB | — |
| `sx_adquisicion.c/.h` | Sensores en unidades físicas, ventana segura con AT apagada | 0.8 KB | — |
| `sx_placa.h` | **Todo lo que depende de la PCB**: pines, escalas `[CALIBRAR]`, hardware presente | — | — |
| `stm32/` | HAL por registros (CMSIS), arranque, linker | 2.7 KB | 5.5 KB |
| `host/` | Simulación de 6 tarjetas en la PC y pruebas | (PC) | |

### Cabe en el M4

`make tamano` (firmware real, `-Os`):

| | Usado | Disponible en el STM32F411RC | Uso |
|---|---|---|---|
| Flash | 47.9 KB | 128 KB para el programa (los otros 128 KB guardan la configuración) | 37 % (18 % de los 256 KB) |
| RAM | 9.4 KB + 8 KB de pila reservada | 128 KB | 14 % |
| Pila medida (`-fstack-usage`) | ~2.5 KB en el peor camino | 8 KB | |
| CPU | interrupción de enlaces a 38.4 kHz: ~240 instrucciones → ~15 % estimado; `sx_paso()` corre 1 vez cada 15 min (microsegundos) | 84 MHz | |

Unos 28 KB de los 47 KB son de la biblioteca de C (printf con float, sscanf, `double` por software
que el núcleo v2 usa a propósito para redondear igual que la PC). Lo propio son ~19 KB. Si hiciera
falta espacio, quitar printf con float ahorra ~12 KB; hoy no hace falta.

## Cómo funciona la red

1. **AMBIENTE** (coordinador → todos, cada paso): número de paso `k`, T y HR del aire (SHT40),
   T del vidrio de referencia. Sincroniza la hora de todo el arreglo (no hace falta RTC en los nodos).
2. Cada nodo mide **sus** sensores y corre `sx_paso()`. Lo que no tiene lo toma del AMBIENTE.
3. **El ambiente caduca**: con más de 2 pasos (30 min) sin AMBIENTE, el nodo bloquea la AT y no
   corre el núcleo (para no encender `FALLA_SENSOR`, que en el v2 es permanente, por un corte de cable).
4. Si la política pide limpiar → **SOLICITUD**. El coordinador da **TURNOS**: máximo
   `max_simult` a la vez (presupuesto de la fuente), primero los de pre-rocío.
5. El nodo solo ejecuta un turno del paso actual y con permiso vigente → ciclo EDS → **FIN(rc)**.
6. **ESTADO** de cada nodo llega al coordinador, que lo imprime como JSON por USART1.

Si el coordinador se reinicia, espera 1.6 pasos y adopta el `k` que traen los nodos (no pierde la hora).

### Comandos por USART1 (115200 8N1)

| Comando | Qué hace |
|---|---|
| `HORA 0 06:15` | Fija día y hora en el coordinador |
| `LAVADO n` | La cuadrilla lavó el panel n (reinicia la costra) |
| `CAL n c_ref k_sr` | Calibración de instalación del nodo n (vidrio limpio y seco), se guarda en flash |
| `ID n` | Cambia el ID de **esta** tarjeta (reiniciar después) |
| `MAXSIM n` | Limpiezas simultáneas permitidas |
| `RED` | Padre, saltos, vecinos y contadores de la red |

## Resultados en la PC (`make pruebas`)

- **Núcleo**: el nodo 1 de la red toma las mismas decisiones (AT, bits, limpiar, alertas) que el demo
  del v2 en los 294 pasos comparados: 0 diferencias. Los 4 archivos del núcleo tienen el mismo SHA-256.
- **Red de 6 tarjetas, 28 días, enlaces simulados bit a bit**: nunca hubo más de 1 limpieza a la vez;
  0 ms de AT con agua; 0 limpiezas con el ambiente caducado; 0 tramas con CRC malo.
  - Coordinador apagado 6 h (día 12): los nodos bloquean la AT a los 2 pasos y se recuperan solos.
  - Cable 1-2 cortado (día 16): los nodos 2 y 3 rehacen el camino por 5 → 4 → 1.
  - Alertas solo en la tarjeta con la falla: conector en el nodo 3, térmica en el nodo 4.
- **2ª barrera** (`test_eds`): aborta con agua al arrancar (rc −1), rocío a media limpieza (rc −2)
  y fuga de corriente (rc −2); siempre termina con flyback y fases apagados.
- **Deriva de reloj** (`test_deriva`): los enlaces aguantan ±4 % de diferencia entre tarjetas.
  Con el oscilador interno (HSI) y la temperatura de un panel la deriva puede acercarse a ese margen:
  por eso se recomienda cristal de 8 MHz (el firmware lo usa si está).

## Instalar en el STM32 (M4)

**Antes, en la PCB** (hoy no se puede programar). Todo esto es solo de cables, ver H3/H4:
1. J2 como SWD: PGD → PA13 (SWDIO), PGC → PA14 (SWCLK). MCLR (NRST), +3.5 V y GND ya están.
2. BOOT0 a GND (hoy está al aire).
3. VDDA, VREF+ y VBAT a +3.5 V; VSSA a GND (hoy sin conectar: el ADC no leería).
4. Recomendado: cristal de 8 MHz en PH0/PH1.

**Con ST-LINK (V2 o el de cualquier Nucleo) y STM32CubeProgrammer (gratis):**
1. Conecta el ST-LINK al conector SWD con la tarjeta alimentada.
2. CubeProgrammer → ST-LINK → *Connect* → *Erasing & Programming*.
3. Archivo: `sparkx_coordinador.hex` para la tarjeta 1; `sparkx_nodoN.hex` para las demás
   (`make nodo ID=N`), o el mismo `sparkx_nodo.hex` en todas y después `ID N` por la terminal.
4. *Start Programming* con *Verify* → *Disconnect* → reset.
   Por línea de comandos: `STM32_Programmer_CLI -c port=SWD -w build/sparkx_nodo3.hex -v -rst`

**Sin ST-LINK** (bootloader de fábrica del STM32 por USART1 PA9/PA10): BOOT0 = 1, reset, adaptador
USB-serie de 3.3 V a PA9/PA10, `STM32_Programmer_CLI -c port=COM5 -w build/sparkx_nodo3.hex -v`,
BOOT0 = 0 y reset.

**Después de instalar en el panel:** con el vidrio limpio y seco, `CAL n c_ref k_sr` para cada nodo.

**Compilar** (Linux o WSL): `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi make` y
`cd firmware && make stm32`.

## Simular varias tarjetas en Proteus

Proteus 9.2 trae el **STM32F411RC con modelo de simulación** (búscalo con *Pick Devices*,
biblioteca CM4_STM32; el asistente de firmware no lo lista). Si tu versión no lo tiene, usa
el STM32F401RC con los archivos de `build/f401/` (`make proteus_f401`).

1. Pon 2 STM32F411RC vecinos (U1 y U2). *Program File*: `build/demo_nodo1.hex` en U1 (coordinador)
   y `build/demo_nodo2.hex` en U2. Cristal: 8 MHz (valor por defecto).
2. Une RIGHT PB13 de U1 con LEFT PC10 de U2 y pon una **resistencia de 4.7 kΩ a VDD** en el enlace.
3. Pon una *Virtual Terminal* a 115200 8N1 en PA9 de cada STM32 (TXD de la terminal a PA10 en U1
   para mandar comandos). En la del coordinador salen los JSON del arreglo.
4. Osciloscopio en PB4 (flyback) y PA2–PA4 (fases) de U1.
5. Modo demo: sensores simulados, paso de 15 min = 4 s, limpieza de 2 s. **Arranca el día 15 a
   las 18:00**: a las 19:45 (≈28 s) la IA predice el rocío y manda limpiar; de 00:30 a 09:00 la AT
   queda bloqueada; el agua aparece a las 05:15 (≈180 s). La secuencia esperada está en
   `resultados/sim_demo_rocio/` y en el reporte de simulación.
6. Con pasos de 4 s solo alcanzan turno las tarjetas a 1 salto del coordinador: usa tarjetas vecinas.
   La tarjeta 3 trae el conector que se degrada y el punto caliente (sus alertas necesitan 7 días
   de calibración; en la PC, `make sim`, se ven completas).

## Verificación

`make pruebas` (EDS, deriva de reloj, red de 6 tarjetas), `make nucleo` (SHA-256),
`python3 python/sil_validacion.py` (Python ↔ C, 0 diferencias) y
`python3 python/verificar_hex.py` (el árbol dentro de cada .hex es el modelo entrenado).

## Lo que falta (decirlo antes que el jurado)

- `SX_K_MEASURE_V = 476.7` supone MEASURE_V en el nodo R25.2 (H1). Hay que calibrarlo con
  un multímetro de AT. No hay medición de corriente de AT (`SX_TIENE_I_AT = 0`).
- Sin estación (comando `AMB`) ni SHT40 en el coordinador, nadie limpia (es a propósito).
- La tarjeta real necesita los cambios de componentes de la sección B de la revisión
  (zener de compuerta, U5, resistencias de LED y de AT). Proteus no los muestra.
- La simulación reproduce lecturas grabadas; no modela el efecto de una limpieza diferida sobre el polvo.
- El firmware no ha corrido en un STM32 real; está compilado para él y probado en la PC. El banco de
  Proteus está armado; falta la corrida con el binario ARM.
