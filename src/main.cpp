/*
 * ============================================================================
 *  SENTRY - Monitoramento crítico de ambientes sensíveis (borda + MQTT)
 *  Plataforma: ESP32 (Wokwi) | Protocolo: MQTT (PubSubClient)
 *
 *  Princípio de projeto:
 *   - A DECISÃO CRÍTICA (gás/temperatura -> alarme + exaustor) roda 100% no
 *     ESP32, a cada ciclo do loop(), ANTES de qualquer rotina de rede.
 *     Se Wi-Fi ou broker caírem, o Sentry continua protegendo o ambiente.
 *   - O MQTT serve para TELEMETRIA (publish) e COMANDOS do operador (subscribe).
 *   - Comando remoto NUNCA se sobrepõe à segurança: em PERIGO o exaustor
 *     permanece ligado mesmo que alguém envie "OFF".
 * ============================================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ------------------------------- PINOS --------------------------------------
#define PIN_GAS          34   // Sensor 1: potenciômetro (simula MQ-2)  [ADC1]
#define PIN_DHT          15   // Sensor 2: DHT22 (temperatura/umidade)
#define PIN_PIR          27   // Sensor 3: PIR (presença humana)
#define PIN_BUZZER       12   // Atuador 1: buzzer (alarme sonoro)
#define PIN_RELE         14   // Atuador 2: relé (exaustor / válvula de corte)
#define PIN_LED_VERDE    25   // Atuador 3a: LED verde  = SEGURO
#define PIN_LED_VERMELHO 26   // Atuador 3b: LED vermelho = PERIGO
#define DHTTYPE          DHT22
#define BUZZER_CANAL     0    // canal LEDC usado para gerar o tom do buzzer

// ------------------------------- REDE ---------------------------------------
const char* WIFI_SSID = "Wokwi-GUEST";   // rede padrão do simulador Wokwi
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// ------------------- TÓPICOS MQTT (hierarquia organizada) -------------------
// sentry/<local>/<categoria>/<assunto>
// Troque "bunker01" por algo único se mais de uma dupla usar o mesmo broker.
#define BASE "sentry/bunker01"
// PUBLICADOS (ESP32 -> broker)
#define T_PUB_GAS        BASE "/sensores/gas"
#define T_PUB_TEMP       BASE "/sensores/temperatura"
#define T_PUB_PRESENCA   BASE "/sensores/presenca"
#define T_PUB_STATUS     BASE "/status/alarme"
// ASSINADOS (broker -> ESP32)
#define T_SUB_EXAUSTOR   BASE "/cmd/exaustor"   // ON | OFF | AUTO
#define T_SUB_LIMIAR     BASE "/cmd/limiar"     // número inteiro 100..900 (ppm)
#define T_SUB_ALARME     BASE "/cmd/alarme"     // SILENCIAR | ATIVAR

// ---------------------------- PARÂMETROS DE SEGURANÇA -----------------------
int   limiarGas        = 400;     // ppm - ajustável via MQTT (cmd/limiar)
const float TEMP_LIMITE = 50.0;   // °C  - risco de ignição
const float HISTERESE   = 0.90;   // só volta a "SEGURO" abaixo de 90% do limiar
const float TEMP_HIST   = 2.0;    // °C de histerese para a temperatura

// ------------------------------- ESTADO -------------------------------------
enum ModoExaustor { EX_AUTO, EX_ON, EX_OFF };
ModoExaustor modoExaustor = EX_AUTO;

int   gasPpm       = 0;
float temperatura  = 25.0;
float umidade      = 50.0;
bool  presenca     = false;
bool  perigo       = false;        // estado de emergência (decidido na borda)
bool  buzzerSilenciado = false;    // operador reconheceu o alarme
bool  releLigado   = false;
bool  buzzerSoando = false;
const char* origemMudanca = "borda"; // quem causou a última mudança de estado

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
DHT          dht(PIN_DHT, DHTTYPE);

unsigned long tLeituraGas = 0, tLeituraDht = 0, tTelemetria = 0;
unsigned long tSerial = 0, tTentativaMqtt = 0, tTentativaWifi = 0;

// ------------------------------ UTILITÁRIOS ---------------------------------
const char* nomeModo() {
  return modoExaustor == EX_ON ? "ON" : (modoExaustor == EX_OFF ? "OFF" : "AUTO");
}

// Publica somente se o broker estiver acessível (a borda nunca fica esperando)
void publica(const char* topico, const char* payload, bool retido = false) {
  if (!mqtt.connected()) return;
  mqtt.publish(topico, payload, retido);
  Serial.printf("[MQTT] -> %s : %s\n", topico, payload);
}

void publicaStatus() {
  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"estado\":\"%s\",\"gas_ppm\":%d,\"limiar\":%d,\"exaustor\":%s,"
    "\"buzzer\":%s,\"modo_exaustor\":\"%s\",\"origem\":\"%s\"}",
    perigo ? "PERIGO" : "SEGURO", gasPpm, limiarGas,
    releLigado ? "true" : "false", buzzerSoando ? "true" : "false",
    nomeModo(), origemMudanca);
  publica(T_PUB_STATUS, buf, true);   // retido: quem conectar depois vê o estado atual
}

void publicaTelemetria() {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ppm\":%d}", gasPpm);
  publica(T_PUB_GAS, buf);
  snprintf(buf, sizeof(buf), "{\"temp_c\":%.1f,\"umid_pct\":%.1f}", temperatura, umidade);
  publica(T_PUB_TEMP, buf);
  snprintf(buf, sizeof(buf), "{\"presenca\":%s}", presenca ? "true" : "false");
  publica(T_PUB_PRESENCA, buf);
}

// Lê o "sensor de gás" (média de 8 amostras) e converte 0..4095 -> 0..1000 ppm
int lerGasPpm() {
  long soma = 0;
  for (int i = 0; i < 8; i++) soma += analogRead(PIN_GAS);
  return map(soma / 8, 0, 4095, 0, 1000);
}

// ============================================================================
//  >>> DECISÃO NA BORDA (R5) <<<
//  Roda a cada ciclo do loop(), usa só variáveis locais (sensores) e aciona os
//  atuadores diretamente. NÃO consulta Wi-Fi nem broker.
// ============================================================================
void controleBorda() {
  bool gasAlto   = gasPpm >= limiarGas;
  bool tempAlta  = temperatura >= TEMP_LIMITE;
  bool gasOk     = gasPpm < (int)(limiarGas * HISTERESE);
  bool tempOk    = temperatura < (TEMP_LIMITE - TEMP_HIST);

  bool novoPerigo = perigo;
  if (!perigo && (gasAlto || tempAlta)) novoPerigo = true;       // entra em PERIGO
  else if (perigo && gasOk && tempOk)   novoPerigo = false;      // volta a SEGURO

  if (novoPerigo != perigo) {                    // houve transição de estado
    perigo = novoPerigo;
    origemMudanca = "borda";
    if (!perigo) buzzerSilenciado = false;       // rearma o alarme para a próxima vez
    if (perigo) {
      Serial.printf("[BORDA] PERIGO! gas=%d ppm (limiar %d) temp=%.1fC -> "
                    "exaustor LIGADO + alarme ACIONADO | DECISAO LOCAL (broker %s)\n",
                    gasPpm, limiarGas, temperatura,
                    mqtt.connected() ? "online" : "OFFLINE");
    } else {
      Serial.printf("[BORDA] Ambiente SEGURO: gas=%d ppm temp=%.1fC -> "
                    "alarme desligado | DECISAO LOCAL\n", gasPpm, temperatura);
    }
    // (a atualização do relé/LEDs/buzzer abaixo acontece no MESMO ciclo)
  }

  // --- Atuador 2: relé do exaustor (segurança tem prioridade sobre comando) ---
  releLigado = perigo || (modoExaustor == EX_ON);
  digitalWrite(PIN_RELE, releLigado ? HIGH : LOW);

  // --- Atuador 3: LEDs de estado ---
  digitalWrite(PIN_LED_VERMELHO, perigo ? HIGH : LOW);
  digitalWrite(PIN_LED_VERDE,    perigo ? LOW  : HIGH);

  // --- Atuador 1: buzzer. PIR muda o padrão: com pessoas na zona, bipa mais rápido ---
  bool soar = false;
  if (perigo && !buzzerSilenciado) {
    unsigned long periodo = presenca ? 200 : 600;           // ms
    soar = ((millis() / periodo) % 2) == 0;                 // liga/desliga intermitente
  }
  if (soar != buzzerSoando) {
    buzzerSoando = soar;
    ledcWriteTone(BUZZER_CANAL, soar ? 2000 : 0);
  }
}

// ============================================================================
//  >>> DECISÃO VIA MQTT (R6) <<<
//  Chamado pela biblioteca sempre que chega mensagem em um tópico ASSINADO.
// ============================================================================
void callback(char* topico, byte* payload, unsigned int tamanho) {
  char msg[32];
  unsigned int n = tamanho < sizeof(msg) - 1 ? tamanho : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  String valor = String(msg);
  valor.trim();
  valor.toUpperCase();
  String t = String(topico);

  Serial.printf("[MQTT] <- %s : %s\n", topico, valor.c_str());

  if (t == T_SUB_EXAUSTOR) {                         // comando do exaustor
    if (valor == "ON")        { modoExaustor = EX_ON;   Serial.println("[CMD] Exaustor FORCADO LIGADO (remoto)"); }
    else if (valor == "OFF")  {
      modoExaustor = EX_OFF;
      if (perigo) Serial.println("[CMD] OFF registrado, mas PERIGO ativo: exaustor segue LIGADO (prioridade da borda)");
      else        Serial.println("[CMD] Exaustor DESLIGADO (remoto)");
    }
    else if (valor == "AUTO") { modoExaustor = EX_AUTO; Serial.println("[CMD] Exaustor em modo AUTOMATICO"); }
    else { Serial.println("[CMD] Valor invalido para exaustor (use ON|OFF|AUTO)"); return; }
  }
  else if (t == T_SUB_LIMIAR) {                      // ajuste de limiar de gás
    int v = valor.toInt();
    if (v >= 100 && v <= 900) {
      limiarGas = v;
      Serial.printf("[CMD] Novo limiar de gas: %d ppm\n", limiarGas);
    } else {
      Serial.println("[CMD] Limiar fora da faixa 100..900 - ignorado");
      return;
    }
  }
  else if (t == T_SUB_ALARME) {                      // reconhecimento do alarme
    if (valor == "SILENCIAR") {
      buzzerSilenciado = true;
      Serial.println("[CMD] Buzzer SILENCIADO (LED vermelho e exaustor continuam)");
    } else if (valor == "ATIVAR") {
      buzzerSilenciado = false;
      Serial.println("[CMD] Buzzer REATIVADO");
    } else { Serial.println("[CMD] Valor invalido para alarme (use SILENCIAR|ATIVAR)"); return; }
  }

  origemMudanca = "mqtt";
  controleBorda();      // aplica o efeito imediatamente nos atuadores
  publicaStatus();      // confirma o novo estado para o operador
}

// ------------------------------ CONEXÕES ------------------------------------
void conectaWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Conectando");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {  // no máx. 10 s
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[WIFI] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[WIFI] Sem Wi-Fi - Sentry segue operando em modo AUTONOMO (borda)");
}

void conectaMqtt() {
  String id = "sentry-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Conectando a %s:%u ...\n", MQTT_HOST, MQTT_PORT);
  if (mqtt.connect(id.c_str())) {
    Serial.println("[MQTT] Conectado ao broker!");
    mqtt.subscribe(T_SUB_EXAUSTOR); Serial.println("[MQTT] Assinado: " T_SUB_EXAUSTOR);
    mqtt.subscribe(T_SUB_LIMIAR);   Serial.println("[MQTT] Assinado: " T_SUB_LIMIAR);
    mqtt.subscribe(T_SUB_ALARME);   Serial.println("[MQTT] Assinado: " T_SUB_ALARME);
    publicaStatus();
  } else {
    Serial.printf("[MQTT] Falha (rc=%d). Nova tentativa em 5 s (borda segue ativa)\n", mqtt.state());
  }
}

// Reconexão SEM bloquear a lógica crítica: tenta no máximo a cada 5 s
void gerenciaRede() {
  unsigned long agora = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (agora - tTentativaWifi > 5000) { tTentativaWifi = agora; WiFi.reconnect(); }
    return;
  }
  if (!mqtt.connected()) {
    if (agora - tTentativaMqtt > 5000) { tTentativaMqtt = agora; conectaMqtt(); }
  } else {
    mqtt.loop();   // processa mensagens recebidas -> callback()
  }
}

// -------------------------------- SETUP -------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SENTRY - Monitoramento critico (borda + MQTT) ===");

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  ledcSetup(BUZZER_CANAL, 2000, 8);
  ledcAttachPin(PIN_BUZZER, BUZZER_CANAL);
  ledcWriteTone(BUZZER_CANAL, 0);
  digitalWrite(PIN_RELE, LOW);
  digitalWrite(PIN_LED_VERDE, HIGH);

  dht.begin();
  gasPpm = lerGasPpm();

  conectaWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(callback);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(2);   // evita bloquear o loop por muito tempo
  if (WiFi.status() == WL_CONNECTED) conectaMqtt();
}

// --------------------------------- LOOP -------------------------------------
void loop() {
  unsigned long agora = millis();

  // 1) LEITURA DOS SENSORES (sempre, independente da rede)
  if (agora - tLeituraGas >= 200) {
    tLeituraGas = agora;
    gasPpm = lerGasPpm();
    bool p = digitalRead(PIN_PIR) == HIGH;
    if (p != presenca) {
      presenca = p;
      Serial.printf("[PIR] Presenca %s\n", presenca ? "DETECTADA" : "ausente");
      char b[40];
      snprintf(b, sizeof(b), "{\"presenca\":%s}", presenca ? "true" : "false");
      publica(T_PUB_PRESENCA, b);
    }
  }
  if (agora - tLeituraDht >= 2000) {              // DHT22: mínimo 2 s entre leituras
    tLeituraDht = agora;
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t)) temperatura = t;
    if (!isnan(h)) umidade = h;
  }

  // 2) DECISÃO LOCAL (R5) - prioridade máxima
  bool estadoAntes = perigo;
  controleBorda();
  if (perigo != estadoAntes) publicaStatus();     // avisa o broker SE houver conexão

  // 3) REDE: reconexão não bloqueante + recebimento de comandos
  gerenciaRede();

  // 4) TELEMETRIA a cada 3 s
  if (agora - tTelemetria >= 3000) {
    tTelemetria = agora;
    publicaTelemetria();
  }

  // 5) LOG SERIAL a cada 2 s
  if (agora - tSerial >= 2000) {
    tSerial = agora;
    Serial.printf("[LEITURA] gas=%d ppm | temp=%.1fC | umid=%.0f%% | presenca=%d | "
                  "estado=%s | exaustor=%s(modo %s) | broker=%s\n",
                  gasPpm, temperatura, umidade, presenca, perigo ? "PERIGO" : "SEGURO",
                  releLigado ? "ON" : "OFF", nomeModo(),
                  mqtt.connected() ? "online" : "OFFLINE");
  }
  delay(10);
}
