"""
config.py — Parámetros del modelo Spark-X v2 (simulador + sensores + firmware)
=============================================================================

TODO lo que es una SUPOSICIÓN está aquí, en un solo lugar, con su origen.
Si consiguen un dato real (banco, campo, datasheet), cámbienlo aquí y vuelvan
a correr todo:  python correr_todo.py

Marcas usadas en los comentarios:
  [DATO]       viene de una fuente verificable (se indica cuál)
  [SUPUESTO]   valor razonable de ingeniería, NO medido: hay que calibrarlo
  [DISEÑO]     decisión nuestra (umbral, periodo, etc.)
"""

# ---------------------------------------------------------------------------
# 1. SITIO Y TIEMPO
# ---------------------------------------------------------------------------
SITIO = dict(
    nombre="Hermosillo, Sonora",
    lat=29.07, lon=-110.96, altitud_m=210,
    tz="Etc/GMT+7",            # Sonora no usa horario de verano (UTC-7)
)
DT_MIN = 15                    # [DISEÑO] paso de simulación y de decisión del MCU
DT_H = DT_MIN / 60.0
PASOS_DIA = 24 * 60 // DT_MIN  # 96

# ---------------------------------------------------------------------------
# 2. CLIMA — normales 1991-2020 de Hermosillo
# [DATO] Temperatura y lluvia: CONAGUA/SMN, estación Hermosillo II (DGE).
#        Humedad, nubosidad y viento: NASA POWER (modelado).
#        Consultado vía climate-zone.com/climate/mx/hermosillo (oct-2026).
# ---------------------------------------------------------------------------
CLIMA = dict(
    tmax=[25.0, 26.1, 29.4, 32.8, 36.1, 40.0, 39.4, 38.9, 37.8, 34.4, 29.4, 24.4],
    tmin=[10.6, 11.7, 13.9, 16.1, 19.4, 24.4, 26.1, 26.1, 25.0, 20.6, 15.0, 10.6],
    lluvia_mm=[12.7, 17.8, 7.6, 2.5, 2.5, 7.6, 96.5, 101.6, 78.7, 17.8, 22.9, 20.3],
    hr_media=[44, 43, 37, 31, 29, 31, 52, 59, 57, 45, 42, 45],
    nubes=[0.35, 0.39, 0.33, 0.26, 0.19, 0.24, 0.59, 0.54, 0.35, 0.24, 0.30, 0.34],
    viento_ms=[2.2, 2.2, 2.4, 2.6, 2.7, 2.9, 2.4, 2.0, 2.0, 2.1, 2.1, 2.2],
    # [SUPUESTO] días con lluvia por mes (coherente con el total mensual y el monzón)
    dias_lluvia=[2, 2, 1, 0.5, 0.3, 1, 7, 8, 6, 2, 1.5, 2],
    # [SUPUESTO] variabilidad día a día (anomalías AR(1))
    sigma_t=1.8, phi_t=0.7,
    sigma_td=3.0, phi_td=0.8,
)

# ---------------------------------------------------------------------------
# 3. MÓDULO FOTOVOLTAICO
# ---------------------------------------------------------------------------
PANEL = dict(
    p_stc_w=550.0,            # módulo comercial típico (el mismo de las cuentas del banco)
    area_m2=2.58,
    inclinacion=25.0, azimut=180.0,
    gamma_t=-0.0035,          # [DATO típico c-Si] coef. de potencia por °C
    eta_otros=0.97,           # [SUPUESTO] pérdidas de cableado/mismatch
    # Modelo térmico de Faiman (pvlib, valores por defecto)
    faiman_u0=25.0, faiman_u1=6.84,
    tau_termico_min=8.0,      # [SUPUESTO] constante de tiempo térmica del módulo
    emisividad_vidrio=0.90,
)

# ---------------------------------------------------------------------------
# 4. SUCIEDAD Y AGUA SOBRE EL VIDRIO
# ---------------------------------------------------------------------------
SUCIEDAD = dict(
    # Pérdida óptica: SR = exp(-a * masa)  (masa en g/m²)
    a_perdida=0.05,           # [SUPUESTO] ~4.9 % de pérdida por 1 g/m² (orden de la literatura)
    # Depósito base (g/m²/día) y factor mensual
    deposito_base=0.012,      # [SUPUESTO] calibrado para ~4-7 %/3 meses sin limpiar
                              #  (Cabanillas y Munguía 2011, c-Si en Hermosillo) — VERIFICAR
    factor_mes=[0.7, 0.8, 1.0, 1.2, 1.4, 1.5, 1.2, 0.9, 0.8, 0.8, 0.7, 0.7],
    # Eventos de polvo (tolvaneras / outflows del monzón)
    eventos_mes=[0.2, 0.2, 0.4, 0.5, 0.5, 0.8, 1.0, 0.6, 0.3, 0.2, 0.2, 0.2],
    evento_mediana_gm2=0.15, evento_sigma_log=0.6,
    factor_deposito_mojado=1.5,   # [SUPUESTO] el vidrio mojado atrapa más polvo
    # Cementación: polvo suelto -> costra cuando hay película de agua
    k_cementacion_h=0.10,     # [SUPUESTO] fracción/hora del polvo suelto que se cementa al estar mojado
    # Lluvia (efecto al terminar el evento, según mm totales)
    lluvia_ligera_mm=2.0, lluvia_fuerte_mm=8.0,
    ligera_a_lodo=0.5,        # lluvia ligera: 50 % del polvo suelto se vuelve lodo
    media_quita_suelto=0.6, media_quita_cem=0.10,
    fuerte_quita_suelto=0.95, fuerte_quita_cem=0.50,
)

AGUA = dict(
    k_condensa=8.0,           # [SUPUESTO] g/m²/h por °C bajo el punto de rocío
    k_evapora=15.0,           # [SUPUESTO] g/m²/h por °C sobre el punto de rocío
    w_lluvia=300.0,           # g/m² con lámina de lluvia
    w_mojado=5.0,             # [DISEÑO] a partir de aquí se considera "vidrio mojado"
    w_inseguro=2.0,           # [DISEÑO] cualquier película >= esto hace INSEGURO activar AT
)

# ---------------------------------------------------------------------------
# 5. LIMPIEZA
# ---------------------------------------------------------------------------
EDS = dict(
    # Eficiencia por ciclo sobre el polvo SUELTO, cae con la humedad relativa
    # [SUPUESTO] forma lineal; la literatura reporta caídas fuertes con HR alta
    #            (J. Electrostatics 2020) y >90 % en condiciones favorables (Mazumder)
    eff_max=0.92, eff_min=0.30, hr_ref=30.0, pendiente_hr=0.012,
    energia_wh_m2=0.85,       # [DATO] Mas'ud et al. 2026: 0.85 ± 0.12 Wh/m² por ciclo
    min_entre_ciclos_min=60,  # [DISEÑO] desgaste de la etapa de AT
    ref_periodo_min=15,       # [DISEÑO] el mini-EDS de la celda de referencia limpia cada paso
                              # (si limpiara cada hora, tras una tolvanera la 'celda limpia' también
                              #  estaría sucia y el SR medido no vería la suciedad)
)

LAVADO = dict(
    litros_m2=1.5,            # [SUPUESTO] agua por lavado manual (rango típico 0.5-2 L/m²)
    retraso_dias=2,           # [SUPUESTO] la cuadrilla llega 2 días después de la orden
    periodo_sin_eds_dias=30,  # [SUPUESTO] línea base O&M sin EDS: lavado mensual
)

ECONOMIA = dict(tarifa_usd_kwh=0.12)   # [SUPUESTO] tarifa industrial (0.10-0.15)

# ---------------------------------------------------------------------------
# 6. SENSORES  (lo que el MCU realmente ve)
# ---------------------------------------------------------------------------
SENSORES = dict(
    # ADC del STM32
    adc_bits=12, vref=3.3,
    # Riel de 12 V: divisor 47k/10k -> ganancia 5.7 ; fondo de escala 18.8 V
    div_12v=5.7, ruido_adc_lsb=1.5, promedios=16,
    # Fuente de 12 V respaldada por batería (típico en campo): varía con la carga
    v_bat_noche=12.45, v_bat_dia=13.5, v_bat_sigma=0.06,
    r_interna=0.03, r_cableado=0.04,         # ohm
    i_prueba_a=1.0, i_ruido_a=0.005, i_ganancia_sigma=0.01,  # prueba de carga 1 A, 50 ms
    # Sensor de aire SHT40 (con escudo de radiación)
    sht_t_sigma=0.15, sht_t_bias=0.2, sht_hr_sigma=1.0, sht_hr_bias=1.5,
    # NTC en el vidrio
    ntc_sigma=0.3, ntc_bias=0.3,
    # Celdas de referencia (soiling ratio)
    isc_sigma_rel=0.002, ganancia_cal_sigma=0.005, g_min_valida=150.0,
    # AFE de impedancia de electrodos (baja tensión, AT apagada)
    c0_nf=100.0, c0_sigma=0.05, c_por_gm2=0.02, c_por_agua=0.40, c_ruido_rel=0.003,
    g0_us=0.02, g_ruido_rel=0.03, g_piso_us=0.005,
    g_agua_us=8.0, g_agua_polvo_us=20.0,   # conductancia de la película (más con sales del polvo)
    g_polvo_us=0.05,                       # polvo seco higroscópico, crece con HR
    isc_ref_ma=50.0,                       # mini-celda: 50 mA a 1000 W/m²
)

# ---------------------------------------------------------------------------
# 7. FALLAS INYECTADAS (para probar las alertas)
# ---------------------------------------------------------------------------
FALLAS = dict(
    r_contacto_sano=0.015,      # ohm
    r_contacto_alerta=0.15,     # [DISEÑO] mantenimiento requerido
    r_contacto_falla=0.25,      # [DISEÑO] falla (calentamiento / caída inaceptable)
    tau_degradacion_dias=(15, 35),
    hotspot_dt_max=(12.0, 20.0),   # °C extra a 1000 W/m² en el punto del sensor
    ruido_intermitente=0.3,        # un conector flojo da lecturas erráticas
)

# ---------------------------------------------------------------------------
# 8. CAPA DE SEGURIDAD (determinista, NO es ML)  — [DISEÑO]
# ---------------------------------------------------------------------------
SEGURIDAD = dict(
    margen_rocio_min=2.0,       # °C: vidrio debe estar >= 2 °C sobre el punto de rocío
    hr_max=85.0,                # %
    g_fuga_max_us=0.30,         # µS: más fuga = agua/sales sobre electrodos
    dc_max=0.08,                # +8 % de capacitancia = película de agua
    t_vidrio_rango=(-15.0, 85.0),
    t_aire_rango=(-10.0, 55.0),
    dif_vidrio_aire_max=40.0,
    c_rango_rel=(0.5, 3.0),
    pasos_congelado=24,         # 6 h con lectura idéntica = sensor congelado
)

# ---------------------------------------------------------------------------
# 9. MODELO DE IA (predictor de rocío)  — [DISEÑO]
# ---------------------------------------------------------------------------
POLITICA = dict(
    # [DISEÑO] valores iniciales; evaluar_v2.py los AJUSTA con años de entrenamiento
    hora_fija_ini=7.0, hora_fija_fin=11.0,   # P1 horario fijo
    umbral_suelto=0.010,                     # P2/P3: limpiar si la pérdida por polvo suelto >= 1 %
    p_rocio=0.5, umbral_pre=0.002,           # P3: limpieza preventiva antes del rocío
    margen_regla=4.0,                        # P3-regla: igual pero con 'margen de rocío <= X °C'
    umbral_costra=0.030,                     # pedir lavado si la costra estimada >= 3 %
    alfa_sr=0.3,
    costo_lavado_usd=1.0,                    # [SUPUESTO] mano de obra + agua por módulo
    costo_ciclo_usd=0.0002,                  # [SUPUESTO] desgaste de relés/etapa AT por ciclo
)

ALERTAS = dict(
    hora_prueba_carga=12.0,
    dias_calibracion=7,                      # línea base de R y del modelo térmico
    delta_r_alerta=0.135,                    # ohm sobre la línea base  (= 0.15 absoluto)
    delta_r_falla=0.235,
    dias_tendencia=7,
    g_min_termica=500.0,                     # W/m² estimados
    residuo_termico=7.0,                     # °C  (mediana diaria del residuo) — ajustado con años de entrenamiento
    dias_termica=4,                          # ... de los últimos 5 días
    viento_supuesto=2.4,                     # m/s, no hay anemómetro
    # Estimador NOCTURNO de polvo por capacitancia (cuando no hay luz para las celdas)
    c_por_gm2_cal=0.02,                      # [CALIBRAR EN BANCO] +2 % de C por g/m² de polvo
    a_perdida_cal=0.05,                      # [CALIBRAR EN BANCO] misma ley que SR = exp(-a*masa)
    alfa_c=0.2,                              # filtro EWMA de la capacitancia
    banda_c=0.004,                           # zona muerta (ruido) en C relativa
    margen_c_min=3.0,                        # °C: solo con vidrio claramente seco
    g_c_max=0.15,                            # µS
)

IA = dict(
    horizonte_h=8.0,            # ¿habrá película de agua en las próximas 8 h? (la capa de seguridad
                                # bloquea ~4.5 h antes del rocío: hay que anticipar más que eso)
    profundidad=6, min_hoja=40,
    semillas_entrenamiento=list(range(0, 6)),   # 6 años sintéticos
    semillas_prueba=list(range(100, 104)),      # 4 años NUNCA vistos
)
