"""
caracteristicas.py — Variables de entrada del predictor de rocío
================================================================

Se calculan SOLO con lo que el microcontrolador mide en ese momento y en las
últimas 3 horas (nada del futuro). Todo en float32, igual que en el STM32,
para que el árbol en C tome exactamente las mismas decisiones.

  margen_rocio   = T_vidrio - T_rocio          (si baja de 0 -> condensa)
  d_margen_1h/3h = cuánto bajó el margen       (tendencia: lo que anticipa)
  hr, d_hr_1h    = humedad del aire y su tendencia
  t_aire, d_taire_1h
  enfriamiento   = T_aire - T_vidrio           (noche despejada -> vidrio frío)
  hora_sin/cos   = hora del día en forma circular (23:45 está cerca de 00:00)

La versión paso a paso (la que corre el firmware) está en controlador.py y en
firmware/sx_caracteristicas.c. test_equivalencia() verifica que coinciden.
"""
import numpy as np

NOMBRES = ["margen_rocio", "d_margen_1h", "d_margen_3h", "hr", "d_hr_1h",
           "t_aire", "d_taire_1h", "enfriamiento", "hora_sin", "hora_cos"]
N_CARACT = len(NOMBRES)
HIST = 13            # 3 h de historia + el paso actual (pasos de 15 min)
F32 = np.float32
DOS_PI_24 = F32(2.0 * np.pi / 24.0)


def punto_rocio_f32(t, hr):
    """Punto de rocío (Magnus). Se calcula en DOBLE precisión y se redondea a float32:
    así la PC y el STM32 dan el mismo resultado aunque sus logf() difieran en el último bit."""
    t = np.asarray(t, F32).astype(np.float64)
    hr = np.clip(np.asarray(hr, F32), F32(0.1), F32(100.0)).astype(np.float64)
    a, b = 17.62, 243.12
    g = np.log(hr / 100.0) + a * t / (b + t)
    return (b * g / (a - g)).astype(F32)


# seno/coseno de la hora: solo hay 96 valores posibles (pasos de 15 min) -> tabla.
# La misma tabla se escribe en firmware/sx_config.h (evita diferencias de sinf entre plataformas).
_ANG = (np.arange(96, dtype=F32) * F32(0.25) * DOS_PI_24).astype(F32)
TABLA_SIN = np.sin(_ANG).astype(F32)
TABLA_COS = np.cos(_ANG).astype(F32)


def _rezago(x, k):
    idx = np.maximum(np.arange(len(x)) - k, 0)
    return x[idx]


def caracteristicas_vectorizadas(t_aire, hr, t_vidrio, hora):
    ta = np.asarray(t_aire, F32)
    h = np.asarray(hr, F32)
    tv = np.asarray(t_vidrio, F32)
    hora = np.asarray(hora, F32)
    td = punto_rocio_f32(ta, h)
    m = (tv - td).astype(F32)
    X = np.empty((len(ta), N_CARACT), F32)
    X[:, 0] = m
    X[:, 1] = m - _rezago(m, 4)
    X[:, 2] = m - _rezago(m, 12)
    X[:, 3] = h
    X[:, 4] = h - _rezago(h, 4)
    X[:, 5] = ta
    X[:, 6] = ta - _rezago(ta, 4)
    X[:, 7] = ta - tv
    i = np.round(hora * 4).astype(int) % 96
    X[:, 8] = TABLA_SIN[i]
    X[:, 9] = TABLA_COS[i]
    return X
