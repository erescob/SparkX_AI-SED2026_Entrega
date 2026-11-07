"""
simulador.py — Lazo cerrado: planta física  <->  controlador (firmware)
======================================================================

Cada 15 min:
  1. Se generan las lecturas de sensores a partir del estado REAL.
  2. El controlador decide (con lo que mide, no con la verdad).
  3. Se aplica la decisión a la física (si limpia con agua en el vidrio,
     se cuenta como ACTIVACIÓN INSEGURA; la meta es que sea siempre 0).
  4. Avanzan la suciedad, el agua y la energía.

El controlador puede ser el espejo Python (controlador.Controlador) o el
firmware C compilado (sil_validacion.ControladorC): tienen la misma interfaz.
"""
import numpy as np
import pandas as pd

from config import AGUA, EDS, LAVADO, PANEL, PASOS_DIA, DT_H, ALERTAS as AL
from fisica import Suciedad, potencia_w, eficiencia_eds
from sensores import SensoresDinamicos
from controlador import PASO_PRUEBA


def simular(esc, controlador, lavado_periodico_dias=None, usar_eds=True, registrar=False):
    clima = esc.clima
    mes = clima.mes.values; poa = clima.poa.values; hr_real = clima.hr.values
    viento = clima.viento.values; polvo = clima.polvo_evento_gm2.values
    fin_lluvia = clima.evento_lluvia_fin_mm.values
    w = esc.w; tv = esc.tv
    sd = SensoresDinamicos(esc)
    panel, ref = Suciedad(), Suciedad()

    e_real = e_limpio = 0.0
    n_ciclos = n_inseguras = n_bloqueadas_agua = n_lavados = 0
    e_limpieza_wh = 0.0
    lavado_en = None
    log = [] if registrar else None
    lavado_realizado = False
    primera = {}

    for k in range(esc.n):
        # ---------- 1. lecturas
        isc_s, isc_l = sd.celdas(k, panel.sr(), ref.sr())
        c_nf, g_us = sd.afe(k, panel.total, hr_real[k])
        prueba = (k % PASOS_DIA) == PASO_PRUEBA
        if prueba:
            vr, vc, ic = sd.prueba_carga(k)
        else:
            vr = vc = ic = np.float32(0.0)
        L = dict(t_aire=esc.t_aire_m[k], hr=esc.hr_m[k], t_vidrio=esc.tv_m[k],
                 isc_s=isc_s, isc_l=isc_l, c_nf=c_nf, g_us=g_us,
                 prueba_valida=prueba, v_reposo=vr, v_carga=vc, i_carga=ic,
                 lavado_realizado=lavado_realizado)
        lavado_realizado = False

        # ---------- 2. decisión del firmware
        out = controlador.paso(k, L)

        # ---------- 3. aplicar
        mojado_inseguro = w[k] >= AGUA["w_inseguro"]
        if usar_eds and out["limpiar"]:
            n_ciclos += 1
            e_limpieza_wh += EDS["energia_wh_m2"] * PANEL["area_m2"]
            if mojado_inseguro:
                n_inseguras += 1           # ¡esto NUNCA debería pasar!
            else:
                panel.limpiar_eds(hr_real[k])
        elif mojado_inseguro and not out["permiso"]:
            n_bloqueadas_agua += 1
        if usar_eds and out["limpiar_ref"] and not mojado_inseguro:
            ref.limpiar_eds(hr_real[k])

        # alertas -> órdenes de lavado
        dia = k // PASOS_DIA
        for nombre in ("alerta_lavado", "alerta_conector", "alerta_termica", "falla_sensor", "alerta_v1_conector"):
            if out.get(nombre) and nombre not in primera:
                primera[nombre] = k / PASOS_DIA
                if nombre == "alerta_conector":
                    primera["dias_falla_conector_en_alerta"] = out["dias_falla_conector"]
        if out["alerta_lavado"] and lavado_en is None:
            lavado_en = k + LAVADO["retraso_dias"] * PASOS_DIA
        if lavado_periodico_dias and k > 0 and k % (lavado_periodico_dias * PASOS_DIA) == 0 and lavado_en is None:
            lavado_en = k
        if lavado_en is not None and k >= lavado_en and poa[k] > 50:   # se lava de día
            panel.lavar(); ref.lavar(); n_lavados += 1
            lavado_en = None; lavado_realizado = True
            primera.pop("alerta_lavado", None)

        # ---------- 4. física
        panel.avanzar(mes[k], w[k], polvo[k], fin_lluvia[k], viento[k])
        ref.avanzar(mes[k], w[k], polvo[k], fin_lluvia[k], viento[k])
        sr = panel.sr()
        p_l = potencia_w(poa[k], 1.0, tv[k])
        e_limpio += p_l * DT_H
        e_real += p_l * sr * DT_H

        if registrar:
            log.append((k, sr, panel.suelto, panel.cem, w[k], out["permiso"], out["bits"], out["p_rocio"],
                        out["limpiar"], out["suelto"], out["costra"], out["margen"], ref.sr()))

    e_neta = e_real - e_limpieza_wh
    res = dict(
        energia_kwh=e_real / 1000, energia_limpio_kwh=e_limpio / 1000,
        perdida_suciedad_pct=100 * (1 - e_real / e_limpio),
        energia_neta_kwh=e_neta / 1000,
        ciclos_eds=n_ciclos, activaciones_inseguras=n_inseguras,
        pasos_agua_bloqueados=n_bloqueadas_agua, lavados=n_lavados,
        agua_litros=n_lavados * LAVADO["litros_m2"] * PANEL["area_m2"],
        energia_limpieza_kwh=e_limpieza_wh / 1000,
        costra_final_gm2=panel.cem,
        primera_alerta=primera,
    )
    if registrar:
        cols = ["k", "sr", "suelto_gm2", "cem_gm2", "w", "permiso", "bits", "p_rocio", "limpiar",
                "suelto_est", "costra_est", "margen", "sr_ref"]
        res["log"] = pd.DataFrame(log, columns=cols, index=clima.index)
    return res
