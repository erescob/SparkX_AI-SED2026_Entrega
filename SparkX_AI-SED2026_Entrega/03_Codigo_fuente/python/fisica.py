"""
fisica.py — Física del vidrio: temperatura, agua (rocío/lluvia), suciedad y energía
==================================================================================

Lo que NO depende de la política de limpieza se precalcula para todo el año
(temperatura del vidrio y película de agua). La suciedad sí depende de cuándo
se limpia, así que se avanza paso a paso en simulador.py usando las funciones
de aquí.

Idea física clave que el v1 no tenía:
  El rocío NO depende de que el vidrio esté "frío" (< 20 °C), sino de que el
  vidrio esté por DEBAJO DEL PUNTO DE ROCÍO del aire. De noche el vidrio se
  enfría por radiación hacia el cielo y queda 2-6 °C por debajo del aire en
  noches despejadas. Por eso hay rocío a 24 °C en el monzón (punto de rocío
  ~23 °C) y no lo hay a 15 °C en una mañana seca de abril (punto de rocío ~2 °C).
"""
import numpy as np

from config import PANEL, SUCIEDAD, AGUA, EDS, DT_H

SIGMA_SB = 5.670e-8


def emisividad_cielo(t_rocio, nubes):
    """Berdahl-Martin (cielo despejado) + corrección por nubosidad."""
    tdp = t_rocio / 100.0
    e_clear = np.clip(0.711 + 0.56 * tdp + 0.73 * tdp ** 2, 0.5, 0.98)
    return e_clear + (1 - e_clear) * 0.8 * nubes


def temperatura_vidrio(clima):
    """Temperatura del vidrio [°C].
    Día: modelo de Faiman (calentamiento por irradiancia, enfriado por viento).
    Noche y día: déficit radiativo hacia el cielo (lo que permite el rocío).
    Inercia térmica: filtro de primer orden con constante tau."""
    t_air = clima.t_aire.values
    ws = clima.viento.values
    g = clima.poa.values
    e_sky = emisividad_cielo(clima.t_rocio.values, clima.nubes.values)
    tk = t_air + 273.15
    eps = PANEL["emisividad_vidrio"]
    f_cielo = (1 + np.cos(np.radians(PANEL["inclinacion"]))) / 2
    deficit = eps * SIGMA_SB * tk ** 4 * (1 - e_sky) * f_cielo          # W/m²
    h_c = 5.7 + 3.8 * ws
    h_r = 4 * eps * SIGMA_SB * tk ** 3
    dt_rad = deficit / (h_c + h_r)                                     # °C bajo el aire
    peso_noche = np.clip(1.0 - g / 300.0, 0.3, 1.0)                    # Faiman ya incluye parte del día
    t_eq = t_air + g / (PANEL["faiman_u0"] + PANEL["faiman_u1"] * ws) - dt_rad * peso_noche
    alfa = 1 - np.exp(-15.0 / PANEL["tau_termico_min"])
    t = np.empty_like(t_eq)
    t[0] = t_eq[0]
    for i in range(1, len(t)):
        t[i] = t[i - 1] + alfa * (t_eq[i] - t[i - 1])
    return t


def pelicula_agua(clima, t_vidrio):
    """Masa de agua sobre el vidrio W [g/m²].
    Condensa si el vidrio está bajo el punto de rocío, evapora si está arriba
    (más rápido con sol). La lluvia deja una lámina que escurre."""
    td = clima.t_rocio.values
    lluvia = clima.lluvia_mm.values
    g = clima.poa.values
    w = np.zeros(len(td))
    for i in range(1, len(w)):
        wi = w[i - 1]
        if lluvia[i] > 0:
            wi = AGUA["w_lluvia"]
        else:
            if wi > 60.0:                      # la lámina de lluvia escurre en ~1 h
                wi = max(60.0, wi - 240.0 * DT_H)
            dif = td[i] - t_vidrio[i]
            if dif > 0:
                wi += AGUA["k_condensa"] * dif * DT_H
            else:
                wi -= AGUA["k_evapora"] * (-dif) * (1 + g[i] / 200.0) * DT_H
            wi = max(0.0, wi)
        w[i] = wi
    return w


def perdida_optica(masa_gm2):
    """Soiling ratio (fracción de luz que llega a la celda)."""
    return np.exp(-SUCIEDAD["a_perdida"] * masa_gm2)


def eficiencia_eds(hr):
    """Fracción del polvo SUELTO que quita un ciclo EDS (cae con la humedad)."""
    e = EDS["eff_max"] - EDS["pendiente_hr"] * (hr - EDS["hr_ref"])
    return float(np.clip(e, EDS["eff_min"], EDS["eff_max"]))


def deposito_base_paso(mes):
    """g/m² depositados en un paso de 15 min por depósito de fondo."""
    return SUCIEDAD["deposito_base"] * SUCIEDAD["factor_mes"][mes - 1] * DT_H / 24.0


def potencia_w(poa, sr, t_celda):
    return PANEL["p_stc_w"] * poa / 1000.0 * sr * (1 + PANEL["gamma_t"] * (t_celda - 25.0)) * PANEL["eta_otros"]


class Suciedad:
    """Estado de suciedad de UNA superficie (el panel o la celda de referencia).
    Dos depósitos:  suelto  -> lo quita el EDS
                    cementado (costra/lodo) -> solo lo quita lluvia fuerte o lavado manual"""

    def __init__(self):
        self.suelto = 0.0
        self.cem = 0.0

    @property
    def total(self):
        return self.suelto + self.cem

    def sr(self):
        return float(perdida_optica(self.total))

    def avanzar(self, mes, w_agua, polvo_evento, fin_lluvia_mm, viento):
        moj = w_agua >= AGUA["w_mojado"]
        dep = deposito_base_paso(mes) * (0.6 + 0.2 * viento) + polvo_evento
        if moj:
            dep *= SUCIEDAD["factor_deposito_mojado"]
        self.suelto += dep
        if moj:   # el agua disuelve sales y pega el polvo: parte se vuelve costra
            k = min(1.0, SUCIEDAD["k_cementacion_h"] * min(1.0, w_agua / 30.0) * DT_H)
            pasa = k * self.suelto
            self.suelto -= pasa
            self.cem += pasa
        if fin_lluvia_mm > 0:
            mm = fin_lluvia_mm
            if mm < SUCIEDAD["lluvia_ligera_mm"]:
                pasa = SUCIEDAD["ligera_a_lodo"] * self.suelto      # lluvia ligera = lodo
                self.suelto -= pasa
                self.cem += pasa
            elif mm < SUCIEDAD["lluvia_fuerte_mm"]:
                self.suelto *= 1 - SUCIEDAD["media_quita_suelto"]
                self.cem *= 1 - SUCIEDAD["media_quita_cem"]
            else:
                self.suelto *= 1 - SUCIEDAD["fuerte_quita_suelto"]
                self.cem *= 1 - SUCIEDAD["fuerte_quita_cem"]

    def limpiar_eds(self, hr):
        self.suelto *= 1 - eficiencia_eds(hr)

    def lavar(self):
        self.suelto = 0.0
        self.cem = 0.0
