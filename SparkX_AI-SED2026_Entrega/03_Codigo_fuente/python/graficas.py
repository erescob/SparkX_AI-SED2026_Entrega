"""
graficas.py — Figuras del informe (todas salen de resultados/*.json o de la simulación)
"""
import json
import os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from config import CLIMA, AGUA
from clima import generar_clima
from sensores import construir_escenario, Fallas
from controlador import Arbol, Controlador, IA_PRED
from simulador import simular

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(RAIZ, "resultados")
FIG = os.path.join(RES, "figuras")
AZUL, NARANJA, AQUA, AMARILLO, VIOLETA, ROJO = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#6250d6", "#e34948"
GRIS, TEXTO, TEXTO2 = "#b9b8b2", "#0b0b0b", "#52514e"
plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.edgecolor": GRIS, "axes.labelcolor": TEXTO2, "xtick.color": TEXTO2,
                     "ytick.color": TEXTO2, "axes.titleweight": "bold", "axes.titlesize": 10,
                     "grid.color": "#e6e5e0", "grid.linewidth": 0.6})


def fig_clima():
    rows = []
    for s in range(8):
        c = generar_clima(s); g = c.groupby("mes")
        rows.append(pd.DataFrame({"tmax": g.t_aire.apply(lambda x: x.resample("D").max().mean()),
                                  "tmin": g.t_aire.apply(lambda x: x.resample("D").min().mean()),
                                  "hr": g.hr.mean(), "lluvia": g.lluvia_mm.sum()}))
    m = sum(rows) / len(rows)
    meses = np.arange(1, 13)
    fig, ax = plt.subplots(1, 3, figsize=(10, 2.8))
    ax[0].plot(meses, CLIMA["tmax"], color=GRIS, lw=2, label="Normal 1991-2020")
    ax[0].plot(meses, m.tmax, color=NARANJA, lw=2, label="Simulado (8 años)")
    ax[0].plot(meses, CLIMA["tmin"], color=GRIS, lw=2)
    ax[0].plot(meses, m.tmin, color=AZUL, lw=2)
    ax[0].set_title("Temperatura máx./mín. (°C)"); ax[0].legend(frameon=False, fontsize=7)
    ax[1].plot(meses, CLIMA["hr_media"], color=GRIS, lw=2); ax[1].plot(meses, m.hr, color=AQUA, lw=2)
    ax[1].set_title("Humedad relativa media (%)")
    ax[2].bar(meses - 0.2, CLIMA["lluvia_mm"], 0.4, color=GRIS); ax[2].bar(meses + 0.2, m.lluvia, 0.4, color=AZUL)
    ax[2].set_title("Lluvia mensual (mm)")
    for a in ax:
        a.set_xticks(meses); a.grid(axis="y")
    fig.suptitle("Validación del generador de clima contra las normales de Hermosillo (gris)", fontsize=10)
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "clima_validacion.png"), dpi=170); plt.close(fig)


def fig_linea_tiempo(par):
    """3 días de invierno con rocío + una tolvanera seguida de lluvia ligera."""
    esc = construir_escenario(100, Fallas(lodo_dia=20))
    A = Arbol(os.path.join(RES, "modelo_rocio.json"))
    r = simular(esc, Controlador(A, IA_PRED, par, esc.sesgos["c0"], esc.sesgos["gan_cal"]), registrar=True)
    lg = r["log"]; c = esc.clima
    a, b = 18 * 96, 22 * 96
    t = lg.index[a:b]
    fig, ax = plt.subplots(3, 1, figsize=(10, 6.2), sharex=True)
    ax[0].plot(t, c.t_aire.values[a:b], color=NARANJA, lw=1.6, label="Aire")
    ax[0].plot(t, esc.tv[a:b], color=AZUL, lw=1.6, label="Vidrio")
    ax[0].plot(t, c.t_rocio.values[a:b], color=VIOLETA, lw=1.6, ls="--", label="Punto de rocío")
    ax[0].set_ylabel("°C"); ax[0].legend(frameon=False, ncol=3, fontsize=7, loc="upper left")
    ax[0].set_title("El rocío aparece cuando el VIDRIO baja del punto de rocío (no a una temperatura fija)")
    bloq = ~lg.permiso.values[a:b]
    ax[1].fill_between(t, 0, 1, where=bloq, color="#f3d2c7", step="post", label="AT bloqueada (seguridad)")
    ax[1].plot(t, np.minimum(esc.w[a:b] / 30, 1), color=AQUA, lw=1.6, label="Agua en el vidrio (relativa)")
    ax[1].plot(t, lg.p_rocio.values[a:b], color=VIOLETA, lw=1.4, label="IA: P(agua en 8 h)")
    ax[1].set_ylim(0, 1.05); ax[1].legend(frameon=False, ncol=3, fontsize=7, loc="upper left")
    ax[1].set_title("La IA avisa horas antes; la capa de seguridad bloquea la alta tensión antes de que se moje")
    ax[2].plot(t, 100 * (1 - lg.sr.values[a:b]), color=TEXTO2, lw=1.6, label="Pérdida real por suciedad")
    ax[2].plot(t, 100 * (lg.suelto_est.values[a:b] + lg.costra_est.values[a:b]), color=AZUL, lw=1.4,
               label="Estimada por el MCU")
    li = np.where(lg.limpiar.values[a:b])[0]
    ax[2].scatter(t[li], 100 * (1 - lg.sr.values[a:b][li]), color=NARANJA, s=22, zorder=3, label="Ciclo EDS")
    ax[2].set_ylabel("%"); ax[2].legend(frameon=False, ncol=3, fontsize=7, loc="upper left")
    ax[2].set_title("Tolvanera (17:00) y lluvia ligera (19:00): se limpia antes de que el polvo se vuelva lodo")
    for x in ax:
        x.grid(axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "linea_tiempo.png"), dpi=170); plt.close(fig)


def fig_politicas(R):
    sitios = [("base", "Sitio base (Hermosillo)"), ("polvoso", "Sitio polvoso (4× depósito)"),
              ("cementacion_alta", "Cementación alta (2.5×)")]
    orden = ["Sin limpieza", "Lavado mensual (sin EDS)", "Horario fijo", "Umbral (reactiva)",
             "Regla de rocío", "IA predictiva", "Cada hora"]
    corto = ["Sin\nlimpiar", "Lavado\nmensual", "Horario\nfijo", "Umbral", "Regla\nrocío", "IA", "Cada\nhora"]
    fig, ax = plt.subplots(2, 3, figsize=(11, 5.4))
    for j, (s, tit) in enumerate(sitios):
        tab = R["politicas"][s]
        perd = [tab[o]["perdida_suciedad_pct"] for o in orden]
        cic = [tab[o]["ciclos_eds"] for o in orden]
        col = [AZUL if o == "IA predictiva" else GRIS for o in orden]
        ax[0, j].bar(range(7), perd, color=col)
        for i, v in enumerate(perd):
            ax[0, j].text(i, v, f"{v:.2f}", ha="center", va="bottom", fontsize=6.5, color=TEXTO2)
        ax[0, j].set_title(tit); ax[0, j].set_xticks(range(7), corto, fontsize=6.5)
        ax[1, j].bar(range(7), cic, color=col)
        ax[1, j].set_yscale("symlog", linthresh=100)
        for i, v in enumerate(cic):
            ax[1, j].text(i, max(v, 1), f"{v:.0f}", ha="center", va="bottom", fontsize=6.5, color=TEXTO2)
        ax[1, j].set_xticks(range(7), corto, fontsize=6.5)
        for a in ax[:, j]:
            a.grid(axis="y")
    ax[0, 0].set_ylabel("Pérdida anual por suciedad (%)")
    ax[1, 0].set_ylabel("Ciclos de alta tensión / año")
    fig.suptitle("Políticas ajustadas con 6 años y evaluadas en 4 años no vistos (promedio)", fontsize=10)
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "politicas.png"), dpi=170); plt.close(fig)


def fig_seguridad_alertas(R):
    seg, al = R["seguridad"], R["alertas"]
    fig, ax = plt.subplots(1, 3, figsize=(10.5, 2.9))
    et = ["v1\npolítica IA", "v2\npolítica IA", "v1\ncada hora", "v2\ncada hora"]
    vals = [seg["v1_ia_por_anio"], seg["v2_ia_por_anio"], seg["v1_cada_hora_por_anio"], seg["v2_cada_hora_por_anio"]]
    ax[0].bar(range(4), vals, color=[ROJO, AZUL, ROJO, AZUL])
    for i, v in enumerate(vals):
        ax[0].text(i, v, f"{v:.1f}", ha="center", va="bottom", fontsize=7, color=TEXTO2)
    ax[0].set_yscale("symlog", linthresh=1); ax[0].set_xticks(range(4), et, fontsize=7)
    ax[0].set_title("Activaciones de AT con agua / año")
    dc = pd.DataFrame(al["conector"]["detalle"])
    ax[1].scatter(np.zeros(len(dc)) + np.random.default_rng(0).normal(0, 0.05, len(dc)), dc.antes_de_falla_v1,
                  color=ROJO, s=14, label="v1 (V12 <= 11.13 V)")
    ax[1].scatter(np.ones(len(dc)) + np.random.default_rng(1).normal(0, 0.05, len(dc)), dc.antes_de_falla_v2,
                  color=AZUL, s=14, label="v2 (R bajo carga)")
    ax[1].axhline(0, color=TEXTO2, lw=0.8)
    ax[1].set_xticks([0, 1], ["v1", "v2"]); ax[1].set_ylabel("días antes de la falla (+) / después (−)")
    ax[1].set_title("Conector: anticipación del aviso")
    sanos = al["sanos"]
    ax[2].bar([0, 1], [sanos["v1_dias_con_alarma_termica_por_anio"], sanos["v2_falsas_termica"] / max(sanos["anios"], 1)],
              color=[ROJO, AZUL])
    ax[2].set_xticks([0, 1], ["v1 (T > 45 °C)", "v2 (residuo)"])
    ax[2].set_title("Térmica: falsas alarmas / año (panel sano)")
    for a in ax:
        a.grid(axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "seguridad_alertas.png"), dpi=170); plt.close(fig)


def fig_rocio(M):
    filas = [(k, v) for k, v in M["metricas"].items() if "referencia" not in k and "p>=0.50" not in k]
    nombres = [k.replace("Árbol (IA, en el MCU)", "Árbol IA").replace("Regla: ", "") for k, _ in filas]
    fig, ax = plt.subplots(figsize=(7.5, 2.8))
    x = np.arange(len(filas)); w = 0.26
    for i, (m, col, et) in enumerate((("precision", AZUL, "Precisión"), ("recall", NARANJA, "Recall"), ("f1", AQUA, "F1"))):
        ax.bar(x + (i - 1) * w, [v[m] for _, v in filas], w, color=col, label=et)
    ax.set_xticks(x, nombres, fontsize=7); ax.set_ylim(0, 1); ax.grid(axis="y")
    ax.legend(frameon=False, ncol=3, fontsize=7)
    ax.set_title("¿Se mojará el vidrio en 8 h? — 4 años no vistos")
    fig.tight_layout(); fig.savefig(os.path.join(FIG, "rocio_vs_reglas.png"), dpi=170); plt.close(fig)


def main():
    os.makedirs(FIG, exist_ok=True)
    R = json.load(open(os.path.join(RES, "resultados_v2.json")))
    M = json.load(open(os.path.join(RES, "metricas_rocio.json")))
    fig_clima()
    fig_linea_tiempo(R["politicas"]["base"]["IA predictiva"]["params"])
    fig_politicas(R)
    fig_seguridad_alertas(R)
    fig_rocio(M)
    print("figuras listas en resultados/figuras/")


if __name__ == "__main__":
    main()
