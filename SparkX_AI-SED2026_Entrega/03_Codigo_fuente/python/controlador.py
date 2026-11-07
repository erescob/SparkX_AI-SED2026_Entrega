"""
controlador.py — Espejo en Python del firmware del STM32 (paso de 15 min)
========================================================================

Este archivo hace EXACTAMENTE lo mismo que firmware/sx_controlador.c.
Se usa para simular el año completo rápido, y luego sil_validacion.py
compara paso a paso contra el C compilado.

Capas (de la más importante a la menos):
  1. SEGURIDAD (determinista). Si cualquier sensor es inválido o hay indicio de
     agua -> NO se permite alta tensión. El modelo de IA NO puede saltarse esto.
     En la PCB, además, un comparador por hardware corta la AT si la fuga sube
     durante la limpieza (segunda barrera, independiente del MCU).
  2. ESTIMADORES: soiling ratio, pérdida por polvo suelto vs costra, tasa.
  3. IA: probabilidad de rocío en las próximas 3 h (árbol de decisión).
  4. POLÍTICA: cuándo limpiar (varias políticas para compararlas).
  5. ALERTAS de mantenimiento con pronóstico: conector, anomalía térmica,
     lavado manual, falla de sensor.
"""
import json
import math
import numpy as np

from config import (SEGURIDAD as SG, POLITICA as PO, ALERTAS as AL, SENSORES as S,
                    PANEL, EDS, PASOS_DIA, DT_MIN)
from caracteristicas import HIST, N_CARACT, F32, punto_rocio_f32, TABLA_SIN, TABLA_COS

NUNCA, FIJO, UMBRAL, IA_PRED, REGLA_ROCIO, CADA_HORA = range(6)
NOMBRE_POLITICA = {NUNCA: "Sin limpieza", FIJO: "Horario fijo", UMBRAL: "Umbral (reactiva)",
                   IA_PRED: "IA predictiva", REGLA_ROCIO: "Regla de rocío", CADA_HORA: "Cada hora"}

# bits de motivo de bloqueo (iguales en C)
B_SENSOR, B_MARGEN, B_HR, B_FUGA, B_CAP, B_CONGELADO = 1, 2, 4, 8, 16, 32
PASOS_MIN_CICLO = EDS["min_entre_ciclos_min"] // DT_MIN
PASOS_REF = EDS["ref_periodo_min"] // DT_MIN
PASO_PRUEBA = int(AL["hora_prueba_carga"] * 60 // DT_MIN)


class Arbol:
    """Árbol de decisión en formato de arreglos (idéntico al de C)."""

    def __init__(self, ruta_json):
        d = json.load(open(ruta_json))
        self.f = np.array(d["feature"], np.int16)
        self.t = np.array(d["threshold"], F32)
        self.l = np.array(d["left"], np.int16)
        self.r = np.array(d["right"], np.int16)
        self.p = np.array(d["prob"], F32)

    def prob(self, x):
        n = 0
        while self.f[n] >= 0:
            n = self.l[n] if x[self.f[n]] <= self.t[n] else self.r[n]
        return float(self.p[n])


def _mediana(v):
    v = sorted(v)
    k = len(v)
    return v[k // 2] if k % 2 else F32(0.5) * (v[k // 2 - 1] + v[k // 2])


class Controlador:
    def __init__(self, arbol: Arbol, politica=IA_PRED, params=None, c_ref_nf=100.0, k_sr=1.0,
                 precalc=None):
        """precalc = (X, p) características y probabilidades ya calculadas para todo el año
        (vía rápida para simular muchas veces; da exactamente lo mismo, ver test en entrenar)."""
        self.arbol = arbol
        self.precalc = precalc
        self.pol = politica
        self.P = dict(PO)
        if params:
            self.P.update(params)
        self.c_ref = F32(c_ref_nf)
        self.k_sr = F32(k_sr)
        # historia para características
        self.h_m = []; self.h_hr = []; self.h_ta = []
        # detección de sensor congelado
        self.prev_ta = None; self.prev_hr = None; self.prev_tv = None
        self.cnt_sht = 0; self.cnt_tv = 0
        # limpieza
        self.ult_limpieza = -10 ** 6; self.ult_ref = -10 ** 6; self.dia_fijo = -1
        # estimadores
        self.sr_f = F32(1.0); self.sr_ini = False
        self.costra = F32(0.0); self.pendiente_residuo = 0
        self.suelto = F32(0.0)
        self.suelto_mediodia = F32(-1.0); self.limpio_desde_mediodia = False
        self.tasa_dia = F32(0.0)
        self.alerta_lavado = False
        # conector
        self.r_cal = []; self.r_base = None; self.r_hist = []
        self.cnt_r = 0; self.alerta_conector = False; self.dias_falla = F32(999.0)
        self.alerta_v1_conector = False
        # térmica
        self.buf_t = []; self.cal_t = []; self.off_t = None
        self.cnt_t = 0; self.alerta_termica = False; self.ultimo_residuo = F32(0.0)
        self.falla_sensor = False
        # polvo nocturno por capacitancia
        self.c_f = None; self.c_base = None; self.extra_c = F32(0.0); self.rebase = False
        self.c_seco_prev = False

    # ------------------------------------------------------------------
    def _caracteristicas(self, ta, hr, tv, hora):
        td = punto_rocio_f32(ta, hr)
        m = F32(tv - td)
        for h, v in ((self.h_m, m), (self.h_hr, F32(hr)), (self.h_ta, F32(ta))):
            h.append(v)
            if len(h) > HIST:
                h.pop(0)

        def rez(h, k):
            return h[max(len(h) - 1 - k, 0)]

        ih = int(round(float(hora) * 4)) % 96
        x = np.array([m, m - rez(self.h_m, 4), m - rez(self.h_m, 12), F32(hr),
                      F32(hr) - rez(self.h_hr, 4), F32(ta), F32(ta) - rez(self.h_ta, 4),
                      F32(ta) - F32(tv), TABLA_SIN[ih], TABLA_COS[ih]], F32)
        return x, m

    def _seguridad(self, L, margen):
        if self.P.get("seguridad_v1"):
            # Réplica de la lógica del v1 SOLO para compararla: bloquea si temp_panel <= 20 °C.
            # Sin validación de sensores, sin humedad, sin medición de agua.
            return B_MARGEN if L["t_vidrio"] <= F32(20.0) else 0
        bits = 0
        ta, hr, tv, c, g = L["t_aire"], L["hr"], L["t_vidrio"], L["c_nf"], L["g_us"]
        vals = (ta, hr, tv, c, g)
        if not all(np.isfinite(v) for v in vals):
            self.falla_sensor = True
            return B_SENSOR
        if not (SG["t_aire_rango"][0] <= ta <= SG["t_aire_rango"][1]): bits |= B_SENSOR
        if not (0.0 <= hr <= 100.0): bits |= B_SENSOR
        if not (SG["t_vidrio_rango"][0] <= tv <= SG["t_vidrio_rango"][1]): bits |= B_SENSOR
        if abs(tv - ta) > SG["dif_vidrio_aire_max"]: bits |= B_SENSOR
        c_rel = F32(c / self.c_ref)
        if not (SG["c_rango_rel"][0] <= c_rel <= SG["c_rango_rel"][1]): bits |= B_SENSOR
        if g < 0: bits |= B_SENSOR
        # congelado: lectura idéntica durante 6 h
        self.cnt_sht = self.cnt_sht + 1 if (ta == self.prev_ta and hr == self.prev_hr) else 0
        self.cnt_tv = self.cnt_tv + 1 if tv == self.prev_tv else 0
        self.prev_ta, self.prev_hr, self.prev_tv = ta, hr, tv
        if self.cnt_sht >= SG["pasos_congelado"] or self.cnt_tv >= SG["pasos_congelado"]:
            bits |= B_CONGELADO
        if bits & (B_SENSOR | B_CONGELADO):
            self.falla_sensor = True
            return bits
        if margen < SG["margen_rocio_min"]: bits |= B_MARGEN
        if hr > SG["hr_max"]: bits |= B_HR
        if g > SG["g_fuga_max_us"]: bits |= B_FUGA
        if c_rel - F32(1.0) > SG["dc_max"]: bits |= B_CAP
        return bits

    def _capacitancia(self, L, valido, seco):
        """Filtro de la capacitancia SOLO con vidrio seco. Con agua se congela (el agua sube la C
        mucho más que el polvo); al volver a secarse se reinicia sin retraso, para que el filtro
        no 'recuerde' el agua y la confunda con polvo."""
        c_rel = F32(L["c_nf"] / self.c_ref)
        if not valido:
            return
        if not seco:
            self.c_seco_prev = False
            return
        if self.c_f is None or self.rebase or not self.c_seco_prev:
            self.c_f = c_rel
            if self.rebase:
                self.c_base = c_rel
                self.extra_c = F32(0.0)
                self.rebase = False
        else:
            self.c_f = F32(self.c_f + F32(AL["alfa_c"]) * F32(c_rel - self.c_f))
        self.c_seco_prev = True

    def _estimar_suciedad(self, L, seco):
        valida = L["isc_l"] >= F32(S["isc_ref_ma"] * S["g_min_valida"] / 1000.0)
        if valida:
            sr_m = F32(L["isc_s"] / L["isc_l"] / self.k_sr)
            if self.pendiente_residuo > 0:
                self.costra = F32(min(max(F32(1.0) - sr_m, 0.0), 0.5))
                self.sr_f = sr_m
                self.pendiente_residuo = 0
            elif not self.sr_ini:
                self.sr_f = sr_m
            else:
                self.sr_f = F32(self.sr_f + F32(self.P["alfa_sr"]) * (sr_m - self.sr_f))
            self.sr_ini = True
            if self.c_f is not None:            # de día la celda manda; la capacitancia se re-referencia
                self.c_base = self.c_f
            self.extra_c = F32(0.0)
        else:
            if self.pendiente_residuo > 0:
                self.pendiente_residuo -= 1
            # sin luz: polvo nuevo estimado por el aumento de capacitancia (solo con vidrio seco)
            if seco and self.c_base is not None and self.c_f is not None:
                dc = F32(F32(self.c_f - self.c_base) - F32(AL["banda_c"]))
                if dc > F32(0.0):
                    self.extra_c = F32(1.0 - math.exp(-(AL["a_perdida_cal"] / AL["c_por_gm2_cal"]) * float(dc)))
                else:
                    self.extra_c = F32(0.0)
        perdida = F32(F32(F32(1.0) - self.sr_f) + self.extra_c)
        self.costra = F32(min(self.costra, max(perdida, F32(0.0))))
        self.suelto = F32(max(perdida - self.costra, F32(0.0)))
        if self.costra >= F32(self.P["umbral_costra"]):
            self.alerta_lavado = True

    def _tasa(self, paso):
        if paso % PASOS_DIA == PASO_PRUEBA:
            if self.suelto_mediodia >= 0 and not self.limpio_desde_mediodia:
                inc = max(F32(self.suelto - self.suelto_mediodia), F32(0.0))
                self.tasa_dia = F32(F32(0.7) * self.tasa_dia + F32(0.3) * inc)
            self.suelto_mediodia = self.suelto
            self.limpio_desde_mediodia = False

    def _conector(self, L):
        if not L["prueba_valida"] or L["i_carga"] < 0.5:
            return
        r = F32((L["v_reposo"] - L["v_carga"]) / L["i_carga"])
        if L["v_carga"] <= F32(11.1265):        # regla del v1, solo para comparar
            self.alerta_v1_conector = True
        if self.r_base is None:
            self.r_cal.append(r)
            if len(self.r_cal) >= AL["dias_calibracion"]:
                self.r_base = _mediana(self.r_cal)
            return
        dr = F32(r - self.r_base)
        self.r_hist.append(dr)
        if len(self.r_hist) > AL["dias_tendencia"]:
            self.r_hist.pop(0)
        self.cnt_r = self.cnt_r + 1 if dr >= F32(AL["delta_r_alerta"]) else 0
        if self.cnt_r >= 2:
            self.alerta_conector = True
        k = len(self.r_hist)
        if k >= 4:
            # regresión lineal con sumas en orden (igual que el ciclo en C)
            sx = sy = F32(0.0)
            for i, v in enumerate(self.r_hist):
                sx = F32(sx + F32(i)); sy = F32(sy + v)
            xm, ym = F32(sx / F32(k)), F32(sy / F32(k))
            num = den = F32(0.0)
            for i, v in enumerate(self.r_hist):
                dx = F32(F32(i) - xm)
                num = F32(num + F32(dx * F32(v - ym)))
                den = F32(den + F32(dx * dx))
            pend = F32(num / den)
            ult = F32(ym + F32(pend * F32(F32(k - 1) - xm)))
            if pend > F32(1e-4):
                self.dias_falla = F32(max((F32(AL["delta_r_falla"]) - ult) / pend, F32(0.0)))
            else:
                self.dias_falla = F32(999.0)

    def _termica(self, L, paso, valido):
        g_est = F32(L["isc_l"] / F32(S["isc_ref_ma"] / 1000.0))
        if valido and g_est >= F32(AL["g_min_termica"]) and len(self.buf_t) < 48:
            esperado = F32(L["t_aire"] + g_est / F32(PANEL["faiman_u0"] + PANEL["faiman_u1"] * AL["viento_supuesto"]))
            self.buf_t.append(F32(L["t_vidrio"] - esperado))
        if paso % PASOS_DIA == PASOS_DIA - 1:
            if len(self.buf_t) >= 8:
                med = _mediana(self.buf_t)
                if self.off_t is None:
                    self.cal_t.append(med)
                    if len(self.cal_t) >= AL["dias_calibracion"]:
                        self.off_t = _mediana(self.cal_t)
                else:
                    self.ultimo_residuo = F32(med - self.off_t)
                    # ventana de 5 días (bits): alerta si >= 3 de los últimos 5 superan el umbral.
                    # Un solo día ventoso ya no reinicia la cuenta.
                    alto = 1 if self.ultimo_residuo >= F32(AL["residuo_termico"]) else 0
                    self.cnt_t = ((self.cnt_t << 1) | alto) & 0x1F
                    if bin(self.cnt_t).count("1") >= AL["dias_termica"]:
                        self.alerta_termica = True
            self.buf_t = []

    # ------------------------------------------------------------------
    def paso(self, k, L):
        """k = número de paso desde el arranque; L = dict con lecturas. Devuelve dict de salidas."""
        if L.get("lavado_realizado"):
            self.costra = F32(0.0); self.sr_f = F32(1.0); self.alerta_lavado = False
        hora = F32((k % PASOS_DIA) * DT_MIN / 60.0)
        if self.precalc is None:
            x, margen = self._caracteristicas(L["t_aire"], L["hr"], L["t_vidrio"], hora)
        else:
            margen = self.precalc[0][k, 0]
        bits = self._seguridad(L, margen)
        permiso = bits == 0
        valido = not (bits & (B_SENSOR | B_CONGELADO))
        if not valido:
            p = 0.0
        elif self.precalc is None:
            p = self.arbol.prob(x)
        else:
            p = float(self.precalc[1][k])
        seco = margen >= AL["margen_c_min"] and L["g_us"] <= AL["g_c_max"]
        self._capacitancia(L, valido, seco)
        self._estimar_suciedad(L, seco)
        self._tasa(k)
        self._conector(L)
        self._termica(L, k, valido)

        P = self.P
        puede = permiso and (k - self.ult_limpieza) >= PASOS_MIN_CICLO
        u = F32(P["umbral_suelto"]); upre = F32(P["umbral_pre"])
        if self.pol == NUNCA:
            limpiar = False
        elif self.pol == FIJO:
            dia = k // PASOS_DIA
            limpiar = puede and P["hora_fija_ini"] <= hora < P["hora_fija_fin"] and dia != self.dia_fijo
            if limpiar:
                self.dia_fijo = dia
        elif self.pol == UMBRAL:
            limpiar = puede and self.suelto >= u
        elif self.pol == IA_PRED:
            limpiar = puede and (self.suelto >= u or (p >= P["p_rocio"] and self.suelto >= upre))
        elif self.pol == REGLA_ROCIO:
            limpiar = puede and (self.suelto >= u or (margen <= F32(P["margen_regla"]) and self.suelto >= upre))
        else:
            limpiar = puede
        if limpiar:
            self.ult_limpieza = k
            self.pendiente_residuo = 2
            self.limpio_desde_mediodia = True
            # sin luz no se puede medir el resultado: se asume ~70 % de eficiencia hasta medir
            self.sr_f = F32(F32(1.0) - (self.costra + self.suelto * F32(0.3)))
            self.suelto = F32(self.suelto * F32(0.3))
            self.extra_c = F32(0.0)
            self.rebase = True
        limpiar_ref = permiso and (k - self.ult_ref) >= PASOS_REF
        if limpiar_ref:
            self.ult_ref = k
        tasa_h = self.tasa_dia / F32(24.0)
        horas = F32(999.0) if tasa_h <= F32(1e-6) else F32(max((u - self.suelto) / tasa_h, F32(0.0)))
        return dict(limpiar=bool(limpiar), limpiar_ref=bool(limpiar_ref), permiso=bool(permiso),
                    bits=bits, p_rocio=p, margen=float(margen), suelto=float(self.suelto),
                    costra=float(self.costra), horas_a_limpieza=float(min(horas, F32(999.0))),
                    alerta_lavado=self.alerta_lavado, alerta_conector=self.alerta_conector,
                    dias_falla_conector=float(self.dias_falla), alerta_termica=self.alerta_termica,
                    falla_sensor=self.falla_sensor, alerta_v1_conector=self.alerta_v1_conector)
