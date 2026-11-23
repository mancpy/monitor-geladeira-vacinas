"""
Gera o gráfico dos tempos de resposta a partir do CSV produzido por medir.py.

Uso:
    pip install matplotlib
    python grafico.py ../medicoes/medicoes.csv ../docs/img/tempos.png
"""

import csv
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

NOMES = {"ds18b20": "DS18B20 (sensor)", "rele": "Relé", "led": "LED", "buzzer": "Buzzer"}
ORDEM = ["ds18b20", "rele", "led", "buzzer"]


def main():
    entrada, saida = sys.argv[1], sys.argv[2]
    tempos = defaultdict(list)
    with open(entrada, encoding="utf-8") as f:
        for linha in csv.DictReader(f):
            tempos[linha["item"]].append(float(linha["tempo_ms"]))

    fig, ax = plt.subplots(figsize=(8, 4.2), dpi=160)
    largura = 0.18
    cinzas = ["#2b2b2b", "#555555", "#808080", "#aaaaaa"]
    rotulos = []
    for g, item in enumerate(ORDEM):
        valores = tempos[item]
        for i, v in enumerate(valores):
            x = g + (i - (len(valores) - 1) / 2) * largura
            ax.bar(x, v, largura * 0.92, color=cinzas[i % 4],
                   label=f"Medida {i + 1}" if g == 0 else None)
        media = sum(valores) / len(valores)
        ax.hlines(media, g - 0.42, g + 0.42, colors="#c53333", linewidth=2,
                  label="Média" if g == 0 else None)
        rotulos.append(f"{NOMES[item]}\nmédia {media:.0f} ms")

    ax.set_xticks(range(len(ORDEM)))
    ax.set_xticklabels(rotulos)
    ax.set_ylabel("Tempo de resposta (ms)")
    ax.set_xlim(-0.6, len(ORDEM) - 0.4)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", linewidth=0.4, alpha=0.6)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, fontsize=8, ncol=5, loc="upper right")
    fig.tight_layout()
    fig.savefig(saida)


if __name__ == "__main__":
    main()
