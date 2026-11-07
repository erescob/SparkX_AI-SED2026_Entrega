"""
entrenar_rocio.py — Predictor de película de agua (rocío/lluvia) a 8 horas
=========================================================================

Pregunta que responde el modelo, cada 15 minutos:
   "Con lo que mido ahora y en las últimas 3 h, ¿habrá agua sobre el vidrio
    en algún momento de las próximas 8 h?"
   (8 h porque la capa de seguridad ya bloquea la AT ~4.5 h antes del rocío;
    para limpiar ANTES hay que avisar con más anticipación que eso)

¿Por qué esto SÍ justifica IA y la tabla del v1 no?
   * La etiqueta es del FUTURO: no se puede leer directamente de un sensor.
   * Las clases se TRASLAPAN (misma HR y temperatura pueden o no terminar en
     rocío según cómo evolucione la noche): no hay 100 %, hay que medir.
   * Hay una línea base honesta contra la cual comparar: reglas fijas de
     margen de rocío y de humedad.

Validación: se entrena con 6 años sintéticos y se prueba con 4 años
DISTINTOS (otras semillas de clima). Nunca se mezclan horas del mismo año
entre entrenamiento y prueba (serían casi iguales y el resultado se inflaría).

Salidas:
  resultados/modelo_rocio.json         (árbol en arreglos, lo usa controlador.py)
  firmware/sx_modelo_rocio.h           (el mismo árbol para el STM32)
  resultados/metricas_rocio.json, figuras de evaluación
  datos/dataset_rocio_v2.csv.gz        (dataset para quien quiera reentrenar)
"""
import json
import os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from sklearn.tree import DecisionTreeClassifier
from sklearn.ensemble import RandomForestClassifier, HistGradientBoostingClassifier
from sklearn.metrics import (precision_score, recall_score, f1_score, roc_auc_score,
                             average_precision_score)

from config import IA, AGUA, SEGURIDAD as SG, DT_MIN
from sensores import construir_escenario, etiqueta_rocio_futuro
from caracteristicas import caracteristicas_vectorizadas, NOMBRES, F32

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(RAIZ, "resultados")
FIG = os.path.join(RES, "figuras")
FW = os.path.join(RAIZ, "firmware")
DAT = os.path.join(RAIZ, "datos")


def dataset_de(semillas):
    filas = []
    for s in semillas:
        esc = construir_escenario(s)
        X = caracteristicas_vectorizadas(esc.t_aire_m, esc.hr_m, esc.tv_m, esc.clima.hora.values)
        y = etiqueta_rocio_futuro(esc.w)
        moj_ahora = esc.w >= AGUA["w_mojado"]
        df = pd.DataFrame(X, columns=NOMBRES)
        df["y"] = y
        df["mojado_ahora"] = moj_ahora
        df["lluvia_mm"] = esc.clima.lluvia_mm.values
        df["w_real"] = esc.w
        df["semilla"] = s
        df["k"] = np.arange(len(df))
        df["fecha"] = esc.clima.index
        filas.append(df)
    return pd.concat(filas, ignore_index=True)


def lit_f32(v):
    """Literal float de C que reproduce exactamente el float32 (siempre con punto decimal)."""
    t = f"{float(np.float32(v)):.9g}"
    if "." not in t and "e" not in t:
        t += ".0"
    return t + "f"


def umbral_f32_seguro(thr64):
    """Convierte el umbral a float32 sin cambiar ninguna decisión x<=thr para x float32."""
    t = np.float32(thr64)
    if float(t) > thr64:
        t = np.nextafter(t, np.float32(-np.inf))
    return t


def exportar_arbol(dt, ruta_json, ruta_h):
    t = dt.tree_
    feat = [int(f) if f >= 0 else -1 for f in t.feature]
    thr = [float(umbral_f32_seguro(v)) if f >= 0 else 0.0 for f, v in zip(t.feature, t.threshold)]
    val = t.value[:, 0, :]
    prob = (val[:, 1] / val.sum(axis=1)).astype(np.float32)
    d = dict(feature=feat, threshold=thr, left=[int(v) for v in t.children_left],
             right=[int(v) for v in t.children_right], prob=[float(p) for p in prob],
             nombres=NOMBRES, n_nodos=int(t.node_count))
    json.dump(d, open(ruta_json, "w"), indent=1)

    n = t.node_count
    def arr(tipo, nombre, vals, fmt):
        cuerpo = ",".join(fmt(v) for v in vals)
        return f"static const {tipo} {nombre}[SX_ARBOL_NODOS] = {{{cuerpo}}};\n"
    h = f"""/* =========================================================================
 * sx_modelo_rocio.h  —  GENERADO por python/entrenar_rocio.py  (NO editar a mano)
 * Árbol de decisión: P(película de agua en las próximas {IA['horizonte_h']:.0f} h)
 * Profundidad {dt.get_depth()}, {dt.get_n_leaves()} hojas, {n} nodos.
 * Memoria: {n * (2 + 4 + 2 + 2 + 4)} bytes en flash. Recorrido: <= {dt.get_depth()} comparaciones.
 * Entradas (en este orden): {", ".join(NOMBRES)}
 * ========================================================================= */
#ifndef SX_MODELO_ROCIO_H
#define SX_MODELO_ROCIO_H
#include <stdint.h>
#define SX_ARBOL_NODOS {n}
#define SX_N_CARACT {len(NOMBRES)}
"""
    h += arr("int16_t", "SX_ARBOL_FEAT", feat, str)
    h += arr("float", "SX_ARBOL_THR", thr, lit_f32)
    h += arr("int16_t", "SX_ARBOL_IZQ", d["left"], str)
    h += arr("int16_t", "SX_ARBOL_DER", d["right"], str)
    h += arr("float", "SX_ARBOL_PROB", prob, lit_f32)
    h += "#endif\n"
    open(ruta_h, "w").write(h)
    return d


def anticipacion(df, prob, umbral, permitido):
    """Para cada inicio de mojado (seco->mojado) en el set de prueba:
       - ¿hubo alerta en las 3 h previas?  (detección)
       - ¿hubo al menos un paso con alerta Y la capa de seguridad aún permitiendo AT?
         (= oportunidad real de limpiar ANTES de que se moje)
       - minutos de anticipación de la primera alerta"""
    k = int(IA["horizonte_h"] * 60 / DT_MIN)
    res = []
    for s, g in df.groupby("semilla"):
        moj = g.mojado_ahora.values
        al = prob[g.index] >= umbral
        perm = permitido[g.index]
        inicios = np.where((moj[1:]) & (~moj[:-1]))[0] + 1
        for i in inicios:
            a = max(0, i - k)
            ventana = al[a:i]
            lluvia = g.lluvia_mm.values[i] > 0
            if ventana.any():
                primera = np.argmax(ventana)
                res.append(dict(detectado=True, oportunidad=bool((ventana & perm[a:i]).any()),
                                min_antes=(len(ventana) - primera) * DT_MIN, lluvia=lluvia))
            else:
                res.append(dict(detectado=False, oportunidad=False, min_antes=0, lluvia=lluvia))
    r = pd.DataFrame(res)
    out = {}
    for nombre, sub in (("todos", r), ("rocio", r[~r.lluvia]), ("lluvia", r[r.lluvia])):
        if len(sub) == 0:
            continue
        out[nombre] = dict(eventos=int(len(sub)), detectados_pct=round(100 * sub.detectado.mean(), 1),
                           con_oportunidad_pct=round(100 * sub.oportunidad.mean(), 1),
                           anticipacion_mediana_min=float(sub[sub.detectado].min_antes.median()) if sub.detectado.any() else 0.0)
    return out


def main():
    os.makedirs(FIG, exist_ok=True)
    print("Generando escenarios de entrenamiento y prueba...")
    tr = dataset_de(IA["semillas_entrenamiento"])
    te = dataset_de(IA["semillas_prueba"])
    # se entrena y evalúa solo cuando el vidrio está SECO ahora (la pregunta es si se mojará)
    trs = tr[~tr.mojado_ahora].reset_index(drop=True)
    tes = te[~te.mojado_ahora].reset_index(drop=True)
    Xtr, ytr = trs[NOMBRES].values.astype(F32), trs.y.values
    Xte, yte = tes[NOMBRES].values.astype(F32), tes.y.values
    print(f"Entrenamiento: {len(trs)} muestras ({ytr.mean()*100:.1f}% positivas) | "
          f"Prueba: {len(tes)} muestras ({yte.mean()*100:.1f}% positivas)")

    dt = DecisionTreeClassifier(max_depth=IA["profundidad"], min_samples_leaf=IA["min_hoja"],
                                class_weight="balanced", random_state=42).fit(Xtr, ytr)
    rf = RandomForestClassifier(n_estimators=150, min_samples_leaf=20, class_weight="balanced",
                                n_jobs=-1, random_state=42).fit(Xtr, ytr)
    gb = HistGradientBoostingClassifier(max_iter=300, learning_rate=0.05, class_weight="balanced",
                                        random_state=42).fit(Xtr, ytr)

    # umbral de probabilidad del árbol: el que maximiza F1 en ENTRENAMIENTO
    p_tr = dt.predict_proba(Xtr)[:, 1]
    cand = np.linspace(0.2, 0.9, 29)
    f1s = [f1_score(ytr, p_tr >= c) for c in cand]
    u = float(cand[int(np.argmax(f1s))])

    # reglas base (sin IA) evaluadas igual
    margen = tes.margen_rocio.values
    hr = tes.hr.values
    reglas = {
        "Regla: margen de rocío <= 2 °C": margen <= 2.0,
        "Regla: margen de rocío <= 4 °C": margen <= 4.0,
        "Regla: HR >= 80 %": hr >= 80.0,
    }
    def met(y, pred, score=None):
        d = dict(precision=round(precision_score(y, pred, zero_division=0), 3),
                 recall=round(recall_score(y, pred), 3), f1=round(f1_score(y, pred), 3))
        if score is not None:
            d["auc_roc"] = round(roc_auc_score(y, score), 3)
            d["auc_pr"] = round(average_precision_score(y, score), 3)
        return d

    p_dt = dt.predict_proba(Xte)[:, 1]
    tabla = {f"Árbol (IA, en el MCU) p>={u:.2f}": met(yte, p_dt >= u, p_dt),
             "Árbol p>=0.50": met(yte, p_dt >= 0.5, p_dt),
             "Random Forest 150 árboles (referencia, no cabe bien en MCU)": met(yte, rf.predict_proba(Xte)[:, 1] >= 0.5, rf.predict_proba(Xte)[:, 1]),
             "Gradient Boosting (referencia)": met(yte, gb.predict_proba(Xte)[:, 1] >= 0.5, gb.predict_proba(Xte)[:, 1])}
    for nombre, pred in reglas.items():
        tabla[nombre] = met(yte, pred, -margen if "margen" in nombre else hr)
    print("\n=== Métricas en 4 años NO vistos (vidrio seco ahora; ¿se moja en 8 h?) ===")
    print(pd.DataFrame(tabla).T.to_string())

    # anticipación y oportunidad de limpieza preventiva (con todo el set de prueba, incluye pasos mojados)
    Xall = te[NOMBRES].values.astype(F32)
    p_all = dt.predict_proba(Xall)[:, 1]
    permitido = (te.margen_rocio.values >= SG["margen_rocio_min"]) & (te.hr.values <= SG["hr_max"])
    ant = {f"Árbol p>={u:.2f}": anticipacion(te, p_all, u, permitido),
           "Regla margen <= 4 °C": anticipacion(te, (te.margen_rocio.values <= 4.0).astype(float), 0.5, permitido),
           "Regla margen <= 2 °C": anticipacion(te, (te.margen_rocio.values <= 2.0).astype(float), 0.5, permitido),
           "Regla HR >= 80 %": anticipacion(te, (te.hr.values >= 80).astype(float), 0.5, permitido)}
    print("\n=== Eventos de mojado: detección, oportunidad real de limpiar antes, anticipación ===")
    for k_, v in ant.items():
        print(f"  {k_:24s} rocío: {v.get('rocio')}\n  {'':24s} lluvia: {v.get('lluvia')}")

    imp = dict(sorted(zip(NOMBRES, dt.feature_importances_.round(3)), key=lambda z: -z[1]))
    print("\nImportancia de variables (árbol):", imp)

    # exportar
    d = exportar_arbol(dt, os.path.join(RES, "modelo_rocio.json"), os.path.join(FW, "sx_modelo_rocio.h"))
    # verificación: el árbol exportado (float32) da lo mismo que sklearn
    from controlador import Arbol
    A = Arbol(os.path.join(RES, "modelo_rocio.json"))
    p_exp = np.array([A.prob(x) for x in Xte[:20000]])
    iguales = np.mean(np.isclose(p_exp, p_dt[:20000], atol=1e-6)) * 100
    print(f"\nÁrbol exportado vs sklearn (20,000 muestras): {iguales:.2f}% idénticas | nodos={d['n_nodos']}")

    json.dump(dict(umbral_arbol=u, metricas=tabla, anticipacion=ant, importancias=imp,
                   n_entrenamiento=int(len(trs)), n_prueba=int(len(tes)),
                   positivos_entrenamiento_pct=round(100 * ytr.mean(), 2),
                   positivos_prueba_pct=round(100 * yte.mean(), 2),
                   profundidad=int(dt.get_depth()), hojas=int(dt.get_n_leaves()),
                   exportado_identico_pct=float(iguales)),
              open(os.path.join(RES, "metricas_rocio.json"), "w"), indent=1, ensure_ascii=False)

    # dataset para el equipo (cada hora para que pese poco)
    os.makedirs(DAT, exist_ok=True)
    full = pd.concat([tr.assign(conjunto="entrenamiento"), te.assign(conjunto="prueba")])
    full[full.k % 4 == 0].to_csv(os.path.join(DAT, "dataset_rocio_v2_horario.csv.gz"), index=False,
                                 float_format="%.4f", compression="gzip")

    # figuras
    fig, ax = plt.subplots(figsize=(7, 4))
    nombres = list(imp.keys())[::-1]
    ax.barh(nombres, [imp[n] for n in nombres], color="#2a6f97")
    ax.set_xlabel("Importancia (Gini)")
    ax.set_title("Predictor de rocío a 8 h: qué variables usa")
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "importancia_rocio.png"), dpi=160); plt.close(fig)
    return u


if __name__ == "__main__":
    main()
