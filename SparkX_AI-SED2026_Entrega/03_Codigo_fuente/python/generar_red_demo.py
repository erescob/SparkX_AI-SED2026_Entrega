"""
generar_red_demo.py — Lecturas de 28 días para VARIAS tarjetas del mismo arreglo
==============================================================================
Todas las tarjetas ven el MISMO clima (mismo arreglo, misma semilla de clima),
pero cada una tiene sus propios sesgos de sensor, su polvo y sus fallas:

    nodo 1  coordinador, sano (tiene el SHT40 que reparte el ambiente)
    nodo 2  sano
    nodo 3  conector que se degrada desde el día 9
    nodo 4  punto caliente desde el día 14
    nodo 5  sano
    nodo 6  sano

Salidas:
    datos/red_nodoN.csv           para la simulación de la red en la PC (firmware/host)
    firmware/sx_demo_datos.h      tablas compactas para el modo demo en Proteus
                                  (nodo sano = tabla A, nodo con fallas = tabla B)
"""
import os
import sys
import numpy as np

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import sensores                                    # noqa: E402
from config import AGUA, PASOS_DIA                 # noqa: E402
from sensores import construir_escenario, Fallas   # noqa: E402
from controlador import Arbol, Controlador, IA_PRED  # noqa: E402
from simulador import simular                      # noqa: E402
import json                                        # noqa: E402

RAIZ = os.path.dirname(AQUI)
DIAS = 28
SEMILLA_CLIMA = 101

# --- mismo clima para todo el arreglo -------------------------------------------
_gen_original = sensores.generar_clima
_cache = {}


def _clima_comun(semilla, anio=2025):
    if "c" not in _cache:
        _cache["c"] = _gen_original(SEMILLA_CLIMA, anio)
    return _cache["c"].copy()


sensores.generar_clima = _clima_comun

NODOS = {
    1: Fallas(),
    2: Fallas(),
    3: Fallas(conector_inicio_dia=9, conector_tau_dias=4.0),
    4: Fallas(hotspot_inicio_dia=14, hotspot_dt=16.0),
    5: Fallas(),
    6: Fallas(),
}


class Grabadora:
    def __init__(self, ctrl, esc):
        self.c, self.esc, self.filas = ctrl, esc, []

    def paso(self, k, L):
        o = self.c.paso(k, L)
        if k < DIAS * PASOS_DIA:
            agua = int(self.esc.w[k] >= AGUA["w_inseguro"])
            self.filas.append((k, L["t_aire"], L["hr"], L["t_vidrio"], L["isc_s"], L["isc_l"], L["c_nf"],
                               L["g_us"], L["v_reposo"], L["v_carga"], L["i_carga"],
                               int(bool(L.get("lavado_realizado"))), agua))
        return o


def generar(nodo, fallas):
    esc = construir_escenario(1000 + nodo, fallas)       # sesgos y ruido propios de la tarjeta
    A = Arbol(os.path.join(RAIZ, "resultados", "modelo_rocio.json"))
    ruta = os.path.join(RAIZ, "resultados", "resultados_v2.json")
    par = json.load(open(ruta))["politicas"]["base"]["IA predictiva"]["params"] if os.path.exists(ruta) else {}
    g = Grabadora(Controlador(A, IA_PRED, par, esc.sesgos["c0"], esc.sesgos["gan_cal"]), esc)
    simular(esc, g)
    ruta_csv = os.path.join(RAIZ, "datos", f"red_nodo{nodo}.csv")
    with open(ruta_csv, "w") as fh:
        fh.write(f"# {esc.sesgos['c0']:.6f},{esc.sesgos['gan_cal']:.6f}\n")
        fh.write("k,t_aire,hr,t_vidrio,isc_s,isc_l,c_nf,g_us,v_reposo,v_carga,i_carga,lavado,agua\n")
        for r in g.filas:
            fh.write(",".join(str(int(v)) if i in (0, 11, 12) else f"{float(v):.5f}" for i, v in enumerate(r)) + "\n")
    print(f"{ruta_csv}: {len(g.filas)} pasos", flush=True)
    return esc.sesgos, np.array(g.filas, dtype=float)


def tabla_c(nombre, filas, sesgos):
    """Tabla compacta int16 para el STM32: 7 campos cada 15 min + prueba de carga diaria."""
    esc = [100, 100, 100, 100, 100, 10, 1000]          # t_aire, hr, t_vidrio, isc_s, isc_l, c_nf, g_us
    col = [1, 2, 3, 4, 5, 6, 7]
    lineas = [f"/* {nombre}: {len(filas)} pasos de 15 min. Escalas: t*100, hr*100, isc*100 (mA), c*10 (nF), g*1000 (uS) */",
              f"static const int16_t {nombre}[{len(filas)}][7] = {{"]
    for r in filas:
        v = [int(round(r[c] * e)) for c, e in zip(col, esc)]
        v = [max(-32768, min(32767, x)) for x in v]
        lineas.append("{" + ",".join(map(str, v)) + "},")
    lineas.append("};")
    # prueba de carga: una vez al día (paso 48)
    pr = filas[48::PASOS_DIA]
    lineas.append(f"static const int16_t {nombre}_PRUEBA[{len(pr)}][3] = {{   /* v_reposo*1000, v_carga*1000, i_carga*1000 */")
    for r in pr:
        lineas.append("{" + ",".join(str(int(round(r[c] * 1000))) for c in (8, 9, 10)) + "},")
    lineas.append("};")
    lineas.append(f"#define {nombre}_C_REF {sesgos['c0']:.6f}f")
    lineas.append(f"#define {nombre}_K_SR  {sesgos['gan_cal']:.6f}f")
    return "\n".join(lineas)


def main():
    os.makedirs(os.path.join(RAIZ, "datos"), exist_ok=True)
    res = {n: generar(n, f) for n, f in NODOS.items()}
    # Tablas para Proteus: A = nodo 1 (sano), B = nodo 3 con conector + hotspot del nodo 4
    # (para que en Proteus con 3-4 tarjetas se vean todas las alertas en una sola tarjeta)
    fB = Fallas(conector_inicio_dia=9, conector_tau_dias=4.0, hotspot_inicio_dia=14, hotspot_dt=16.0)
    sB, filasB = generar(99, fB)
    sA, filasA = res[1]
    h = ["/* sx_demo_datos.h — GENERADO por python/generar_red_demo.py (NO editar a mano)",
         " * Lecturas simuladas para el MODO DEMO (Proteus sin sensores reales).",
         " * Tabla A: tarjeta sana.  Tabla B: tarjeta con conector degradándose (día 9) y punto caliente (día 14). */",
         "#ifndef SX_DEMO_DATOS_H", "#define SX_DEMO_DATOS_H", "#include <stdint.h>",
         f"#define SX_DEMO_PASOS {len(filasA)}",
         tabla_c("SX_DEMO_A", filasA, sA), tabla_c("SX_DEMO_B", filasB, sB), "#endif", ""]
    open(os.path.join(RAIZ, "firmware", "sx_demo_datos.h"), "w").write("\n".join(h))
    os.remove(os.path.join(RAIZ, "datos", "red_nodo99.csv"))
    print("firmware/sx_demo_datos.h listo")


if __name__ == "__main__":
    main()
