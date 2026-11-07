"""
sil_validacion.py — Prueba software-in-the-loop del firmware C
=============================================================

1. Compila firmware/sx_controlador.c como librería compartida.
2. PASO A PASO: alimenta al C y al espejo Python con EXACTAMENTE las mismas
   lecturas durante años completos (con fallas de sensores y de conector
   incluidas) y compara todas las salidas en cada paso.
3. LAZO CERRADO: el C controla la planta simulada él solo y se comparan los
   resultados anuales contra el Python.

Si esto pasa, lo que se evaluó en Python es lo que correrá en el STM32
(salvo el hardware: ADC, drivers y tiempos, que se prueban en Proteus/banco).
"""
import ctypes as C
import json
import os
import subprocess
import numpy as np

from config import IA
from controlador import Arbol, Controlador, IA_PRED, CADA_HORA, UMBRAL, FIJO, REGLA_ROCIO
from sensores import construir_escenario, Fallas
from simulador import simular

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(RAIZ, "firmware", "nucleo_v2")
LIB = os.path.join(RAIZ, "firmware", "build", "libsparkx.so")


class Lecturas(C.Structure):
    _fields_ = [("t_aire", C.c_float), ("hr", C.c_float), ("t_vidrio", C.c_float),
                ("isc_s", C.c_float), ("isc_l", C.c_float), ("c_nf", C.c_float), ("g_us", C.c_float),
                ("prueba_valida", C.c_uint8), ("v_reposo", C.c_float), ("v_carga", C.c_float),
                ("i_carga", C.c_float), ("lavado_realizado", C.c_uint8)]


class Salidas(C.Structure):
    _fields_ = [("limpiar", C.c_uint8), ("limpiar_ref", C.c_uint8), ("permiso", C.c_uint8),
                ("bits", C.c_uint8), ("p_rocio", C.c_float), ("margen", C.c_float),
                ("suelto", C.c_float), ("costra", C.c_float), ("horas_a_limpieza", C.c_float),
                ("alerta_lavado", C.c_uint8), ("alerta_conector", C.c_uint8),
                ("dias_falla_conector", C.c_float), ("alerta_termica", C.c_uint8),
                ("falla_sensor", C.c_uint8), ("alerta_v1_conector", C.c_uint8)]


class Params(C.Structure):
    _fields_ = [("politica", C.c_uint8), ("umbral_suelto", C.c_float), ("umbral_pre", C.c_float),
                ("margen_regla", C.c_float), ("umbral_costra", C.c_float), ("alfa_sr", C.c_float),
                ("p_rocio", C.c_double), ("hora_fija_ini", C.c_double), ("hora_fija_fin", C.c_double),
                ("seguridad_v1", C.c_uint8)]


def compilar():
    os.makedirs(os.path.dirname(LIB), exist_ok=True)
    cmd = ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-ffp-contract=off", "-shared", "-fPIC",
           os.path.join(FW, "sx_controlador.c"), os.path.join(FW, "sil_wrapper.c"), "-o", LIB, "-lm"]
    subprocess.run(cmd, check=True)
    lib = C.CDLL(LIB)
    for nombre, cls in (("sx_tam_lecturas", Lecturas), ("sx_tam_salidas", Salidas), ("sx_tam_params", Params)):
        f = getattr(lib, nombre); f.restype = C.c_size_t
        assert f() == C.sizeof(cls), f"{nombre}: C={f()} Python={C.sizeof(cls)} (estructuras no coinciden)"
    lib.sx_tam_estado.restype = C.c_size_t
    return lib


class ControladorC:
    """Misma interfaz que controlador.Controlador, pero ejecuta el C compilado."""

    def __init__(self, lib, politica, params, c_ref_nf, k_sr):
        from config import POLITICA
        P = dict(POLITICA); P.update(params or {})
        self.lib = lib
        self.p = Params(politica, P["umbral_suelto"], P["umbral_pre"], P["margen_regla"], P["umbral_costra"],
                        P["alfa_sr"], P["p_rocio"], P["hora_fija_ini"], P["hora_fija_fin"],
                        1 if P.get("seguridad_v1") else 0)
        self.estado = C.create_string_buffer(lib.sx_tam_estado())
        lib.sx_iniciar(self.estado, C.byref(self.p), C.c_float(c_ref_nf), C.c_float(k_sr))
        self.L = Lecturas(); self.o = Salidas()

    def paso(self, k, L):
        l = self.L
        l.t_aire, l.hr, l.t_vidrio = L["t_aire"], L["hr"], L["t_vidrio"]
        l.isc_s, l.isc_l, l.c_nf, l.g_us = L["isc_s"], L["isc_l"], L["c_nf"], L["g_us"]
        l.prueba_valida = 1 if L["prueba_valida"] else 0
        l.v_reposo, l.v_carga, l.i_carga = L["v_reposo"], L["v_carga"], L["i_carga"]
        l.lavado_realizado = 1 if L.get("lavado_realizado") else 0
        self.lib.sx_paso(self.estado, C.c_int32(k), C.byref(l), C.byref(self.o))
        o = self.o
        return dict(limpiar=bool(o.limpiar), limpiar_ref=bool(o.limpiar_ref), permiso=bool(o.permiso),
                    bits=int(o.bits), p_rocio=float(o.p_rocio), margen=float(o.margen), suelto=float(o.suelto),
                    costra=float(o.costra), horas_a_limpieza=float(o.horas_a_limpieza),
                    alerta_lavado=bool(o.alerta_lavado), alerta_conector=bool(o.alerta_conector),
                    dias_falla_conector=float(o.dias_falla_conector), alerta_termica=bool(o.alerta_termica),
                    falla_sensor=bool(o.falla_sensor), alerta_v1_conector=bool(o.alerta_v1_conector))


class Doble:
    """Corre Python y C en paralelo con las mismas lecturas y registra diferencias."""

    def __init__(self, py, c):
        self.py, self.c = py, c
        self.pasos = 0
        self.dif = {}

    def paso(self, k, L):
        a = self.py.paso(k, L)
        b = self.c.paso(k, L)
        self.pasos += 1
        for key in a:
            va, vb = a[key], b[key]
            igual = (va == vb) if not isinstance(va, float) else (np.float32(va) == np.float32(vb))
            if not igual:
                self.dif.setdefault(key, []).append((k, va, vb))
        return a


def main():
    lib = compilar()
    A = Arbol(os.path.join(RAIZ, "resultados", "modelo_rocio.json"))
    ruta = os.path.join(RAIZ, "resultados", "resultados_v2.json")
    par_ia = json.load(open(ruta))["politicas"]["base"]["IA predictiva"]["params"] if os.path.exists(ruta) else {}
    casos = [
        (100, IA_PRED, par_ia, Fallas()),
        (101, IA_PRED, par_ia, Fallas(conector_inicio_dia=80, conector_tau_dias=20, hotspot_inicio_dia=200, hotspot_dt=16)),
        (102, REGLA_ROCIO, {}, Fallas(sensor="sht_congelado", sensor_inicio_dia=150.4)),
        (103, FIJO, {}, Fallas(sensor="ntc_abierto", sensor_inicio_dia=250.7)),
        (100, CADA_HORA, {}, Fallas(sensor="afe_abierto", sensor_inicio_dia=90.2)),
        (101, UMBRAL, {"seguridad_v1": True}, Fallas()),
    ]
    reporte = {"paso_a_paso": [], "lazo_cerrado": []}
    total_pasos = total_dif = 0
    for semilla, pol, prm, f in casos:
        esc = construir_escenario(semilla, f)
        py = Controlador(A, pol, prm, esc.sesgos["c0"], esc.sesgos["gan_cal"])
        cc = ControladorC(lib, pol, prm, esc.sesgos["c0"], esc.sesgos["gan_cal"])
        d = Doble(py, cc)
        simular(esc, d)
        n_dif = sum(len(v) for v in d.dif.values())
        total_pasos += d.pasos
        total_dif += n_dif
        r = dict(semilla=semilla, politica=pol, fallas=str(f), pasos=d.pasos,
                 salidas_comparadas=d.pasos * 15, diferencias=n_dif,
                 campos_con_diferencias={k: len(v) for k, v in d.dif.items()},
                 ejemplos={k: v[:3] for k, v in d.dif.items()})
        reporte["paso_a_paso"].append(r)
        print(f"semilla {semilla} pol {pol}: {d.pasos} pasos, diferencias = {n_dif} {r['campos_con_diferencias']}")

    # lazo cerrado: el C solo
    for semilla in IA["semillas_prueba"]:
        esc = construir_escenario(semilla)
        rp = simular(esc, Controlador(A, IA_PRED, par_ia, esc.sesgos["c0"], esc.sesgos["gan_cal"]))
        rc = simular(esc, ControladorC(lib, IA_PRED, par_ia, esc.sesgos["c0"], esc.sesgos["gan_cal"]))
        claves = ["perdida_suciedad_pct", "ciclos_eds", "lavados", "activaciones_inseguras", "energia_neta_kwh"]
        fila = {k: (round(rp[k], 6), round(rc[k], 6)) for k in claves}
        fila["identico"] = all(rp[k] == rc[k] for k in claves)
        fila["semilla"] = semilla
        reporte["lazo_cerrado"].append(fila)
        print(f"lazo cerrado semilla {semilla}: idéntico={fila['identico']}  {fila}")
    reporte["resumen"] = dict(pasos_totales=total_pasos, salidas_comparadas=total_pasos * 15,
                              diferencias_totales=total_dif,
                              lazo_cerrado_identico=all(f["identico"] for f in reporte["lazo_cerrado"]))
    json.dump(reporte, open(os.path.join(RAIZ, "resultados", "sil_validacion.json"), "w"), indent=1, default=str)
    print("RESUMEN:", reporte["resumen"])


if __name__ == "__main__":
    main()
