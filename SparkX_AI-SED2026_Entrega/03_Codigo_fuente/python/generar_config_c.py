"""
generar_config_c.py — Escribe firmware/sx_config.h desde config.py
==================================================================
Una sola fuente de verdad: los umbrales y parámetros viven en config.py (y los
ajustados por simulación en resultados/resultados_v2.json). El firmware nunca
se edita a mano para cambiar un número.
"""
import json
import os
import numpy as np

from config import (SEGURIDAD as SG, ALERTAS as AL, SENSORES as S, PANEL, EDS, POLITICA,
                    PASOS_DIA, DT_MIN)
from caracteristicas import TABLA_SIN, TABLA_COS

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def f32(v):
    t = f"{float(np.float32(v)):.9g}"
    if "." not in t and "e" not in t:
        t += ".0"
    return t + "f"


def dbl(v):
    return f"{float(v):.17g}"


def main():
    par = dict(POLITICA)
    origen = "valores iniciales de config.py"
    ruta = os.path.join(RAIZ, "resultados", "resultados_v2.json")
    if os.path.exists(ruta):
        r = json.load(open(ruta))
        par.update(r["politicas"]["base"]["IA predictiva"]["params"])
        origen = "ajustados en simulación (resultados_v2.json, sitio base, política IA)"
    assert AL["dias_calibracion"] <= 7 and AL["dias_tendencia"] <= 7
    L = []
    a = L.append
    a("/* =========================================================================")
    a(" * sx_config.h — GENERADO por python/generar_config_c.py (NO editar a mano)")
    a(f" * Parámetros de política: {origen}")
    a(" * ========================================================================= */")
    a("#ifndef SX_CONFIG_H\n#define SX_CONFIG_H\n")
    a(f"#define SX_DT_MIN {DT_MIN}")
    a(f"#define SX_PASOS_DIA {PASOS_DIA}")
    a(f"#define SX_PASOS_MIN_CICLO {EDS['min_entre_ciclos_min'] // DT_MIN}")
    a(f"#define SX_PASOS_REF {EDS['ref_periodo_min'] // DT_MIN}")
    a(f"#define SX_PASO_PRUEBA {int(AL['hora_prueba_carga'] * 60 // DT_MIN)}")
    a("\n/* --- capa de seguridad (comparaciones en doble, igual que Python) --- */")
    a(f"#define SX_MARGEN_ROCIO_MIN {dbl(SG['margen_rocio_min'])}")
    a(f"#define SX_HR_MAX {dbl(SG['hr_max'])}")
    a(f"#define SX_G_FUGA_MAX {dbl(SG['g_fuga_max_us'])}")
    a(f"#define SX_DC_MAX {dbl(SG['dc_max'])}")
    a(f"#define SX_T_VIDRIO_MIN {dbl(SG['t_vidrio_rango'][0])}")
    a(f"#define SX_T_VIDRIO_MAX {dbl(SG['t_vidrio_rango'][1])}")
    a(f"#define SX_T_AIRE_MIN {dbl(SG['t_aire_rango'][0])}")
    a(f"#define SX_T_AIRE_MAX {dbl(SG['t_aire_rango'][1])}")
    a(f"#define SX_DIF_VIDRIO_AIRE_MAX {dbl(SG['dif_vidrio_aire_max'])}")
    a(f"#define SX_C_REL_MIN {dbl(SG['c_rango_rel'][0])}")
    a(f"#define SX_C_REL_MAX {dbl(SG['c_rango_rel'][1])}")
    a(f"#define SX_PASOS_CONGELADO {SG['pasos_congelado']}")
    a("\n/* --- estimadores y alertas --- */")
    a(f"#define SX_ISC_MIN_VALIDA {f32(S['isc_ref_ma'] * S['g_min_valida'] / 1000.0)}")
    a(f"#define SX_ISC_POR_WM2 {f32(S['isc_ref_ma'] / 1000.0)}")
    a(f"#define SX_G_MIN_TERMICA {f32(AL['g_min_termica'])}")
    a(f"#define SX_FAIMAN_U_TOTAL {f32(PANEL['faiman_u0'] + PANEL['faiman_u1'] * AL['viento_supuesto'])}")
    a(f"#define SX_RESIDUO_TERMICO {f32(AL['residuo_termico'])}")
    a(f"#define SX_DIAS_TERMICA {AL['dias_termica']}")
    a(f"#define SX_DIAS_CALIBRACION {AL['dias_calibracion']}")
    a(f"#define SX_DIAS_TENDENCIA {AL['dias_tendencia']}")
    a(f"#define SX_DELTA_R_ALERTA {f32(AL['delta_r_alerta'])}")
    a(f"#define SX_DELTA_R_FALLA {f32(AL['delta_r_falla'])}")
    a(f"#define SX_ALFA_C {f32(AL['alfa_c'])}")
    a(f"#define SX_BANDA_C {f32(AL['banda_c'])}")
    a(f"#define SX_K_C_PERDIDA {dbl(AL['a_perdida_cal'] / AL['c_por_gm2_cal'])}")
    a(f"#define SX_MARGEN_C_MIN {dbl(AL['margen_c_min'])}")
    a(f"#define SX_G_C_MAX {dbl(AL['g_c_max'])}")
    a("\n/* --- política (por defecto) --- */")
    a(f"#define SX_PAR_UMBRAL_SUELTO {f32(par['umbral_suelto'])}")
    a(f"#define SX_PAR_UMBRAL_PRE {f32(par['umbral_pre'])}")
    a(f"#define SX_PAR_MARGEN_REGLA {f32(par['margen_regla'])}")
    a(f"#define SX_PAR_UMBRAL_COSTRA {f32(par['umbral_costra'])}")
    a(f"#define SX_PAR_ALFA_SR {f32(par['alfa_sr'])}")
    a(f"#define SX_PAR_P_ROCIO {dbl(par['p_rocio'])}")
    a(f"#define SX_PAR_HORA_FIJA_INI {dbl(par['hora_fija_ini'])}")
    a(f"#define SX_PAR_HORA_FIJA_FIN {dbl(par['hora_fija_fin'])}")
    a("\n/* --- seno y coseno de la hora (96 pasos de 15 min) --- */")
    a("static const float SX_SIN_HORA[96] = {" + ",".join(f32(v) for v in TABLA_SIN) + "};")
    a("static const float SX_COS_HORA[96] = {" + ",".join(f32(v) for v in TABLA_COS) + "};")
    a("\n#endif")
    open(os.path.join(RAIZ, "firmware", "sx_config.h"), "w").write("\n".join(L) + "\n")
    print("firmware/sx_config.h escrito |", origen)


if __name__ == "__main__":
    main()
