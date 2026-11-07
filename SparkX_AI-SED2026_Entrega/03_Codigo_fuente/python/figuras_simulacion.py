"""
figuras_simulacion.py — Figuras del reporte de simulación y pruebas.

  fig/demo_rocio_nodo2.png   lo que debe verse en Proteus (escenario del demo, nodo 2)
  fig/red_28dias.png         red de 6 tarjetas, 28 días, PCB actual vs hardware completo
  fig/deriva_reloj.png       tolerancia del enlace de 1 hilo a la diferencia de reloj
"""
import csv
import json
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(RAIZ, "resultados")
FIG = os.path.join(RES, "figuras")
os.makedirs(FIG, exist_ok=True)

AZUL, NARANJA, VERDE, ROJO, GRIS = "#2a6fb0", "#e07b28", "#3a9d5d", "#c0392b", "#8a8a8a"
plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.grid": True, "grid.alpha": 0.25, "font.family": "DejaVu Sans"})

PAT = re.compile(r"\[N(\d)\] d(\d+) (\d+):(\d+) Ta=(\S+) HR=(\S+) Tv=(\S+) marg=(\S+) AT:(\w+)\(0x([0-9A-F]+)\) p=(\S+)")


def leer_log(ruta):
    filas = []
    for ln in open(ruta):
        m = PAT.match(ln)
        if not m:
            continue
        d, h, mi = int(m[2]), int(m[3]), int(m[4])
        k = d * 96 + h * 4 + mi // 15
        filas.append(dict(k=k, ta=float(m[5]), hr=float(m[6]), tv=float(m[7]), marg=float(m[8]),
                          at=m[9], bits=int(m[10], 16), p=float(m[11]), pide="PIDE_TURNO" in ln))
    return filas


def turnos(ruta, nodo):
    ks = []
    for ln in open(ruta):
        if ln.startswith('{"t":"turno"'):
            d = json.loads(ln)
            if d["n"] == nodo:
                ks.append(d["k"])
    return ks


def agua(nodo):
    r = csv.DictReader(ln for ln in open(os.path.join(RAIZ, "datos", f"red_nodo{nodo}.csv")) if not ln.startswith("#"))
    return {int(x["k"]): int(x["agua"]) for x in r}


def hora(k, k0):
    return (k - k0) / 4.0


def fig_demo():
    d = os.path.join(RES, "sim_demo_rocio")
    F = [f for f in leer_log(os.path.join(d, "nodo2.log")) if f["marg"] == f["marg"]]
    F = [f for f in F if f["k"] <= 1512 + 80]
    k0 = 1512
    A = agua(2)
    T = [k for k in turnos(os.path.join(d, "gateway_nodo1.log"), 2) if k <= k0 + 80]
    x = [hora(f["k"], k0) for f in F]
    fig, ax = plt.subplots(3, 1, figsize=(7.2, 5.6), sharex=True, gridspec_kw={"height_ratios": [1.2, 1.2, 0.7]})

    def sombra(a):
        dentro = False
        for k in range(k0, k0 + 81):
            if A.get(k, 0) and not dentro:
                ini, dentro = k, True
            if dentro and not A.get(k, 0):
                a.axvspan(hora(ini, k0), hora(k, k0), color="#9cc3e6", alpha=0.45, lw=0)
                dentro = False

    a = ax[0]
    sombra(a)
    a.plot(x, [f["marg"] for f in F], color=AZUL, lw=1.6, label="margen al rocío (°C)")
    a.axhline(4, color=NARANJA, ls="--", lw=1, label="4 °C (respaldo sin AFE)")
    a.axhline(2, color=ROJO, ls="--", lw=1, label="2 °C (bloqueo duro)")
    a2 = a.twinx()
    a2.plot(x, [f["hr"] for f in F], color=GRIS, lw=1.1, label="HR (%)")
    a2.set_ylabel("HR (%)", color=GRIS)
    a2.grid(False)
    a.set_ylabel("margen (°C)")
    h1, l1 = a.get_legend_handles_labels()
    h2, l2 = a2.get_legend_handles_labels()
    a.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=6.5, ncol=2, framealpha=0.9)
    a.set_ylim(-5, 52)
    a.set_title("Escenario del demo de Proteus (nodo 2): día 15 18:00 → día 16 14:00", fontsize=10)

    a = ax[1]
    sombra(a)
    a.step(x, [f["p"] for f in F], where="post", color=VERDE, lw=1.6, label="p_rocío (IA, árbol de 125 nodos)")
    a.axhline(0.60, color=ROJO, ls=":", lw=1.2, label="umbral 0.60")
    for k in T:
        a.axvline(hora(k, k0), color=NARANJA, lw=2.2, alpha=0.9)
    a.text(hora(T[0], k0) + 0.2, 0.05, "limpieza\nANTES del rocío\n(19:45)", color=NARANJA, fontsize=7.5)
    if len(T) > 1:
        a.text(hora(T[1], k0) - 0.15, 0.24, "limpieza diaria\n(vidrio seco)", color=NARANJA, fontsize=7.5, ha="right")
    if len(T) > 2:
        a.text(hora(T[2], k0) + 0.2, 0.80, "IA p≥0.6 de día:\nfalsa alarma\n(limpieza extra)", color=NARANJA, fontsize=7)
    a.text(hora(1557, k0) + 0.2, 0.62, "agua en el vidrio\n(rocío real)", color=AZUL, fontsize=7.5)
    a.set_ylim(-0.02, 1.05)
    a.set_ylabel("probabilidad")
    a.legend(loc="upper left", fontsize=7)

    a = ax[2]
    sombra(a)
    b = [f["bits"] for f in F]
    a.step(x, [1 if v else 0 for v in b], where="post", color=ROJO, lw=1.5)
    a.fill_between(x, [1 if v else 0 for v in b], step="post", color=ROJO, alpha=0.15)
    a.set_yticks([0, 1], ["permitida", "bloqueada"])
    a.set_ylabel("alta tensión")
    a.set_xlabel("horas desde el inicio del demo (1 h = 16 s de simulación en Proteus)")
    tick = list(range(0, 21, 2))
    a.set_xticks(tick, [f"{(18 + t) % 24:02d}:00" for t in tick])
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "demo_rocio_nodo2.png"), dpi=200)
    plt.close(fig)
    return dict(pre_rocio_k=T[0], inicio_agua_k=1557, horas_anticipacion=(1557 - T[0]) / 4.0,
                pasos_bloqueados=sum(1 for v in b if v), activaciones_con_agua=sum(1 for k in T if A.get(k, 0)))


def fig_red():
    base = json.load(open(os.path.join(RES, "sim_red", "resumen.json")))
    comp = json.load(open(os.path.join(RES, "sim_red_completo", "resumen.json")))
    ids = [n["id"] for n in base["nodos"]]
    fig, ax = plt.subplots(1, 2, figsize=(7.2, 2.7))
    w = 0.38
    ax[0].bar([i - w / 2 for i in ids], [n["inicios_eds"] for n in base["nodos"]], w, color=NARANJA, label="PCB actual")
    ax[0].bar([i + w / 2 for i in ids], [n["inicios_eds"] for n in comp["nodos"]], w, color=VERDE, label="hardware v2 completo")
    ax[0].set_title("Ciclos EDS por tarjeta en 28 días (auditados)", fontsize=9)
    ax[0].set_xlabel("tarjeta")
    ax[0].legend(fontsize=7, ncol=2, loc="upper center")
    ax[0].set_ylim(0, 68)
    ax[1].bar([i - w / 2 for i in ids], [n["tramas_ok"] for n in base["nodos"]], w, color=AZUL, label="tramas OK")
    ax[1].bar([i + w / 2 for i in ids], [n["reenviadas"] for n in base["nodos"]], w, color=GRIS, label="reenviadas")
    ax[1].set_title("Tráfico de red (PCB actual), 0 CRC malos", fontsize=9)
    ax[1].set_xlabel("tarjeta")
    ax[1].legend(fontsize=7, ncol=2, loc="upper center")
    ax[1].set_ylim(0, 12800)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "red_28dias.png"), dpi=200)
    plt.close(fig)


def fig_deriva(log):
    xs, ys = [], []
    for ln in open(log):
        m = re.match(r"\s+(-?\d+\.\d) %\s+(\d+)", ln)
        if m:
            xs.append(float(m[1]))
            ys.append(int(m[2]) / 2.0)
    fig, a = plt.subplots(figsize=(3.5, 2.4))
    a.plot(xs, ys, "o-", color=AZUL, ms=3.5)
    a.axvspan(-2.5, 2.5, color=VERDE, alpha=0.12, label="HSI ±1 % entre 2 tarjetas")
    a.set_xlabel("diferencia de reloj entre tarjetas (%)")
    a.set_ylabel("tramas recibidas OK (%)")
    a.legend(fontsize=7, loc="lower center")
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "deriva_reloj.png"), dpi=200)
    plt.close(fig)
    return dict(zip(xs, ys))


if __name__ == "__main__":
    r = fig_demo()
    fig_red()
    dv = fig_deriva("/home/claude/logs/02_pruebas.txt" if len(os.sys.argv) < 2 else os.sys.argv[1])
    r["deriva"] = dv
    json.dump(r, open(os.path.join(RES, "demo_esperado.json"), "w"), indent=1)
    print(json.dumps(r, indent=1))
