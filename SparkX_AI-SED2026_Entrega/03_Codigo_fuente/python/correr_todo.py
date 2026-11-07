"""
correr_todo.py — Reproduce TODO desde cero (≈30 min en una laptop de 2 núcleos)

  1. entrena el predictor de rocío y lo exporta a C
  2. evalúa políticas, seguridad, alertas y tolvanera+lluvia (antes/después)
  3. genera firmware/sx_config.h con los parámetros ajustados
  4. valida el C contra Python (software-in-the-loop)
  5. genera el demo, compila el firmware y lo corre en la PC
  6. hace las figuras
"""
import os
import subprocess
import sys
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
PASOS = ["entrenar_rocio.py", "evaluar_v2.py", "generar_config_c.py", "sil_validacion.py",
         "generar_demo.py", "graficas.py"]

if __name__ == "__main__":
    t0 = time.time()
    for p in PASOS:
        print(f"\n######## {p} ########", flush=True)
        subprocess.run([sys.executable, os.path.join(AQUI, p)], check=True, cwd=AQUI)
        if p == "generar_demo.py":
            fw = os.path.join(os.path.dirname(AQUI), "firmware")
            subprocess.run(["make", "-B", "build/demo"], check=True, cwd=fw)
            with open(os.path.join(os.path.dirname(AQUI), "resultados", "demo_terminal.txt"), "w") as fh:
                subprocess.run(["./build/demo"], check=True, cwd=fw, stdout=fh)
    print(f"\nListo en {(time.time() - t0) / 60:.1f} min")
