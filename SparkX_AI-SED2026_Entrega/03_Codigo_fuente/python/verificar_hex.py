"""
verificar_hex.py — Comprueba que el modelo de IA que va DENTRO de cada .hex es
exactamente el árbol entrenado (resultados/modelo_rocio.json).

1. Lee cada archivo .hex (formato Intel HEX, el mismo que se carga en Proteus o
   en el STM32 real) y reconstruye la memoria flash byte por byte.
2. Busca en el .elf correspondiente la dirección de las 5 tablas del árbol
   (SX_ARBOL_FEAT/THR/IZQ/DER/PROB) y las extrae de la imagen del .hex.
3. Compara tabla por tabla contra el modelo de Python (float32 exacto).
4. Recorre el árbol leído del .hex con 50 000 vectores de prueba y compara la
   probabilidad contra el árbol de Python: deben dar idéntico.

Uso:  python3 verificar_hex.py  [carpeta_build]
"""
import json
import os
import struct
import subprocess
import sys

import numpy as np

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(RAIZ, "firmware", "build")
MODELO = os.path.join(RAIZ, "resultados", "modelo_rocio.json")
TABLAS = {"SX_ARBOL_FEAT": "h", "SX_ARBOL_THR": "f", "SX_ARBOL_IZQ": "h",
          "SX_ARBOL_DER": "h", "SX_ARBOL_PROB": "f"}


def leer_ihex(ruta):
    mem, base = {}, 0
    with open(ruta) as f:
        for ln in f:
            ln = ln.strip()
            if not ln.startswith(":"):
                continue
            b = bytes.fromhex(ln[1:])
            n, dir16, tipo = b[0], (b[1] << 8) | b[2], b[3]
            datos = b[4:4 + n]
            if (sum(b) & 0xFF) != 0:
                raise ValueError(f"checksum malo en {ruta}: {ln}")
            if tipo == 0:
                for i, v in enumerate(datos):
                    mem[base + dir16 + i] = v
            elif tipo == 4:
                base = ((datos[0] << 8) | datos[1]) << 16
            elif tipo == 1:
                break
    return mem


def simbolos(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True, check=True).stdout
    s = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[3] in TABLAS:
            s[p[3]] = (int(p[0], 16), int(p[1], 16))
    return s


def tablas_de_hex(hexf, elf):
    mem = leer_ihex(hexf)
    t = {}
    for nom, (dir_, tam) in simbolos(elf).items():
        raw = bytes(mem[dir_ + i] for i in range(tam))
        fmt = TABLAS[nom]
        t[nom] = np.array(struct.unpack("<" + fmt * (tam // struct.calcsize(fmt)), raw))
    return t


def prob(t, x):
    n = 0
    while t["SX_ARBOL_FEAT"][n] >= 0:
        f = t["SX_ARBOL_FEAT"][n]
        n = t["SX_ARBOL_IZQ"][n] if np.float32(x[f]) <= np.float32(t["SX_ARBOL_THR"][n]) else t["SX_ARBOL_DER"][n]
    return float(np.float32(t["SX_ARBOL_PROB"][n]))


def main():
    m = json.load(open(MODELO))
    ref = {"SX_ARBOL_FEAT": np.array(m["feature"]), "SX_ARBOL_THR": np.array(m["threshold"], dtype=np.float32),
           "SX_ARBOL_IZQ": np.array(m["left"]), "SX_ARBOL_DER": np.array(m["right"]),
           "SX_ARBOL_PROB": np.array(m["prob"], dtype=np.float32)}
    ref["SX_ARBOL_THR"] = np.where(ref["SX_ARBOL_FEAT"] < 0, ref["SX_ARBOL_THR"], ref["SX_ARBOL_THR"])

    rng = np.random.default_rng(2026)
    # vectores de prueba cubriendo el rango físico de las 10 características
    lo = np.array([-15, -6, -10, 2, -30, -5, -6, -8, -1, -1], dtype=np.float32)
    hi = np.array([35, 6, 10, 100, 30, 48, 6, 8, 1, 1], dtype=np.float32)
    X = (lo + (hi - lo) * rng.random((50000, 10))).astype(np.float32)
    p_ref = np.array([prob(ref, x) for x in X], dtype=np.float32)

    hexs = sorted(f for f in os.listdir(BUILD) if f.endswith(".hex"))
    res = {"modelo": {"nodos": int(m["n_nodos"]), "bytes_flash": 0}, "archivos": []}
    todo_ok = True
    for h in hexs:
        elf = os.path.join(BUILD, h[:-4] + ".elf")
        if not os.path.exists(elf):
            continue
        t = tablas_de_hex(os.path.join(BUILD, h), elf)
        iguales = {k: bool(np.array_equal(t[k].astype(ref[k].dtype), ref[k])) for k in TABLAS}
        p = np.array([prob(t, x) for x in X], dtype=np.float32)
        dif = int(np.sum(p != p_ref))
        ok = all(iguales.values()) and dif == 0
        todo_ok &= ok
        res["modelo"]["bytes_flash"] = int(sum(v.nbytes if v.dtype != np.int64 else v.size * 2 for v in t.values()))
        res["archivos"].append({"hex": h, "tablas_identicas": iguales, "vectores": len(X),
                                "diferencias_inferencia": dif, "ok": ok})
        print(f"{h:28s} tablas={'OK' if all(iguales.values()) else 'DIFERENTES'}  "
              f"inferencia {len(X)} vectores, diferencias={dif}  -> {'OK' if ok else 'FALLA'}")
    res["todo_ok"] = bool(todo_ok)
    out = os.path.join(RAIZ, "resultados", "verificacion_hex.json")
    json.dump(res, open(out, "w"), indent=1)
    print("RESUMEN:", "TODOS LOS .hex LLEVAN EL MODELO ENTRENADO EXACTO" if todo_ok else "HAY DIFERENCIAS")
    sys.exit(0 if todo_ok else 1)


if __name__ == "__main__":
    main()
