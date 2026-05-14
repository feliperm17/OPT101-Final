/*
 * PLACA 7 - NÓ ALARME (LEDs)
 * Hardware: Arduino Nano + nRF24L01 + LED verde D5 + LED vermelho D6
 * Protocolo MACA Four-Way Handshake (como receptor)
 *
 * Correções aplicadas:
 * - Delays aumentados para transição TX/RX
 * - Timeout DATA aumentado
 * - Seq tolerante para evitar necessidade de reset
 */

#include <SPI.h>
#include "RF24.h"

#define CE_PIN     7
#define CSN_PIN    8
#define LED1_PIN   5   // verde
#define LED2_PIN   6   // vermelho

#define ID_P7      7
#define ID_GATEWAY 13
#define CANAL_RF   100

const uint64_t ADDR_P7      = 0x0707070707LL;
const uint64_t ADDR_GATEWAY = 0x1313131313LL;

#define MSG_RTS    0x01
#define MSG_CTS    0x02
#define MSG_DATA   0x03
#define MSG_ACK    0x04
#define MSG_ALARM  0x10
#define MSG_DISARM 0x11

struct Payload {
  uint8_t seq;
  uint8_t origem;
  uint8_t destino;
  uint8_t tipo;
  uint8_t dado1;
  uint8_t dado2;
  uint8_t dado3;
  uint8_t checksum;
};

#define TIMEOUT_DATA_MS 1500
#define DELAY_TX_MS     20
#define DELAY_RX_MS     20

RF24 radio(CE_PIN, CSN_PIN);

bool alarmeAtivo = false;
unsigned long ultimoPisca = 0;
bool estadoLED = false;
#define INTERVALO_PISCA_MS 300

uint8_t calcChecksum(Payload &p) {
  return p.seq ^ p.origem ^ p.destino ^ p.tipo ^ p.dado1 ^ p.dado2 ^ p.dado3;
}

bool checksumOk(Payload &p) {
  return p.checksum == calcChecksum(p);
}

void modoRX() {
  radio.openReadingPipe(1, ADDR_P7);
  radio.startListening();
  delay(DELAY_RX_MS);
}

void modoTX(uint64_t destAddr) {
  radio.stopListening();
  delay(DELAY_TX_MS);
  radio.openWritingPipe(destAddr);
}

// ─── Envia CTS para o Gateway ─────────────────────────────────────────────────
void enviarCTS(uint8_t seq) {
  Payload cts;
  cts.seq      = seq;
  cts.origem   = ID_P7;
  cts.destino  = ID_GATEWAY;
  cts.tipo     = MSG_CTS;
  cts.dado1    = 0;
  cts.dado2    = 0;
  cts.dado3    = 0;
  cts.checksum = calcChecksum(cts);

  modoTX(ADDR_GATEWAY);
  bool ok = radio.write(&cts, sizeof(cts));
  Serial.print(F("[MACA] CTS write: "));
  Serial.println(ok ? F("OK") : F("FALHOU"));
  modoRX();
}

// ─── Envia ACK para o Gateway ─────────────────────────────────────────────────
void enviarACK(uint8_t seq) {
  Payload ack;
  ack.seq      = seq;
  ack.origem   = ID_P7;
  ack.destino  = ID_GATEWAY;
  ack.tipo     = MSG_ACK;
  ack.dado1    = 0;
  ack.dado2    = 0;
  ack.dado3    = 0;
  ack.checksum = calcChecksum(ack);

  modoTX(ADDR_GATEWAY);
  bool ok = radio.write(&ack, sizeof(ack));
  Serial.print(F("[MACA] ACK write: "));
  Serial.println(ok ? F("OK") : F("FALHOU"));
  modoRX();
}

// ─── Aciona Alarme ────────────────────────────────────────────────────────────
void acionarAlarme() {
  alarmeAtivo = true;
  Serial.println(F("[ALARME] ACIONADO!"));
  // Pisca 3x rápido como feedback imediato
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED1_PIN, HIGH);
    digitalWrite(LED2_PIN, HIGH);
    delay(100);
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);
    delay(100);
  }
}

// ─── Desativa Alarme ──────────────────────────────────────────────────────────
void desativarAlarme() {
  alarmeAtivo = false;
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  estadoLED = false;
  Serial.println(F("[ALARME] Desarmado."));
}

// ─── Pisca LEDs enquanto alarme ativo (non-blocking) ─────────────────────────
void atualizarLEDs() {
  if (!alarmeAtivo) return;
  if (millis() - ultimoPisca >= INTERVALO_PISCA_MS) {
    ultimoPisca = millis();
    estadoLED = !estadoLED;
    digitalWrite(LED1_PIN, estadoLED ? HIGH : LOW);
    digitalWrite(LED2_PIN, estadoLED ? LOW  : HIGH);
  }
}

// ─── Recepção MACA ────────────────────────────────────────────────────────────
void verificarMensagensRF() {
  if (!radio.available()) return;

  Payload pkt;
  radio.read(&pkt, sizeof(pkt));

  Serial.print(F("[RF] tipo=0x"));
  Serial.print(pkt.tipo, HEX);
  Serial.print(F(" origem="));
  Serial.print(pkt.origem);
  Serial.print(F(" dest="));
  Serial.print(pkt.destino);
  Serial.print(F(" seq="));
  Serial.print(pkt.seq);
  Serial.print(F(" chk="));
  Serial.println(checksumOk(pkt) ? F("OK") : F("ERRO"));

  if (!checksumOk(pkt)) return;
  if (pkt.destino != ID_P7) return;

  if (pkt.tipo == MSG_RTS && pkt.origem == ID_GATEWAY) {
    Serial.println(F("[MACA] RTS recebido do Gateway"));
    enviarCTS(pkt.seq);

    // Aguarda DATA — tolerante ao seq
    unsigned long t = millis();
    while (millis() - t < TIMEOUT_DATA_MS) {
      if (radio.available()) {
        Payload data;
        radio.read(&data, sizeof(data));

        Serial.print(F("[RF] Aguardando DATA tipo=0x"));
        Serial.print(data.tipo, HEX);
        Serial.print(F(" seq="));
        Serial.println(data.seq);

        if (!checksumOk(data)) {
          Serial.println(F("[RF] Checksum invalido"));
          continue;
        }

        // Aceita DATA do Gateway independente do seq
        if (data.tipo    != MSG_RTS    &&
            data.origem  == ID_GATEWAY &&
            data.destino == ID_P7) {
          Serial.print(F("[MACA] DATA recebido! tipo=0x"));
          Serial.println(data.tipo, HEX);
          enviarACK(data.seq);
          if (data.tipo == MSG_ALARM)  acionarAlarme();
          if (data.tipo == MSG_DISARM) desativarAlarme();
          break;
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);

  radio.begin();
  radio.setPALevel(RF24_PA_LOW);
  radio.setChannel(CANAL_RF);
  radio.setAutoAck(false);
  radio.setDataRate(RF24_250KBPS);
  radio.setRetries(0, 0);
  radio.setPayloadSize(sizeof(Payload));
  modoRX();

  // Pisca os dois LEDs 1x para indicar inicialização
  digitalWrite(LED1_PIN, HIGH);
  digitalWrite(LED2_PIN, HIGH);
  delay(500);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);

  Serial.println(F("=== PLACA 7 - ALARME PRONTO ==="));
  Serial.print(F("Radio conectado: "));
  Serial.println(radio.isChipConnected() ? F("SIM") : F("NAO"));
  Serial.print(F("Canal RF: "));
  Serial.println(CANAL_RF);
}

void loop() {
  verificarMensagensRF();
  atualizarLEDs();
  delay(10);
}
