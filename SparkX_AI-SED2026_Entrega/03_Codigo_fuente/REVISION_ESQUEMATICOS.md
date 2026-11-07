# Revisión de los esquemáticos (PROYECTO.zip, archivos del 02-nov-2026)

Reconstruí el netlist de los 6 proyectos de Proteus a partir de los archivos `.pdsprj`: posición, rotación y espejo de cada componente, pines, cables, terminales y nodos. Los netlists completos están en `revision/net_*.txt`. Cada hallazgo de abajo lo revisé dibujando el cable correspondiente.

| Proyecto | Qué es | Estado |
|---|---|---|
| `SISTEMA_PCB/MotorPaso_PIC16_PCB` | Integración final: entrada, buck, STM32, flyback, etapa trifásica, medición | Ver hallazgos |
| `POWER_INPUT/POWER_INPUT_12V` | Entrada de 12 V (TVS, diodo, capacitores) | Bien |
| `BUCK/Buck_12Vto5V_AISED - copia` | MC34063 a 3.5 V | Conexiones bien |
| `Flyback/flyback2` | UC3843 + IRF840 + transformador, 1.2 kV | LED del optoacoplador en corto (H2) |
| `Fases/Fases` | Etapa trifásica de prueba | Misma compuerta sin protección (H5) |
| `STM/STM` | Prueba del STM32 con los PhotoMOS | Mismo error de MEASURE a tierra (H1) |

## A. Se arreglan solo con cables o etiquetas (sin componentes nuevos)

| # | Dónde | Qué pasa | Arreglo |
|---|---|---|---|
| H1 | U8 / U4 | **MEASURE_V y MEASURE_C están en el mismo nodo que GND** (en −VIN de U8 y U4, junto al símbolo de tierra). PA0 y PA1 leen 0 V siempre: el firmware nunca vería subir la AT y abortaría cada limpieza. | Mover la etiqueta **MEASURE_V al nodo R25.2 / U1.K**. Es la parte baja del divisor de AT: 8.8 MΩ + 18.5 kΩ dan 2.52 V con 1.2 kV (el firmware ya usa ese factor, 476.7). Quitar MEASURE_C de tierra y dejarlo libre. |
| H2 | U1 (PC817), en SISTEMA y en flyback2 | **El LED del optoacoplador está en corto**: un tramo de cable une el nodo de A con el de K (en SISTEMA va de X=2700 a X=3000 th, Y=−6100 th; en flyback2 de X=2200 a X=2500 th, Y=−2000 th). Sin LED no hay realimentación: el flyback no regula la AT. | Borrar ese tramo: debe quedar D8.A–U1.A y U1.K–R25.2. |
| H3 | U9 (STM32) | VDDA, VREF+, VSSA, VBAT y BOOT0 sin conectar. En Proteus funciona, pero en la tarjeta real el ADC no tendría alimentación y BOOT0 queda al aire. | VDDA, VREF+ y VBAT a +3.5 V; VSSA y BOOT0 a GND. |
| H4 | J2 | Es un conector de PIC: PGD y PGC no van a ningún lado, y PA13/PA14 (SWD) no están conectados. No hay forma de programar el STM32. | Usar el mismo J2 como SWD: PGD → **PA13 (SWDIO)**, PGC → **PA14 (SWCLK)**. MCLR ya es NRST; +3.5 V y GND ya están. |
| H4b | TP1, TP2, TP5 | Las etiquetas "12V" y "3.5V" no son iguales a "+12V" y "+3.5V" (Proteus une por nombre exacto). Los puntos de prueba quedan sueltos. AZUL_1 no va a nada. | Renombrar a +12V / +3.5V. |
| H4c | UP/DOWN/LEFT/RIGHT_PANEL | Solo llegan a PB9, PB13, PC6 y PC10. No hay conector hacia los paneles vecinos. | Poner pads o un conector (+ GND común). |
| H4d | ELECTRODE_A/B/C | Solo tocan el drenador de Q1/Q3/Q5 y R36/R10/R13. **No salen de la tarjeta hacia el vidrio**. | Conector de AT para los 3 electrodos. |

## B. Para la tarjeta real hacen falta cambios de componentes

Proteus no muestra estos problemas porque sus modelos no rompen la compuerta ni limitan la corriente del PhotoMOS.

| # | Dónde | Qué pasa | Cambio mínimo |
|---|---|---|---|
| H5 | Q1/Q3/Q5 (STW4N150) | La compuerta se sube a **+1.2 kV por R3/R9/R12 (8.2 MΩ) sin nada que la limite**. VGS máx es ±30 V, así que se rompe en cuanto Q7/Q2/Q4 se apagan. | Zener de 15 V compuerta–fuente en Q1/Q3/Q5 (3 piezas). |
| H6 | Q7/Q2/Q4 (STW4N150) | Se manejan directo con el GPIO a 3.5 V, pero VGS(th) es 3–5 V (4 V típico): no conducen bien. | Con el zener de H5 estos transistores solo ven ~15 V: cambiarlos por MOSFET de nivel lógico pequeños (ej. 2N7002). Es sustitución, no componente extra. |
| H7 | U5 (AQV258H5A) | **U5 es el relé del flyback**: es un relé de estado sólido (PhotoMOS). Su contacto va de +12V (OUT_1) a SOURCE_FLYBACK (OUT_2), y esa red alimenta el VCC del UC3843 y el devanado TR3.S1–S2 que conmuta el IRF840. O sea, **toda la corriente del primario del flyback pasa por U5**. Su hoja de datos da 20 mA continuos y hasta 500 Ω encendido; el flyback pide cientos de mA. | Cambiar U5 por un relé electromecánico de 12 V o un P-MOSFET de lado alto. |
| H8 | U5, U6, U7 | El LED de cada PhotoMOS va directo del GPIO a tierra, sin resistencia. U6 y U7 comparten PB1. El STM32 da máx. 25 mA por pin; el LED se recomienda a 5–30 mA. | 3 resistencias de ~220 Ω en serie. |
| H9 | U6 + U7 + L2 | Con SECURE_MEASURE = 1, los dos PhotoMOS conectan **+1.2 kV a GND a través de L2 (6 µH)**. Si queda AT, la corriente pico supera los 60 mA que aguantan. | El firmware nunca los cierra con más de 50 V. Si se usan para descargar, poner una resistencia en serie. |
| H10 | R36/R10/R13 (450 k, Yageo MFR-50) | Ven 1.2 kV y disipan **3.2 W cada una** mientras su fase está en bajo. La serie MFR-50 es de 0.5 W y 350 V. | Resistencias de AT de 3 W o cadenas en serie. |
| H11 | R3/R9/R12 (8.2 M, 1/2 W) | Ven 1.2 kV con Q7 encendido. | Resistencia de alta tensión o dos en serie. |
| H12 | C22 (500 pF, AVX 08052C501…) | Está en el riel de +1.2 kV, pero el código AVX corresponde a 0805 de 200 V. Verifíquenlo. | Capacitor de ≥2 kV. |
| H13 | PDQ2D48S12S (U4, U8) | Es un convertidor DC-DC aislado con entrada de 36–75 V. No transmite una señal analógica y aquí se alimenta con 3.5 V. Con H1 corregido no hace falta. | Se pueden dejar sin montar. |

## C. Lista de materiales (BOM)

| # | Componente | Diferencia |
|---|---|---|
| B1 | U9 | El símbolo es STM32F411RC, pero el código de compra dice **STM32F446RCT6**. El firmware es para el F411: confirmen cuál van a comprar. |
| B2 | R7 | El valor dice **1k8** (salida 3.5 V), pero el número de parte ERJ-6GEYJ**302**V es de **3.0 kΩ**. Con 3.0 k el buck da **5.0 V** y quema el STM32 (máx. 3.6 V). |
| B3 | R37 | 2.2 Ω de sensor de corriente del MC34063: pico de 0.3/2.2 = 136 mA, unos 70 mA de salida. Alcanza para el STM32, pero hay poco margen. |
| B4 | D2 (SMBJ12A) | Conduce desde ~13.3 V. Si los 12 V vienen de batería o cargador (13.8–14.4 V), se calienta. Usar SMBJ15A en ese caso. |

## D. Revisado y correcto

- Entrada: J1 → F1 → D2 (TVS a GND) → D3 (B340, contra polaridad invertida) → +12V con C3 470 µF y C4 100 nF.
- Buck MC34063:
  - V+ a +12V, R37 como sensor de corriente, DRC/IPK/SWC unidos y SWE → L1 330 µH / D4.
  - Realimentación R7/R6 en CINV y C5 en CT.
  - 1.25 × (1 + 1.8k/1k) = 3.5 V.
- STM32: NRST con R5 a +3.5 V y botón S3; C10 4.7 µF en VCAP_1; VDD/VSS a +3.5 V/GND por propiedad.
- Flyback:
  - UC3843 con RT/CT (R26/C20) y sensor de corriente R20 0.22 Ω con filtro R19/C16.
  - Rampa R27 desde VREF; compuerta por R17.
  - Arranque y autoalimentación por D7/R16 desde el devanado B; amortiguador D1/C18/R22.
  - Rectificador D5 (R3000F, 3 kV); divisor de 4 × 2.2 MΩ con toma de +300 V.
- Etapa trifásica: PHASE_x → Q7/Q2/Q4 (con R1/R8/R11 de 4 kΩ a GND) → compuerta de Q1/Q3/Q5 → electrodo con resistencia de 450 kΩ a +1.2 kV. **PHASE_x = 1 pone el electrodo en alta y 0 lo lleva a tierra** (el firmware ya usa esta polaridad).
- ON/OFF_FLYBACK = PB4 → LED de U5; ACTIVE_V = PB0 → ON/OFF de U8; ACTIVE_C = PB3 → ON/OFF de U4; SECURE_MEASURE = PB1 → LED de U6 y U7.

## Qué cambió en el firmware por esta revisión

- `SX_K_MEASURE_V = 476.7` (MEASURE_V en el nodo R25.2 de H1) y `SX_TIENE_I_AT = 0`. La 2ª barrera usa la caída de AT; no hay corriente de AT medida.
- Descarga con las fases en 0 (electrodos a tierra por Q1/Q3/Q5), según la polaridad real de la etapa.
- La alerta **REVISAR_EDS** sigue la AT mínima de cada limpieza contra su línea base. Así hay mantenimiento por panel usando solo lo que la PCB mide.
- Mientras no se aplique H1 en Proteus, el `.hex` real leerá 0 V en PA0 y abortará las limpiezas (rc −1). Los `.hex` de demo no dependen de eso.
