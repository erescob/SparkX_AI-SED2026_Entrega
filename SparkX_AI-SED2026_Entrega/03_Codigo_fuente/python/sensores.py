"""
sensores.py — Escenarios (un año de una instalación) y modelos de sensores
=========================================================================

Un "escenario" = clima de un año + física precalculada + fallas inyectadas +
sensores con su ruido, sesgo de calibración y fallas propias.

Sensores del v2 y POR QUÉ cambian respecto al v1:
  * SHT40 (T y HR del aire, I2C, ~3 USD)  -> NUEVO. Sin humedad no se puede
    calcular el punto de rocío, que es la física real del rocío.
  * NTC en el vidrio                       -> igual que v1 (temp_panel).
  * Par de mini-celdas de referencia      -> NUEVO. "vel_suciedad" en el v1 no
    tenía de dónde salir. Una celda bajo el vidrio EDS (sucia) y otra que su
    propio mini-EDS mantiene limpia: SR = Isc_sucia / Isc_limpia.
  * AFE de impedancia de electrodos        -> REEMPLAZA a "v_flyback". Con la
    AT apagada (SECURE_MEASURE=1) se mide capacitancia y conductancia de fuga
    entre fases con una señal de baja tensión. El agua las sube mucho.
    (El v1 medía el retorno del flyback con el flyback APAGADO: imposible.)
  * Riel de 12 V en reposo y BAJO CARGA    -> CORRIGE a "v_12v". Un conector
    flojo solo se nota cuando pasa corriente: R = (V_reposo - V_carga) / I.
    Requiere un sensor de corriente (shunt + amplificador, ~1 USD) y una carga
    de prueba de 1 A durante 50 ms (MOSFET + resistencia).
"""
from dataclasses import dataclass, field
from typing import Optional
import numpy as np

from config import SENSORES as S, FALLAS as FA, PASOS_DIA, DT_H, AGUA, IA
from clima import generar_clima
from fisica import temperatura_vidrio, pelicula_agua


@dataclass
class Fallas:
    conector_inicio_dia: Optional[float] = None
    conector_tau_dias: float = 25.0
    hotspot_inicio_dia: Optional[float] = None
    hotspot_dt: float = 15.0
    sensor: Optional[str] = None          # 'sht_congelado' | 'ntc_abierto' | 'afe_abierto'
    sensor_inicio_dia: float = 0.0
    lodo_dia: Optional[int] = None        # tolvanera a las 17:00 + lluvia ligera a las 19:00 de ese día


def fallas_aleatorias(rng, prob_conector=0.5, prob_hotspot=0.5, prob_sensor=0.0):
    f = Fallas()
    if rng.random() < prob_conector:
        f.conector_inicio_dia = rng.uniform(40, 250)
        f.conector_tau_dias = rng.uniform(*FA["tau_degradacion_dias"])
    if rng.random() < prob_hotspot:
        f.hotspot_inicio_dia = rng.uniform(40, 300)
        f.hotspot_dt = rng.uniform(*FA["hotspot_dt_max"])
    if rng.random() < prob_sensor:
        f.sensor = rng.choice(["sht_congelado", "ntc_abierto", "afe_abierto"])
        f.sensor_inicio_dia = rng.uniform(20, 340)
    return f


def r_contacto(dias, f: Fallas):
    """Resistencia del conector [ohm] en función del día (crece exponencial al degradarse)."""
    r = np.full_like(dias, FA["r_contacto_sano"], dtype=float)
    if f.conector_inicio_dia is not None:
        t = np.clip(dias - f.conector_inicio_dia, 0, None)
        r = r + 0.01 * (np.exp(t / f.conector_tau_dias) - 1)
    return np.minimum(r, 2.0)


@dataclass
class Escenario:
    semilla: int
    fallas: Fallas
    clima: object
    tv: np.ndarray            # temperatura real del vidrio
    w: np.ndarray             # agua real sobre el vidrio
    # lecturas que NO dependen de la limpieza (precalculadas con su ruido)
    t_aire_m: np.ndarray = field(default=None)
    hr_m: np.ndarray = field(default=None)
    tv_m: np.ndarray = field(default=None)
    sesgos: dict = field(default_factory=dict)

    @property
    def n(self):
        return len(self.tv)


def construir_escenario(semilla, fallas: Fallas = None):
    fallas = fallas or Fallas()
    clima = generar_clima(semilla)
    if fallas.lodo_dia is not None:
        # Evento realista que forma LODO: tolvanera (2 g/m² ≈ 10 % de pérdida) y 2 h después
        # una lluvia ligera (1.5 mm) que la pega al vidrio. Si el polvo se quitó antes, no hay lodo.
        i0 = fallas.lodo_dia * PASOS_DIA + 17 * 4
        clima.iloc[i0, clima.columns.get_loc("polvo_evento_gm2")] += 2.0
        j = clima.columns.get_loc("lluvia_mm")
        for d in range(2):
            clima.iloc[i0 + 8 + d, j] = 0.75
        clima.iloc[i0 + 9, clima.columns.get_loc("evento_lluvia_fin_mm")] = 1.5
        clima.iloc[i0 + 8:i0 + 10, clima.columns.get_loc("nubes")] = 1.0
    tv = temperatura_vidrio(clima)
    w = pelicula_agua(clima, tv)
    esc = Escenario(semilla, fallas, clima, tv, w)
    rng = np.random.default_rng(10_000 + semilla)
    n = esc.n
    dia = np.arange(n) / PASOS_DIA

    # sesgos de calibración de ESTA unidad (fijos todo el año)
    sb = dict(
        sht_t=rng.normal(0, S["sht_t_bias"]), sht_hr=rng.normal(0, S["sht_hr_bias"]),
        ntc=rng.normal(0, S["ntc_bias"]), gan_cal=rng.normal(1, S["ganancia_cal_sigma"]),
        c0=S["c0_nf"] * rng.normal(1, S["c0_sigma"]), gan_i=rng.normal(1, S["i_ganancia_sigma"]),
    )
    esc.sesgos = sb
    ta = clima.t_aire.values + sb["sht_t"] + rng.normal(0, S["sht_t_sigma"], n)
    hr = clima.hr.values + sb["sht_hr"] + rng.normal(0, S["sht_hr_sigma"], n)
    hr = np.clip(hr, 0, 100)
    tvm = tv + sb["ntc"] + rng.normal(0, S["ntc_sigma"], n)

    # falla térmica: punto caliente bajo el sensor (se nota con sol)
    if fallas.hotspot_inicio_dia is not None:
        activo = dia >= fallas.hotspot_inicio_dia
        tvm = tvm + activo * fallas.hotspot_dt * clima.poa.values / 1000.0

    # fallas de sensores
    if fallas.sensor is not None:
        i0 = int(fallas.sensor_inicio_dia * PASOS_DIA)
        if fallas.sensor == "sht_congelado":
            ta[i0:] = ta[i0]
            hr[i0:] = hr[i0]
        elif fallas.sensor == "ntc_abierto":
            tvm[i0:] = -40.0      # ADC en el riel: el firmware lo convierte a -40 °C

    esc.t_aire_m = np.round(ta, 2).astype(np.float32)
    esc.hr_m = np.round(hr, 2).astype(np.float32)
    esc.tv_m = np.round(tvm, 2).astype(np.float32)
    return esc


class SensoresDinamicos:
    """Lecturas que dependen del estado de suciedad (cambian con la política)."""

    def __init__(self, esc: Escenario):
        self.esc = esc
        self.rng = np.random.default_rng(20_000 + esc.semilla)
        self.r_c = r_contacto(np.arange(365, dtype=float), esc.fallas)
        f = esc.fallas
        self.afe_falla_paso = (int(f.sensor_inicio_dia * PASOS_DIA)
                               if f.sensor == "afe_abierto" else None)
        n = esc.n
        r = self.rng
        # ruido pregenerado (mucho más rápido que pedirlo paso a paso)
        self.z_isc = r.normal(0, S["isc_sigma_rel"], (n, 2))
        self.z_c = r.normal(0, S["c_ruido_rel"], n)
        self.z_g = r.normal(0, S["g_ruido_rel"], n)
        self.z_gp = np.abs(r.normal(0, S["g_piso_us"], n))
        self.theta = np.minimum(1.0, esc.w / 30.0)
        self.h_hr = np.clip(np.exp((esc.clima.hr.values - 60.0) / 12.0), 0.05, 5.0)
        self.poa = esc.clima.poa.values

    def celdas(self, i, sr_panel, sr_ref):
        """Isc [mA] de la mini-celda sucia y la limpia."""
        g = self.poa[i]
        k = S["isc_ref_ma"] / 1000.0
        ruido = self.z_isc[i]
        isc_l = k * g * sr_ref * (1 + ruido[0])
        isc_s = k * g * sr_panel * self.esc.sesgos["gan_cal"] * (1 + ruido[1])
        return np.float32(max(isc_s, 0.0)), np.float32(max(isc_l, 0.0))

    def afe(self, i, masa_total, hr_real):
        """Capacitancia [nF] y conductancia de fuga [µS] entre fases (AT apagada)."""
        if self.afe_falla_paso is not None and i >= self.afe_falla_paso:
            return np.float32(0.5), np.float32(0.0)     # electrodo desconectado
        theta = self.theta[i]
        c = self.esc.sesgos["c0"] * (1 + S["c_por_gm2"] * masa_total + S["c_por_agua"] * theta)
        c *= 1 + self.z_c[i]
        h = self.h_hr[i]
        g = (S["g0_us"] + S["g_polvo_us"] * masa_total * h
             + theta * (S["g_agua_us"] + S["g_agua_polvo_us"] * masa_total))
        g = g * (1 + self.z_g[i]) + self.z_gp[i]
        return np.float32(c), np.float32(g)

    def prueba_carga(self, i):
        """Prueba de 1 A / 50 ms al mediodía: V reposo, V bajo carga, I medida."""
        clima = self.esc.clima
        dia = int(i // PASOS_DIA)
        poa = clima.poa.values[i]
        v_bat = S["v_bat_noche"] + (S["v_bat_dia"] - S["v_bat_noche"]) * min(1.0, poa / 600.0)
        v_bat += self.rng.normal(0, S["v_bat_sigma"])
        r_int = S["r_interna"] * (1 + 0.01 * (25.0 - clima.t_aire.values[i]))
        rc = self.r_c[min(dia, 364)]
        if self.esc.fallas.conector_inicio_dia is not None and dia >= self.esc.fallas.conector_inicio_dia:
            nivel = min(1.0, rc / FA["r_contacto_falla"])
            rc *= 1 + FA["ruido_intermitente"] * nivel * abs(self.rng.normal())
        i_real = S["i_prueba_a"]
        v_carga = v_bat - i_real * (r_int + S["r_cableado"] + rc)
        lsb = S["vref"] / 2 ** S["adc_bits"] * S["div_12v"]
        sig = S["ruido_adc_lsb"] * lsb / np.sqrt(S["promedios"])
        vr = v_bat + self.rng.normal(0, sig)
        vc = v_carga + self.rng.normal(0, sig)
        im = i_real * self.esc.sesgos["gan_i"] + self.rng.normal(0, S["i_ruido_a"])
        return np.float32(vr), np.float32(vc), np.float32(im)


def etiqueta_rocio_futuro(w, horizonte_h=None):
    """1 si en las próximas H horas habrá película de agua (>= w_mojado)."""
    h = horizonte_h or IA["horizonte_h"]
    k = int(round(h / DT_H))
    moj = (w >= AGUA["w_mojado"]).astype(np.int8)
    fut = np.zeros_like(moj)
    # máximo de moj[t+1 .. t+k]
    for j in range(1, k + 1):
        fut[:-j] = np.maximum(fut[:-j], moj[j:])
    return fut
