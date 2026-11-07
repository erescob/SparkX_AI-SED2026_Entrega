# Spark-X red — firmware de las tarjetas del arreglo (STM32F411RC, Cortex-M4F)
#
#   make sim        simulación de 6 tarjetas en la PC (enlaces bit a bit, 28 días)
#   make stm32      firmware real (paso de 15 min): coordinador (ID 1) y nodo genérico (ID 2)
#   make nodo ID=5  firmware real con el ID 5 grabado de fábrica  -> build/sparkx_nodo5.hex
#   make pruebas    pruebas en la PC: ciclo EDS (2a barrera), deriva de reloj, red de 6 tarjetas
#   make tamano     cuánto ocupa en flash y RAM, por módulo
#   make nucleo     comprueba que el núcleo v2 es idéntico al validado (SHA-256)
#   make proteus    4 hex de demo (nodo1..nodo4, paso de 4 s, sensores simulados)
NUCLEO  = nucleo_v2/sx_controlador.c
COMUN   = sx_adquisicion.c sx_eds.c sx_red.c sx_uart1h.c sx_nodo.c
INC     = -I. -Inucleo_v2
WARN    = -Wall -Wextra -Wno-unused-parameter
# -ffp-contract=off: sin FMA, el M4 redondea igual que la simulación con la que se validó el v2
FP      = -ffp-contract=off

sim: build/sim_red
	mkdir -p ../resultados/sim_red
	./build/sim_red ../datos ../resultados/sim_red
	mkdir -p ../resultados/sim_red_completo
	SX_SIM_HW=completo ./build/sim_red ../datos ../resultados/sim_red_completo
	cat ../resultados/sim_red/resumen.json

build/sim_red: host/sim_red.c $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h
	mkdir -p build
	gcc -std=c99 -O2 $(WARN) $(FP) $(INC) -DSX_PASO_MS=8000u -DSX_EDS_CICLOS=15u -DSX_K0=0 \
	    host/sim_red.c $(NUCLEO) $(COMUN) -o $@ -lm

# ---------------------------------------------------------------- STM32F411RC
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size
NEWLIB ?= /usr/lib/arm-none-eabi/newlib/thumb/v7e-m+fp/hard
MCU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
ACFLAGS = -std=c99 -Os -g $(MCU) $(WARN) $(FP) $(INC) -Istm32 -Istm32/cmsis -DSTM32F411xE \
          -ffunction-sections -fdata-sections -isystem /usr/include/newlib
ALDFLAGS= $(MCU) -Tstm32/stm32f411rc.ld -nostartfiles -Wl,--gc-sections -L$(NEWLIB) \
          -specs=/usr/lib/arm-none-eabi/newlib/nano.specs -Wl,-Map=$(basename $@).map \
          -Wl,--start-group -lc_nano -lm -Wl,--end-group -u _printf_float
STM     = stm32/sx_hal_stm32.c stm32/startup_stm32f411.c stm32/main.c

stm32: build/sparkx_nodo.hex build/sparkx_coordinador.hex

build/sparkx_nodo.elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h 
	mkdir -p build
	$(CC) $(ACFLAGS) -DSX_ID_DEFECTO=2 $(STM) $(NUCLEO) $(COMUN) $(ALDFLAGS) -o $@
	$(SIZE) $@
build/sparkx_coordinador.elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h 
	mkdir -p build
	$(CC) $(ACFLAGS) -DSX_ID_DEFECTO=1 $(STM) $(NUCLEO) $(COMUN) $(ALDFLAGS) -o $@
	$(SIZE) $@

# Demo para Proteus: sensores leídos de sx_demo_datos.h, paso de 4 s, limpieza de 2 s,
# arranca el día 15 a las 18:00 (k0 = 1512): esa noche la IA predice el rocío del día 16
# (05:15) y manda limpiar a las 19:45, ~9 h antes; luego bloquea la AT mientras hay agua.
#   make proteus            PCB ACTUAL, sin componentes nuevos: el ambiente viene de la estación
#                           de la planta (el HAL simula al SCADA mandando "AMB ..." al coordinador)
#   make proteus_completo   con el hardware del v2 (SHT40 en el coordinador, NTC, mini-celdas, AFE)
DEMO    = -DSX_MODO_DEMO=1 -DSX_PASO_MS=4000u -DSX_EDS_CICLOS=100u -DSX_K0=1512
DEMO_HW = -DSX_TIENE_SHT40=1 -DSX_TIENE_NTC=1 -DSX_TIENE_MINICELDAS=1 -DSX_TIENE_AFE=1 -DSX_TIENE_PRUEBA_CARGA=1
LDDEMO  = $(subst stm32f411rc.ld,stm32f411rc_demo.ld,$(ALDFLAGS))
proteus: build/demo_nodo1.hex build/demo_nodo2.hex build/demo_nodo3.hex build/demo_nodo4.hex
# Mismo demo para el STM32F401RC (64 KB de RAM), por si la versión de Proteus no trae el F411RC
LDDEMO401 = $(subst stm32f411rc.ld,stm32f401rc_demo.ld,$(ALDFLAGS))
proteus_f401: build/f401/demo_nodo1.hex build/f401/demo_nodo2.hex build/f401/demo_nodo3.hex build/f401/demo_nodo4.hex
build/f401/demo_nodo%.elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h sx_demo_datos.h
	mkdir -p build/f401
	$(CC) $(ACFLAGS) $(DEMO) -DSX_ID_DEFECTO=$* $(STM) $(NUCLEO) $(COMUN) $(LDDEMO401) -o $@
	$(SIZE) $@
proteus_completo: build/demo_completo_nodo1.hex build/demo_completo_nodo2.hex build/demo_completo_nodo3.hex build/demo_completo_nodo4.hex
build/demo_nodo%.elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h sx_demo_datos.h
	mkdir -p build
	$(CC) $(ACFLAGS) $(DEMO) -DSX_ID_DEFECTO=$* $(STM) $(NUCLEO) $(COMUN) $(LDDEMO) -o $@
	$(SIZE) $@
build/demo_completo_nodo%.elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h sx_demo_datos.h
	mkdir -p build
	$(CC) $(ACFLAGS) $(DEMO) $(DEMO_HW) -DSX_ID_DEFECTO=$* $(STM) $(NUCLEO) $(COMUN) $(LDDEMO) -o $@
	$(SIZE) $@

%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

pruebas: build/test_eds build/test_eds_iat build/test_deriva
	./build/test_eds
	./build/test_eds_iat
	./build/test_deriva
	$(MAKE) sim
build/test_eds: host/test_eds.c sx_eds.c sx_adquisicion.c *.h
	mkdir -p build
	gcc -std=c99 -O2 $(WARN) $(INC) host/test_eds.c sx_eds.c sx_adquisicion.c -o $@ -lm
build/test_eds_iat: host/test_eds.c sx_eds.c sx_adquisicion.c *.h
	mkdir -p build
	gcc -std=c99 -O2 $(WARN) $(INC) -DSX_TIENE_I_AT=1 host/test_eds.c sx_eds.c sx_adquisicion.c -o $@ -lm
build/test_deriva: host/test_deriva.c sx_uart1h.c *.h
	mkdir -p build
	gcc -std=c99 -O2 $(WARN) $(INC) host/test_deriva.c sx_uart1h.c -o $@

ID ?= 2
nodo: build/sparkx_nodo$(ID).hex
build/sparkx_nodo$(ID).elf: $(STM) $(NUCLEO) $(COMUN) *.h nucleo_v2/*.h
	mkdir -p build
	$(CC) $(ACFLAGS) -DSX_ID_DEFECTO=$(ID) $(STM) $(NUCLEO) $(COMUN) $(ALDFLAGS) -o $@
	$(SIZE) $@

tamano: build/sparkx_nodo.elf
	@echo "STM32F411RC: 256 KB flash (128 KB para el programa, 128 KB de configuración), 128 KB RAM"
	@$(SIZE) build/sparkx_nodo.elf
	@mkdir -p build/obj
	@for f in $(STM) $(NUCLEO) $(COMUN); do \
	  $(CC) $(ACFLAGS) -DSX_ID_DEFECTO=2 -c $$f -o build/obj/$$(basename $$f .c).o; \
	  arm-none-eabi-size -A build/obj/$$(basename $$f .c).o | awk -v f=$$(basename $$f) \
	  '/^\.text|^\.rodata/{t+=$$2} /^\.data/{d+=$$2} /^\.bss/{b+=$$2} END{printf "  %-22s flash %6d B   RAM %6d B\n", f, t+d, d+b}'; done

nucleo:
	cd nucleo_v2 && sha256sum -c SHA256SUMS

clean:
	rm -rf build
.PHONY: sim stm32 proteus proteus_completo proteus_f401 nodo tamano nucleo pruebas clean
.PRECIOUS: build/%.elf build/demo_nodo%.elf build/demo_completo_nodo%.elf build/f401/demo_nodo%.elf
