"""
evaluar_v2.py — La comparativa "antes/después" honesta
=====================================================

1. POLÍTICAS DE LIMPIEZA: cada una se AJUSTA con 6 años de entrenamiento y se
   evalúa en 4 años que nunca vio. Se repite en 3 tipos de sitio para ver si
   la conclusión depende de los supuestos (análisis de sensibilidad).
2. SEGURIDAD: activaciones de alta tensión con agua en el vidrio, con la capa
   de seguridad del v1 (solo "temp <= 20 °C") vs la del v2, incluyendo fallas
   de sensores inyectadas.
3. ALERTAS DE MANTENIMIENTO: conector y anomalía térmica, v2 vs reglas del v1
   (detección, anticipación, falsas alarmas y error del pronóstico).

Todo número que aparece en el informe sale de resultados/resultados_v2.json.
"""
import json
import os
import itertools
import copy
from multiprocessing import Pool
import numpy as np
import pandas as pd

import config
from config import IA, POLITICA, FALLAS as FA, PASOS_DIA, ECONOMIA
from controlador import (Arbol, Controlador, NOMBRE_POLITICA, NUNCA, FIJO, UMBRAL, IA_PRED,
                         REGLA_ROCIO, CADA_HORA)

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(RAIZ, "resultados")

SITIOS = {
    "base": {},
    "polvoso": {"deposito_base": 4.0, "evento_mediana_gm2": 2.0},           # multiplicadores
    "cementacion_alta": {"k_cementacion_h": 2.5},
}
BASE_SUCIEDAD = copy.deepcopy(config.SUCIEDAD)

_cache = {}
_arbol = None


def _aplicar_sitio(sitio):
    config.SUCIEDAD.clear()
    config.SUCIEDAD.update(copy.deepcopy(BASE_SUCIEDAD))
    for k, mult in SITIOS[sitio].items():
        config.SUCIEDAD[k] = BASE_SUCIEDAD[k] * mult


def _prob_vectorizada(A, X):
    nodo = np.zeros(len(X), np.int32)
    for _ in range(64):
        f = A.f[nodo]
        hoja = f < 0
        if hoja.all():
            break
        fi = np.where(hoja, 0, f)
        ir_izq = X[np.arange(len(X)), fi] <= A.t[nodo]
        nodo = np.where(hoja, nodo, np.where(ir_izq, A.l[nodo], A.r[nodo]))
    return A.p[nodo].astype(float)


def _escenario(sitio, semilla, fallas_key=None, fallas=None):
    from sensores import construir_escenario
    from caracteristicas import caracteristicas_vectorizadas
    clave = (sitio, semilla, fallas_key)
    if clave not in _cache:
        _aplicar_sitio(sitio)
        esc = construir_escenario(semilla, fallas)
        X = caracteristicas_vectorizadas(esc.t_aire_m, esc.hr_m, esc.tv_m, esc.clima.hora.values)
        p = _prob_vectorizada(_arbol, X)
        if len(_cache) > 40:
            _cache.clear()
        _cache[clave] = (esc, X, p)
    return _cache[clave]


def _init():
    global _arbol
    _arbol = Arbol(os.path.join(RES, "modelo_rocio.json"))


def correr(tarea):
    """tarea = (sitio, semilla, politica, params, extra)"""
    from simulador import simular
    sitio, semilla, pol, params, extra = tarea
    extra = extra or {}
    esc, X, p = _escenario(sitio, semilla, extra.get("fallas_key"), extra.get("fallas"))
    _aplicar_sitio(sitio)
    precalc = None if extra.get("paso_a_paso") else (X, p)
    if pol == "lavado_mensual":
        c = Controlador(_arbol, NUNCA, params, esc.sesgos["c0"], esc.sesgos["gan_cal"], precalc=precalc)
        r = simular(esc, c, lavado_periodico_dias=config.LAVADO["periodo_sin_eds_dias"], usar_eds=False)
    else:
        c = Controlador(_arbol, pol, params, esc.sesgos["c0"], esc.sesgos["gan_cal"], precalc=precalc)
        r = simular(esc, c)
    r.pop("log", None)
    r["usd"] = (r["energia_neta_kwh"] * ECONOMIA["tarifa_usd_kwh"] - r["lavados"] * POLITICA["costo_lavado_usd"]
                - r["ciclos_eds"] * POLITICA["costo_ciclo_usd"])
    r["tarea"] = (sitio, semilla, str(pol), json.dumps(params, sort_keys=True), extra.get("fallas_key"))
    if extra.get("dias_v1_termica") is not None or extra.get("calc_v1_termica"):
        tv = esc.tv_m.reshape(-1, PASOS_DIA)
        r["dias_v1_termica"] = int((tv > 45.175).any(axis=1).sum())
        r["primer_dia_v1_termica"] = float(np.argmax((tv > 45.175).any(axis=1))) if (tv > 45.175).any() else None
    if extra.get("fallas") is not None:
        from sensores import r_contacto
        f = extra["fallas"]
        dias = np.arange(365, dtype=float)
        rc = r_contacto(dias, f)
        r["dia_r_alerta_real"] = float(np.argmax(rc >= FA["r_contacto_alerta"])) if (rc >= FA["r_contacto_alerta"]).any() else None
        r["dia_r_falla_real"] = float(np.argmax(rc >= FA["r_contacto_falla"])) if (rc >= FA["r_contacto_falla"]).any() else None
        r["fallas"] = dict(conector=f.conector_inicio_dia, hotspot=f.hotspot_inicio_dia, hotspot_dt=f.hotspot_dt,
                           sensor=f.sensor, sensor_dia=f.sensor_inicio_dia)
    return r


def rejilla():
    u_vals = [0.0025, 0.005, 0.010, 0.020]
    g = []
    for h in (7.0, 12.0, 17.0, 20.0):
        g.append((FIJO, dict(hora_fija_ini=h, hora_fija_fin=h + 4.0)))
    for u in u_vals:
        g.append((UMBRAL, dict(umbral_suelto=u)))
    for u, pr, up in itertools.product(u_vals, (0.6, 0.8, 0.9), (0.001, 0.003)):
        g.append((IA_PRED, dict(umbral_suelto=u, p_rocio=pr, umbral_pre=up)))
    for u, m, up in itertools.product(u_vals, (3.0, 4.0, 6.0), (0.001, 0.003)):
        g.append((REGLA_ROCIO, dict(umbral_suelto=u, margen_regla=m, umbral_pre=up)))
    g.append((CADA_HORA, {}))
    g.append((NUNCA, {}))
    return g


def resumen(rs):
    df = pd.DataFrame(rs)
    cols = ["perdida_suciedad_pct", "energia_neta_kwh", "ciclos_eds", "lavados", "agua_litros",
            "activaciones_inseguras", "usd", "energia_limpieza_kwh"]
    return df[cols].mean().to_dict()


def main():
    _init()
    salida = {}
    with Pool(os.cpu_count() or 2, initializer=_init) as pool:
        # ------------------------------------------------------------------ 1. políticas
        pol_res = {}
        for sitio in SITIOS:
            print(f"\n=== Sitio: {sitio} — ajustando políticas con años de entrenamiento ===")
            grid = rejilla()
            tareas = [(sitio, s, pol, prm, None) for pol, prm in grid for s in IA["semillas_entrenamiento"]]
            rs = pool.map(correr, tareas, chunksize=4)
            df = pd.DataFrame(rs)
            df["pol"] = [t[2] for t in df.tarea]
            df["prm"] = [t[3] for t in df.tarea]
            ag = df.groupby(["pol", "prm"]).usd.mean().reset_index()
            mejores = {}
            for pol in (FIJO, UMBRAL, IA_PRED, REGLA_ROCIO, CADA_HORA, NUNCA):
                sub = ag[ag.pol == str(pol)].sort_values("usd", ascending=False)
                mejores[pol] = json.loads(sub.prm.iloc[0])
            print("  parámetros elegidos:", {NOMBRE_POLITICA[k]: v for k, v in mejores.items()})
            # evaluación en años de prueba
            tareas = [(sitio, s, pol, prm, None) for pol, prm in mejores.items() for s in IA["semillas_prueba"]]
            tareas += [(sitio, s, "lavado_mensual", {}, None) for s in IA["semillas_prueba"]]
            rs = pool.map(correr, tareas, chunksize=2)
            tab = {}
            for pol in list(mejores) + ["lavado_mensual"]:
                sub = [r for r in rs if r["tarea"][2] == str(pol)]
                nombre = NOMBRE_POLITICA.get(pol, "Lavado mensual (sin EDS)")
                tab[nombre] = resumen(sub)
                tab[nombre]["params"] = mejores.get(pol, {})
                tab[nombre]["usd_por_anio"] = [round(r["usd"], 3) for r in sub]
            ref_usd = tab["Sin limpieza"]["usd"]
            for v in tab.values():
                v["ganancia_vs_sin_limpieza_usd"] = v["usd"] - ref_usd
            pol_res[sitio] = tab
            t = pd.DataFrame(tab).T[["perdida_suciedad_pct", "ciclos_eds", "lavados", "agua_litros",
                                     "activaciones_inseguras", "energia_neta_kwh", "ganancia_vs_sin_limpieza_usd"]]
            print(t.astype(float).round(3).to_string())
        salida["politicas"] = pol_res

        # ------------------------------------------------------------------ 2. seguridad
        print("\n=== Seguridad: v1 (solo temp <= 20 °C) vs v2, con y sin fallas de sensor ===")
        from sensores import Fallas
        par = pol_res["base"]["IA predictiva"]["params"]
        par_v1 = dict(par); par_v1["seguridad_v1"] = True
        tareas = []
        for s in IA["semillas_prueba"]:
            tareas.append(("base", s, IA_PRED, par, None))
            tareas.append(("base", s, IA_PRED, par_v1, None))
            tareas.append(("base", s, CADA_HORA, {}, None))
            tareas.append(("base", s, CADA_HORA, {"seguridad_v1": True}, None))
            for tipo in ("sht_congelado", "ntc_abierto", "afe_abierto"):
                for dia in (60.3, 200.6, 300.9):   # inicio a distintas horas del día
                    f = Fallas(sensor=tipo, sensor_inicio_dia=dia)
                    tareas.append(("base", s, CADA_HORA, {}, dict(fallas=f, fallas_key=f"{tipo}-{dia}")))
        rs = pool.map(correr, tareas, chunksize=2)
        seg = {"v2_ia": 0, "v1_ia": 0, "v2_cada_hora": 0, "v1_cada_hora": 0, "fallas_sensor": {}}
        anios = len(IA["semillas_prueba"])
        for r in rs:
            sitio, s, pol, prm, fk = r["tarea"]
            prm = json.loads(prm)
            if fk:
                d = seg["fallas_sensor"].setdefault(fk.split("-")[0], dict(casos=0, inseguras=0, detectada=0, dias_hasta_deteccion=[]))
                d["casos"] += 1
                d["inseguras"] += r["activaciones_inseguras"]
                if "falla_sensor" in r["primera_alerta"]:
                    d["detectada"] += 1
                    d["dias_hasta_deteccion"].append(round(r["primera_alerta"]["falla_sensor"] - r["fallas"]["sensor_dia"], 3))
            else:
                clave = ("v1" if prm.get("seguridad_v1") else "v2") + ("_cada_hora" if pol == str(CADA_HORA) else "_ia")
                seg[clave] += r["activaciones_inseguras"]
        for k in ("v2_ia", "v1_ia", "v2_cada_hora", "v1_cada_hora"):
            seg[k + "_por_anio"] = seg[k] / anios
        for d in seg["fallas_sensor"].values():
            hs = d.pop("dias_hasta_deteccion")
            d["horas_hasta_deteccion_max"] = round(max(hs) * 24, 1) if hs else None
            d["horas_hasta_deteccion_mediana"] = round(float(np.median(hs)) * 24, 1) if hs else None
        print(json.dumps(seg, indent=1))
        salida["seguridad"] = seg

        # ------------------------------------------------------------------ 3. alertas
        print("\n=== Alertas de mantenimiento (conector, térmica) ===")
        rng = np.random.default_rng(7)
        tareas = []
        for s in IA["semillas_prueba"]:
            tareas.append(("base", s, UMBRAL, {}, dict(fallas=Fallas(), fallas_key="sano", calc_v1_termica=True)))
            for j in range(4):
                fc = Fallas(conector_inicio_dia=float(rng.uniform(40, 220)),
                            conector_tau_dias=float(rng.uniform(*FA["tau_degradacion_dias"])))
                tareas.append(("base", s, UMBRAL, {}, dict(fallas=fc, fallas_key=f"con{j}")))
                fh = Fallas(hotspot_inicio_dia=float(rng.uniform(40, 300)), hotspot_dt=float(rng.uniform(*FA["hotspot_dt_max"])))
                tareas.append(("base", s, UMBRAL, {}, dict(fallas=fh, fallas_key=f"hot{j}", calc_v1_termica=True)))
        rs = pool.map(correr, tareas, chunksize=2)
        con, hot, sano = [], [], []
        for r in rs:
            fk = r["tarea"][4]
            pa = r["primera_alerta"]
            if fk == "sano":
                sano.append(dict(conector=pa.get("alerta_conector"), termica=pa.get("alerta_termica"),
                                 v1_conector=pa.get("alerta_v1_conector"), dias_v1_termica=r["dias_v1_termica"]))
            elif fk.startswith("con"):
                con.append(dict(real_alerta=r["dia_r_alerta_real"], real_falla=r["dia_r_falla_real"],
                                v2=pa.get("alerta_conector"), v1=pa.get("alerta_v1_conector"),
                                pronostico_dias=pa.get("dias_falla_conector_en_alerta")))
            else:
                hot.append(dict(inicio=r["fallas"]["hotspot"], dt=r["fallas"]["hotspot_dt"],
                                v2=pa.get("alerta_termica"), v1_primer_dia=r.get("primer_dia_v1_termica"),
                                dias_v1=r["dias_v1_termica"]))
        dfc = pd.DataFrame(con)
        dfc = dfc[dfc.real_alerta.notna()]
        dfc["retraso_v2"] = dfc.v2 - dfc.real_alerta
        dfc["retraso_v1"] = dfc.v1 - dfc.real_alerta
        dfc["antes_de_falla_v2"] = dfc.real_falla - dfc.v2
        dfc["antes_de_falla_v1"] = dfc.real_falla - dfc.v1
        dfc["error_pronostico"] = dfc.pronostico_dias - (dfc.real_falla - dfc.v2)
        dfh = pd.DataFrame(hot)
        dfh["retraso_v2"] = dfh.v2 - dfh.inicio
        ds = pd.DataFrame(sano)
        alertas = dict(
            conector=dict(
                casos=int(len(dfc)),
                v2_detectados=int(dfc.v2.notna().sum()), v1_detectados=int(dfc.v1.notna().sum()),
                v2_retraso_mediano_dias=float(dfc.retraso_v2.median()),
                v1_retraso_mediano_dias=float(dfc.retraso_v1.median()) if dfc.v1.notna().any() else None,
                v2_dias_antes_de_falla_mediana=float(dfc.antes_de_falla_v2.median()),
                v1_dias_antes_de_falla_mediana=float(dfc.antes_de_falla_v1.median()) if dfc.v1.notna().any() else None,
                v1_avisa_despues_de_la_falla=int((dfc.antes_de_falla_v1 < 0).sum()),
                pronostico_error_mediano_abs_dias=float(dfc.error_pronostico.abs().median()),
                pronostico_error_mediano_dias=float(dfc.error_pronostico.median()),
                detalle=dfc.round(2).to_dict("records"),
            ),
            termica=dict(
                casos=int(len(dfh)), v2_detectados=int(dfh.v2.notna().sum()),
                v2_retraso_mediano_dias=float(dfh.retraso_v2.median()),
                v2_retraso_max_dias=float(dfh.retraso_v2.max()),
                detalle=dfh.round(2).to_dict("records"),
            ),
            sanos=dict(
                anios=int(len(ds)),
                v2_falsas_conector=int(ds.conector.notna().sum()),
                v2_falsas_termica=int(ds.termica.notna().sum()),
                v1_falsas_conector=int(ds.v1_conector.notna().sum()),
                v1_dias_con_alarma_termica_por_anio=float(ds.dias_v1_termica.mean()),
            ),
        )
        print(json.dumps({k: {kk: vv for kk, vv in v.items() if kk != "detalle"} for k, v in alertas.items()}, indent=1))
        salida["alertas"] = alertas

        # ------------------------------------------------------------------ 4. tolvanera + lluvia ligera
        print("\n=== Tolvanera seguida de lluvia ligera (formación de lodo) ===")
        tareas = []
        for s in IA["semillas_prueba"]:
            for dia in (80, 150, 300):
                f = Fallas(lodo_dia=dia)
                for nombre in ("Horario fijo", "Umbral (reactiva)", "IA predictiva", "Regla de rocío", "Sin limpieza"):
                    pol = {v: k for k, v in NOMBRE_POLITICA.items()}[nombre]
                    tareas.append(("base", s, pol, pol_res["base"][nombre]["params"],
                                   dict(fallas=f, fallas_key=f"lodo{dia}")))
        rs = pool.map(correr, tareas, chunksize=2)
        lodo = {}
        for r in rs:
            nombre = NOMBRE_POLITICA[int(r["tarea"][2])]
            d = lodo.setdefault(nombre, dict(casos=0, lavados=0, perdida=[], ciclos=[], dias_a_orden=[]))
            d["casos"] += 1
            d["lavados"] += r["lavados"]
            d["perdida"].append(r["perdida_suciedad_pct"])
            d["ciclos"].append(r["ciclos_eds"])
        for d in lodo.values():
            d["perdida_media_pct"] = round(float(np.mean(d.pop("perdida"))), 3)
            d["ciclos_medios"] = round(float(np.mean(d.pop("ciclos"))), 1)
            d.pop("dias_a_orden")
            d["lavados_por_evento"] = round(d["lavados"] / d["casos"], 2)
        print(json.dumps(lodo, indent=1, ensure_ascii=False))
        salida["lodo"] = lodo

    json.dump(salida, open(os.path.join(RES, "resultados_v2.json"), "w"), indent=1, ensure_ascii=False, default=float)
    print("\nGuardado resultados/resultados_v2.json")


if __name__ == "__main__":
    main()
