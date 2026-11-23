"""
Mede os tempos de resposta do protótipo pelo broker MQTT.

Atuadores: publica um comando de teste (TESTE_RELE, TESTE_LED, TESTE_BUZZER) e
mede o tempo até chegar a confirmação que o ESP32 publica em .../atuadores logo
depois de acionar a GPIO. O tempo inclui a ida do comando e a volta da
confirmação.

Sensor: registra as mensagens de .../latencia, em que o próprio ESP32 informa
quanto tempo passou entre a leitura do DS18B20 e o retorno da publicação pelo
broker. Esse tempo é contado pelo relógio do ESP32 (millis). No simulador esse
relógio pode andar mais devagar que o tempo real, então o script compara o
millis de duas mensagens seguidas com o relógio do computador, calcula a
velocidade da simulação no intervalo e converte a medida para tempo real.

Uso:
    pip install paho-mqtt
    python medir.py                 # 4 medições por item, grava medicoes.csv
    python medir.py -n 4 -o saida.csv
"""

import argparse
import csv
import json
import threading
import time

import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
PORTA = 1883
BASE = "rededefrio/geladeira01"
ATUADORES = {"rele": "TESTE_RELE", "led": "TESTE_LED", "buzzer": "TESTE_BUZZER"}
DURACAO_TESTE = 3.0  # s, igual ao firmware


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=4, help="medições por item")
    ap.add_argument("-o", default="medicoes.csv", help="arquivo de saída")
    ap.add_argument("--folga", type=float, default=1.0,
                    help="multiplica a espera entre comandos (simulador lento)")
    args = ap.parse_args()

    linhas = []                 # (item, numero, tempo_ms, bruto_ms, velocidade)
    confirmacao = threading.Event()
    esperado = {}               # atuador aguardado no momento
    latencias = []

    def ao_conectar(cliente, *_):
        cliente.subscribe([(f"{BASE}/atuadores", 0), (f"{BASE}/latencia", 0),
                           (f"{BASE}/status", 0)])

    def ao_receber(_cliente, _dados, msg):
        agora = time.perf_counter()
        texto = msg.payload.decode(errors="replace")
        if msg.topic.endswith("/status"):
            print(f"status: {texto}")
        elif msg.topic.endswith("/latencia") and not msg.retain:
            dados = json.loads(texto)
            latencias.append((dados["sensor_ms"], dados["millis"], agora))
            print(f"sensor: {dados['sensor_ms']} ms no relógio do ESP32")
        elif msg.topic.endswith("/atuadores") and esperado:
            estado = json.loads(texto)
            if estado.get("origem") == "comando" and estado.get(esperado["nome"]) == 1:
                esperado["chegada"] = agora
                confirmacao.set()

    cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    cliente.on_connect = ao_conectar
    cliente.on_message = ao_receber
    cliente.connect(BROKER, PORTA, keepalive=30)
    cliente.loop_start()
    time.sleep(2)

    for nome, comando in ATUADORES.items():
        for i in range(1, args.n + 1):
            confirmacao.clear()
            esperado.clear()
            esperado["nome"] = nome
            inicio = time.perf_counter()
            cliente.publish(f"{BASE}/comando", comando, qos=1)
            if confirmacao.wait(timeout=10):
                ms = (esperado["chegada"] - inicio) * 1000
                linhas.append((nome, i, round(ms, 1), "", ""))
                print(f"{nome}: {ms:.1f} ms (medida {i})")
            else:
                print(f"{nome}: sem confirmação em 10 s (medida {i})")
            esperado.clear()
            time.sleep(DURACAO_TESTE * args.folga + 2)  # espera o teste terminar

    # o sensor publica a cada 30 s: espera juntar n medidas
    # precisa de n + 1 mensagens: a primeira só serve de referência de relógio
    limite = time.time() + 45 * (args.n + 1) * args.folga
    while len(latencias) < args.n + 1 and time.time() < limite:
        time.sleep(1)
    for i in range(1, min(len(latencias), args.n + 1)):
        _, millis_antes, pc_antes = latencias[i - 1]
        bruto, millis_agora, pc_agora = latencias[i]
        velocidade = (millis_agora - millis_antes) / ((pc_agora - pc_antes) * 1000)
        linhas.append(("ds18b20", i, round(bruto / velocidade, 1), bruto,
                       round(velocidade, 3)))

    cliente.loop_stop()
    cliente.disconnect()

    with open(args.o, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["item", "medida", "tempo_ms", "esp32_ms", "velocidade_simulacao"])
        w.writerows(linhas)
    print(f"\n{len(linhas)} medições gravadas em {args.o}")


if __name__ == "__main__":
    main()
