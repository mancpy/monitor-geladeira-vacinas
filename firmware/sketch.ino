/*
 * Monitoramento de temperatura da geladeira de vacinas com ESP32 e MQTT.
 *
 * Sensor:    DS18B20 (1-Wire) na GPIO 4
 * Atuadores: relé (refrigeração) na GPIO 26, LED na GPIO 25, buzzer na GPIO 27
 * Rede:      Wi-Fi + MQTT (broker.hivemq.com:1883)
 *
 * Tópicos (prefixo rededefrio/geladeira01):
 *   temperatura  publica  leitura em °C a cada 30 s
 *   alarme       publica  NORMAL, ALTA ou BAIXA (retida)
 *   status       publica  online; offline pelo Last Will (retida)
 *   atuadores    publica  estado de relé, LED e buzzer a cada mudança
 *   latencia     publica  tempo entre a leitura do sensor e o retorno da
 *                         mensagem pelo broker, em ms
 *   comando      assina   TESTE_RELE, TESTE_LED, TESTE_BUZZER, SILENCIAR
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------------------------------------------------------- rede
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_SENHA = "";
const char *BROKER = "broker.hivemq.com";
const uint16_t BROKER_PORTA = 1883;

#define BASE "rededefrio/geladeira01"
const char *T_TEMPERATURA = BASE "/temperatura";
const char *T_ALARME = BASE "/alarme";
const char *T_STATUS = BASE "/status";
const char *T_ATUADORES = BASE "/atuadores";
const char *T_LATENCIA = BASE "/latencia";
const char *T_COMANDO = BASE "/comando";

// ---------------------------------------------------------------- pinos
const uint8_t PINO_SENSOR = 4;
const uint8_t PINO_LED = 25;
const uint8_t PINO_RELE = 26;
const uint8_t PINO_BUZZER = 27;

// ------------------------------------------------------- regras de negócio
const float TEMP_MIN = 2.0;        // limite inferior da rede de frio (°C)
const float TEMP_MAX = 8.0;        // limite superior da rede de frio (°C)
const float HISTERESE = 0.5;       // margem para encerrar o alarme (°C)
const float TEMP_DESLIGA_RELE = 6.0;

const unsigned long INTERVALO_LEITURA = 5000;      // ms
const unsigned long TEMPO_CONVERSAO = 750;         // ms, DS18B20 em 12 bits
const unsigned long INTERVALO_PUBLICACAO = 30000;  // ms
const unsigned long INTERVALO_RECONEXAO = 5000;    // ms
const uint16_t KEEPALIVE = 90;                     // s, folga para o simulador
const unsigned long DURACAO_TESTE = 3000;          // ms
const unsigned long MEIO_PERIODO_BUZZER = 500;     // ms
const unsigned int FREQ_BUZZER = 2000;             // Hz

enum Alarme { NORMAL, ALTA, BAIXA };
const char *NOME_ALARME[] = {"NORMAL", "ALTA", "BAIXA"};

// ---------------------------------------------------------------- estado
WiFiClient wifi;
PubSubClient mqtt(wifi);
OneWire barramento(PINO_SENSOR);
DallasTemperature sensor(&barramento);

Alarme alarme = NORMAL;
bool refrigeracao = false;   // decisão automática para o relé
bool silenciado = false;     // comando SILENCIAR durante um alarme
float ultimaTemperatura = NAN;

// teste remoto dos atuadores (0 = sem teste em andamento)
unsigned long fimTesteRele = 0, fimTesteLed = 0, fimTesteBuzzer = 0;

// estado físico dos atuadores, para publicar só quando muda
bool estadoRele = false, estadoLed = false, estadoBuzzer = false;

unsigned long ultimaLeitura = 0, inicioConversao = 0, ultimaPublicacao = 0;
unsigned long ultimaTentativaMqtt = 0, ultimaTrocaBuzzer = 0;
bool convertendo = false, somLigado = false, primeiraPublicacao = true;

// medição de latência do sensor
unsigned long instanteLeitura = 0;
bool aguardandoEco = false;
unsigned int numeroMedida = 0;

// ------------------------------------------------------------- publicação
void publicarAtuadores(const char *origem) {
  char msg[96];
  snprintf(msg, sizeof(msg),
           "{\"rele\":%d,\"led\":%d,\"buzzer\":%d,\"origem\":\"%s\"}",
           estadoRele, estadoLed, estadoBuzzer, origem);
  mqtt.publish(T_ATUADORES, msg);
  Serial.printf("[%lu] atuadores %s\n", millis(), msg);
}

void publicarAlarme() {
  mqtt.publish(T_ALARME, NOME_ALARME[alarme], true);
  Serial.printf("[%lu] alarme %s\n", millis(), NOME_ALARME[alarme]);
}

void publicarTemperatura() {
  char msg[16];
  snprintf(msg, sizeof(msg), "%.2f", ultimaTemperatura);
  instanteLeitura = millis();
  aguardandoEco = mqtt.publish(T_TEMPERATURA, msg);
  Serial.printf("[%lu] temperatura %s\n", millis(), msg);
}

// --------------------------------------------------------------- atuadores
// Aplica nas GPIOs a combinação de decisão automática e teste remoto.
// Devolve true se algum atuador mudou de estado.
bool aplicarAtuadores() {
  unsigned long agora = millis();
  bool emAlarme = alarme != NORMAL;

  bool rele = refrigeracao || agora < fimTesteRele;
  bool led = emAlarme || agora < fimTesteLed;
  bool buzzer = (emAlarme && !silenciado) || agora < fimTesteBuzzer;

  bool mudou = rele != estadoRele || led != estadoLed || buzzer != estadoBuzzer;
  estadoRele = rele;
  estadoLed = led;
  estadoBuzzer = buzzer;

  digitalWrite(PINO_RELE, rele ? HIGH : LOW);
  digitalWrite(PINO_LED, led ? HIGH : LOW);

  // buzzer intermitente: meio segundo com som, meio segundo em silêncio
  if (!buzzer) {
    if (somLigado) {
      noTone(PINO_BUZZER);
      somLigado = false;
    }
  } else if (mudou || agora - ultimaTrocaBuzzer >= MEIO_PERIODO_BUZZER) {
    ultimaTrocaBuzzer = agora;
    somLigado = mudou ? true : !somLigado;
    if (somLigado) tone(PINO_BUZZER, FREQ_BUZZER);
    else noTone(PINO_BUZZER);
  }
  return mudou;
}

// ------------------------------------------------------ regra de temperatura
void avaliar(float t) {
  Alarme anterior = alarme;

  // alarme com histerese de 0,5 °C para encerrar
  if (t > TEMP_MAX) alarme = ALTA;
  else if (t < TEMP_MIN) alarme = BAIXA;
  else if (alarme == ALTA && t < TEMP_MAX - HISTERESE) alarme = NORMAL;
  else if (alarme == BAIXA && t > TEMP_MIN + HISTERESE) alarme = NORMAL;

  // refrigeração: liga acima de 8 °C, desliga em 6 °C, nunca abaixo de 2 °C
  if (t > TEMP_MAX) refrigeracao = true;
  else if (t <= TEMP_DESLIGA_RELE) refrigeracao = false;

  if (alarme != anterior) {
    if (alarme == NORMAL) silenciado = false;
    publicarAlarme();
  }
}

// ---------------------------------------------------------------- comandos
void aoReceber(char *topico, byte *dados, unsigned int tamanho) {
  // eco da própria publicação de temperatura: mede a latência do sensor
  if (strcmp(topico, T_TEMPERATURA) == 0) {
    if (!aguardandoEco) return;
    aguardandoEco = false;
    unsigned long dt = millis() - instanteLeitura;
    char msg[72];
    snprintf(msg, sizeof(msg), "{\"n\":%u,\"sensor_ms\":%lu,\"millis\":%lu}",
             ++numeroMedida, dt, millis());
    mqtt.publish(T_LATENCIA, msg);
    Serial.printf("[%lu] latencia %s\n", millis(), msg);
    return;
  }

  char comando[24];
  unsigned int n = min(tamanho, (unsigned int)sizeof(comando) - 1);
  memcpy(comando, dados, n);
  comando[n] = '\0';
  Serial.printf("[%lu] comando %s\n", millis(), comando);

  unsigned long fim = millis() + DURACAO_TESTE;
  if (strcmp(comando, "TESTE_RELE") == 0) fimTesteRele = fim;
  else if (strcmp(comando, "TESTE_LED") == 0) fimTesteLed = fim;
  else if (strcmp(comando, "TESTE_BUZZER") == 0) fimTesteBuzzer = fim;
  else if (strcmp(comando, "SILENCIAR") == 0) silenciado = true;
  else return;

  // aciona e confirma na hora, sem esperar a próxima volta do laço
  if (aplicarAtuadores()) publicarAtuadores("comando");
}

// ----------------------------------------------------------------- conexão
void conectarMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (ultimaTentativaMqtt != 0 && millis() - ultimaTentativaMqtt < INTERVALO_RECONEXAO) return;
  ultimaTentativaMqtt = millis();

  String id = "rededefrio-" + String((uint32_t)ESP.getEfuseMac(), HEX) + "-" + String(random(0xffff), HEX);
  Serial.printf("[%lu] conectando ao broker como %s\n", millis(), id.c_str());

  // Last Will: o broker publica "offline" se o ESP32 sumir sem avisar
  if (mqtt.connect(id.c_str(), T_STATUS, 1, true, "offline")) {
    mqtt.publish(T_STATUS, "online", true);
    mqtt.subscribe(T_COMANDO, 1);
    mqtt.subscribe(T_TEMPERATURA);
    publicarAlarme();
    publicarAtuadores("conexao");
    Serial.printf("[%lu] broker conectado\n", millis());
  } else {
    Serial.printf("[%lu] falha no broker, rc=%d\n", millis(), mqtt.state());
  }
}

// ------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);

  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_RELE, LOW);
  digitalWrite(PINO_LED, LOW);
  digitalWrite(PINO_BUZZER, LOW);

  sensor.begin();
  sensor.setResolution(12);
  sensor.setWaitForConversion(false);  // a espera de 750 ms é feita no laço

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
  Serial.print("conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi conectado, IP %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(BROKER, BROKER_PORTA);
  mqtt.setCallback(aoReceber);
  mqtt.setKeepAlive(KEEPALIVE);
}

// -------------------------------------------------------------------- loop
void loop() {
  unsigned long agora = millis();

  conectarMqtt();
  mqtt.loop();

  // 1. pede a conversão ao DS18B20 a cada 5 s
  if (!convertendo && (ultimaLeitura == 0 || agora - ultimaLeitura >= INTERVALO_LEITURA)) {
    sensor.requestTemperatures();
    inicioConversao = agora;
    convertendo = true;
  }

  // 2. depois de 750 ms, lê o resultado e aplica a regra
  if (convertendo && agora - inicioConversao >= TEMPO_CONVERSAO) {
    convertendo = false;
    ultimaLeitura = agora;
    float t = sensor.getTempCByIndex(0);

    if (t == DEVICE_DISCONNECTED_C) {
      Serial.printf("[%lu] leitura invalida, descartada\n", agora);
    } else {
      ultimaTemperatura = t;
      avaliar(t);

      // 3. publica a leitura a cada 30 s, junto com o estado do alarme
      if (mqtt.connected() && (primeiraPublicacao || agora - ultimaPublicacao >= INTERVALO_PUBLICACAO)) {
        primeiraPublicacao = false;
        ultimaPublicacao = agora;
        publicarTemperatura();
        mqtt.publish(T_ALARME, NOME_ALARME[alarme], true);
      }
    }
  }

  // 4. atualiza relé, LED e buzzer; confirma no broker quando algo muda
  if (aplicarAtuadores()) publicarAtuadores("automatico");
}
