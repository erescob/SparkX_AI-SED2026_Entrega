"""
clima.py — Generador estocástico de clima para Hermosillo (paso de 15 min)
=========================================================================

¿Por qué no usar datos reales directamente? Porque esta máquina no tiene
acceso a internet para descargarlos. El generador está calibrado con las
normales 1991-2020 (config.CLIMA) y produce años distintos con cada semilla.
CUANDO TENGAN DATOS REALES (NSRDB, PVGIS, estación de CONAGUA o la del
asesor), reemplacen generar_clima() por un lector de CSV con las mismas
columnas y todo lo demás funciona igual.

Columnas que produce (una fila cada 15 min):
  t_aire [°C], t_rocio [°C], hr [%], viento [m/s], nubes [0-1],
  lluvia_mm [mm en el paso], evento_lluvia_fin_mm [mm totales del evento, solo
  en el último paso del evento], polvo_evento_gm2 [g/m² depositados por
  tolvanera], ghi, poa [W/m²], hora [h decimal], mes [1-12], dia [0-364]
"""
import numpy as np
import pandas as pd
import pvlib

from config import SITIO, CLIMA, PANEL, SUCIEDAD, PASOS_DIA, DT_MIN


def magnus_td(t, hr):
    """Punto de rocío (°C) con la fórmula de Magnus. Misma fórmula que en el firmware."""
    a, b = 17.62, 243.12
    g = np.log(np.clip(hr, 1e-3, 100.0) / 100.0) + a * t / (b + t)
    return b * g / (a - g)


def magnus_hr(t, td):
    a, b = 17.62, 243.12
    return 100.0 * np.exp(a * td / (b + td) - a * t / (b + t))


_MES_DE_DIA = np.repeat(np.arange(12), [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31])


def _diario_desde_mensual(valores, dias):
    """Interpola valores mensuales a diarios de forma suave y cíclica (anclas a mitad de mes).
    Ajusta las anclas para que la MEDIA de cada mes siga siendo la normal
    (si no, los picos como junio se aplanan)."""
    objetivo = np.asarray(valores, float)
    mitad = np.array([15, 45, 74, 105, 135, 166, 196, 227, 258, 288, 319, 349], float)
    x = np.concatenate([mitad - 365, mitad, mitad + 365])
    anclas = objetivo.copy()
    todos = np.arange(365)
    for _ in range(8):
        diario = np.interp(todos, x, np.tile(anclas, 3))
        medias = np.array([diario[_MES_DE_DIA == m].mean() for m in range(12)])
        anclas += objetivo - medias
    return np.interp(dias, x, np.tile(anclas, 3))


def _rocio_para_hr_media(tmin, tmax, hr_obj):
    """Busca (bisección) el punto de rocío diario tal que el PROMEDIO de la HR a lo largo
    del ciclo de temperatura del día sea hr_obj. Usar el rocío de la temperatura media
    sobreestima la HR porque de noche la HR sube mucho (la función no es lineal)."""
    h = np.arange(24) + 0.5
    forma = np.where(h < 6, np.sqrt((h + 9) / 15) * -1 + 1,
             np.where(h < 15, 0.5 * (1 - np.cos(np.pi * (h - 6) / 9)), 1 - np.sqrt(np.clip(h - 15, 0, None) / 15)))
    t = tmin[:, None] + (tmax - tmin)[:, None] * forma[None, :]
    lo, hi = np.full(len(tmin), -30.0), tmin.copy()
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        hr = magnus_hr(t, mid[:, None]).mean(axis=1)
        alto = hr > hr_obj
        hi = np.where(alto, mid, hi)
        lo = np.where(alto, lo, mid)
    return 0.5 * (lo + hi)


def _ar1(rng, n, phi, sigma):
    e = np.zeros(n)
    s = sigma * np.sqrt(1 - phi ** 2)
    for i in range(1, n):
        e[i] = phi * e[i - 1] + rng.normal(0, s)
    return e


def generar_clima(semilla, anio=2025):
    rng = np.random.default_rng(semilla)
    nd = 365
    dias = np.arange(nd)
    mes_d = pd.date_range(f"{anio}-01-01", periods=nd, freq="D").month.values

    # --- normales suavizadas a escala diaria
    tmax_n = _diario_desde_mensual(CLIMA["tmax"], dias)
    tmin_n = _diario_desde_mensual(CLIMA["tmin"], dias)
    hr_n = _diario_desde_mensual(CLIMA["hr_media"], dias)
    nub_n = _diario_desde_mensual(CLIMA["nubes"], dias)
    vie_n = _diario_desde_mensual(CLIMA["viento_ms"], dias)
    tmed_n = 0.5 * (tmax_n + tmin_n)
    td_n = _rocio_para_hr_media(tmin_n, tmax_n, hr_n)   # rocío "típico" del día

    # --- lluvia: cadena de Markov de 2 estados (persistencia de días húmedos)
    dias_mes = np.array([31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31])
    p_lluvia = np.array(CLIMA["dias_lluvia"]) / dias_mes
    lluvia_d = np.zeros(nd)
    mojado_prev = False
    for d in range(nd):
        p = p_lluvia[mes_d[d] - 1]
        p11 = p + 0.30 * (1 - p)                 # persistencia: tras un día húmedo es más probable otro
        p01 = p * (1 - p11) / max(1 - p, 1e-6)   # así la fracción de días húmedos sigue siendo p
        p = p11 if mojado_prev else p01
        if rng.random() < p:
            m = mes_d[d] - 1
            media = CLIMA["lluvia_mm"][m] / max(CLIMA["dias_lluvia"][m], 0.3)
            lluvia_d[d] = rng.gamma(0.7, media / 0.7)
            mojado_prev = True
        else:
            mojado_prev = False
    llueve = lluvia_d > 0.2

    # --- temperaturas y rocío diarios con anomalías correlacionadas
    a_t = _ar1(rng, nd, CLIMA["phi_t"], CLIMA["sigma_t"])
    a_t2 = 0.6 * a_t + 0.8 * _ar1(rng, nd, CLIMA["phi_t"], CLIMA["sigma_t"])
    tmax_d = tmax_n + a_t - 4.0 * llueve
    tmin_d = tmin_n + a_t2 - 1.0 * llueve
    tmin_d = np.minimum(tmin_d, tmax_d - 4.0)
    td_d = td_n + _ar1(rng, nd, CLIMA["phi_td"], CLIMA["sigma_td"]) + 1.0 * llueve
    td_d = np.minimum(td_d, tmin_d + 0.5)

    # --- nubosidad diaria (Beta con media mensual)
    k = 2.5
    nub_d = rng.beta(np.maximum(nub_n * k, 0.05), np.maximum((1 - nub_n) * k, 0.05))
    nub_d = np.where(llueve, np.maximum(nub_d, 0.7), nub_d)

    # --- viento diario (Weibull k=2 escalada a la media mensual)
    vie_d = vie_n * rng.weibull(2.0, nd) / 0.886

    # ------------------------------------------------------------------
    # Escala de 15 minutos
    # ------------------------------------------------------------------
    n = nd * PASOS_DIA
    idx = pd.date_range(f"{anio}-01-01", periods=n, freq=f"{DT_MIN}min", tz=SITIO["tz"])
    hora = idx.hour.values + idx.minute.values / 60.0
    d_i = np.repeat(dias, PASOS_DIA)

    # Temperatura del aire: mínima ~06:00, máxima ~15:00, enfriamiento nocturno hacia la mínima siguiente
    tmin_sig = np.roll(tmin_d, -1)
    t = np.empty(n)
    for i in range(n):
        d, h = d_i[i], hora[i]
        if h < 6.0:     # madrugada: viene de la tarde anterior
            dp = max(d - 1, 0)
            frac = (h + 9.0) / 15.0
            t[i] = tmax_d[dp] + (tmin_d[d] - tmax_d[dp]) * np.sqrt(frac)
        elif h < 15.0:  # calentamiento
            t[i] = tmin_d[d] + (tmax_d[d] - tmin_d[d]) * 0.5 * (1 - np.cos(np.pi * (h - 6.0) / 9.0))
        else:           # enfriamiento hacia la mínima de mañana
            frac = (h - 15.0) / 15.0
            t[i] = tmax_d[d] + (tmin_sig[d] - tmax_d[d]) * np.sqrt(frac)
    t += _ar1(rng, n, 0.95, 0.4)

    td = td_d[d_i] + 0.8 * np.sin(2 * np.pi * (hora - 9.0) / 24.0) + _ar1(rng, n, 0.97, 0.6)

    # Lluvia: un evento por día lluvioso; en monzón por la tarde-noche
    lluvia = np.zeros(n)
    fin_evento = np.zeros(n)
    for d in np.where(llueve)[0]:
        monzon = mes_d[d] in (7, 8, 9)
        h0 = rng.uniform(15, 22) if monzon else rng.uniform(0, 24)
        dur = rng.uniform(0.75, 3.0)
        i0 = d * PASOS_DIA + int(h0 * 4)
        i1 = min(i0 + max(1, int(dur * 4)), n)
        lluvia[i0:i1] += lluvia_d[d] / (i1 - i0)
        fin_evento[i1 - 1] = lluvia_d[d]
        t[i0:i1] -= 3.0
    moj = lluvia > 0
    # Después de la lluvia el aire queda casi saturado unas horas
    sat = np.convolve(moj.astype(float), np.ones(8), mode="full")[:n] > 0
    td = np.where(sat, np.maximum(td, t - 1.5), td)
    td = np.where(moj, t - 0.3, td)
    td = np.minimum(td, t)
    hr = magnus_hr(t, td)

    # Viento con ciclo diario (más fuerte en la tarde) y ráfagas con tormenta
    viento = vie_d[d_i] * (1.0 + 0.4 * np.sin(2 * np.pi * (hora - 10.0) / 24.0))
    viento = np.clip(viento + rng.normal(0, 0.3, n) + 4.0 * moj, 0.2, None)

    nubes = np.clip(nub_d[d_i] + _ar1(rng, n, 0.9, 0.12), 0, 1)
    nubes = np.where(moj, 1.0, nubes)

    # Tolvaneras: Poisson por día, por la tarde-noche
    polvo_ev = np.zeros(n)
    tasa = np.array(SUCIEDAD["eventos_mes"])[mes_d - 1] / 30.0
    for d in range(nd):
        if rng.random() < tasa[d]:
            h0 = rng.uniform(14, 21)
            i0 = d * PASOS_DIA + int(h0 * 4)
            polvo_ev[i0] = rng.lognormal(np.log(SUCIEDAD["evento_mediana_gm2"]), SUCIEDAD["evento_sigma_log"])
            viento[i0:i0 + 4] += 6.0

    # --- Irradiancia: cielo despejado (Ineichen, pvlib) atenuado por nubes (Kasten-Czeplak)
    loc = pvlib.location.Location(SITIO["lat"], SITIO["lon"], SITIO["tz"], SITIO["altitud_m"])
    cs = loc.get_clearsky(idx, model="ineichen")
    sol = loc.get_solarposition(idx)
    ghi = cs["ghi"].values * (1 - 0.75 * nubes ** 3.4)
    erbs = pvlib.irradiance.erbs(ghi, sol["zenith"].values, idx)
    poa = pvlib.irradiance.get_total_irradiance(
        PANEL["inclinacion"], PANEL["azimut"], sol["apparent_zenith"].values,
        sol["azimuth"].values, erbs["dni"], ghi, erbs["dhi"], model="isotropic"
    )["poa_global"]
    poa = np.nan_to_num(np.asarray(poa, float), nan=0.0).clip(0)

    return pd.DataFrame(dict(
        t_aire=t, t_rocio=td, hr=hr, viento=viento, nubes=nubes,
        lluvia_mm=lluvia, evento_lluvia_fin_mm=fin_evento, polvo_evento_gm2=polvo_ev,
        ghi=ghi, poa=poa, hora=hora, mes=idx.month.values, dia=d_i,
    ), index=idx)


if __name__ == "__main__":
    c = generar_clima(0)
    g = c.groupby("mes")
    res = pd.DataFrame({
        "tmax": g.t_aire.apply(lambda s: s.resample("D").max().mean()),
        "tmin": g.t_aire.apply(lambda s: s.resample("D").min().mean()),
        "hr": g.hr.mean(), "lluvia": g.lluvia_mm.sum(), "nubes": g.nubes.mean(),
        "td": g.t_rocio.mean(),
    }).round(1)
    res["tmax_norm"] = CLIMA["tmax"]; res["tmin_norm"] = CLIMA["tmin"]
    res["hr_norm"] = CLIMA["hr_media"]; res["lluvia_norm"] = CLIMA["lluvia_mm"]
    print(res.to_string())
    print("POA anual kWh/m2:", round(c.poa.sum() * 0.25 / 1000, 0))
