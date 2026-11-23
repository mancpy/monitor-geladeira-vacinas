"""
Mostra, com horário, todas as mensagens MQTT do protótipo.

Uso:
    pip install paho-mqtt
    python monitor.py
"""

from datetime import datetime

import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
PORTA = 1883
BASE = "rededefrio/geladeira01"


def ao_conectar(cliente, *_):
    print(f"conectado a {BROKER}:{PORTA}, assinando {BASE}/#")
    cliente.subscribe(f"{BASE}/#", qos=1)


def ao_receber(_cliente, _dados, msg):
    hora = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    topico = msg.topic.removeprefix(BASE + "/")
    retida = " (retida)" if msg.retain else ""
    print(f"{hora}  {topico:<12} {msg.payload.decode(errors='replace')}{retida}", flush=True)


cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
cliente.on_connect = ao_conectar
cliente.on_message = ao_receber
cliente.connect(BROKER, PORTA, keepalive=30)
cliente.loop_forever()
