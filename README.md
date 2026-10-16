# Sentry – Monitoramento crítico de ambientes sensíveis (ESP32 + MQTT)

Decisão crítica (gás/temperatura → alarme + exaustor) executada **na borda**, no ESP32.
O MQTT é usado para telemetria (publish) e comandos do operador (subscribe).

## Como executar
**PlatformIO + Wokwi (VS Code):** abra a pasta → `PlatformIO: Build` → `Wokwi: Start Simulator`.
**Wokwi web:** novo projeto ESP32 → cole `src/main.cpp` no `sketch.ino`, `diagram.json` e `libraries.txt`.
Wi-Fi: `Wokwi-GUEST` (sem senha). Broker: `broker.hivemq.com:1883`.

## Pinos
| Componente | GPIO |
|---|---|
| Potenciômetro (gás, simula MQ-2) | 34 |
| DHT22 | 15 |
| PIR | 27 |
| Buzzer | 12 |
| Relé (exaustor) | 14 |
| LED verde / vermelho | 25 / 26 |

## Tópicos (prefixo `sentry/bunker01`)
| Tópico | Dir. | Payload |
|---|---|---|
| `/sensores/gas` | Pub | `{"ppm":412}` |
| `/sensores/temperatura` | Pub | `{"temp_c":27.5,"umid_pct":58.0}` |
| `/sensores/presenca` | Pub | `{"presenca":true}` |
| `/status/alarme` | Pub (retido) | `{"estado":"PERIGO","gas_ppm":620,...}` |
| `/cmd/exaustor` | Sub | `ON` / `OFF` / `AUTO` |
| `/cmd/limiar` | Sub | inteiro 100–900 |
| `/cmd/alarme` | Sub | `SILENCIAR` / `ATIVAR` |

## Roteiro de testes (para os prints)
1. Inicie a simulação e abra o Serial → **P2**.
2. No cliente MQTT, inscreva-se em `sentry/bunker01/#` → **P3**.
3. Publique `ON` em `.../cmd/exaustor` (relé liga), `350` em `.../cmd/limiar`, `SILENCIAR` em `.../cmd/alarme` → **P4/P5**.
4. Gire o potenciômetro acima do limiar → LED vermelho + buzzer + relé, Serial `[BORDA] ... DECISAO LOCAL` → **P6**.
5. Publique `OFF` em `.../cmd/exaustor` durante o perigo: é ignorado (segurança local tem prioridade).
