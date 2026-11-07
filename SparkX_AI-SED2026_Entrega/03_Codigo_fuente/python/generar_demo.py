"""
generar_demo.py — Crea datos/demo_lecturas.csv para correr el firmware en la PC
==============================================================================
Ventana de 28 días de invierno (noches con rocío) con una falla de conector que
se degrada rápido y un punto caliente, para ver todas las alertas en la terminal.
Las sesiones de lectura salen del mismo simulador físico.
"""
import os
import numpy as np

from config import AGUA, PASOS_DIA
from sensores import construir_escenario, Fallas
from controlador import Arbol, Controlador, IA_PRED
from simulador import simular

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIAS = 28


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


def main():
    import json
    f = Fallas(conector_inicio_dia=9, conector_tau_dias=4.0, hotspot_inicio_dia=14, hotspot_dt=16.0)
    esc = construir_escenario(101, f)
    A = Arbol(os.path.join(RAIZ, "resultados", "modelo_rocio.json"))
    ruta = os.path.join(RAIZ, "resultados", "resultados_v2.json")
    par = json.load(open(ruta))["politicas"]["base"]["IA predictiva"]["params"] if os.path.exists(ruta) else {}
    g = Grabadora(Controlador(A, IA_PRED, par, esc.sesgos["c0"], esc.sesgos["gan_cal"]), esc)
    simular(esc, g)
    os.makedirs(os.path.join(RAIZ, "datos"), exist_ok=True)
    with open(os.path.join(RAIZ, "datos", "demo_lecturas.csv"), "w") as fh:
        fh.write(f"# {esc.sesgos['c0']:.6f},{esc.sesgos['gan_cal']:.6f}\n")
        fh.write("k,t_aire,hr,t_vidrio,isc_s,isc_l,c_nf,g_us,v_reposo,v_carga,i_carga,lavado,agua\n")
        for r in g.filas:
            fh.write(",".join(str(int(v)) if i in (0, 11, 12) else f"{float(v):.5f}" for i, v in enumerate(r)) + "\n")
    print(f"datos/demo_lecturas.csv: {len(g.filas)} pasos ({DIAS} días)")


if __name__ == "__main__":
    main()
