/*
 * PLACA 30 - NÓ SENSOR
 * Hardware: Arduino Nano + nRF24L01 + HC-SR04
 * Protocolo MACA Four-Way Handshake: RTS → CTS → DATA → ACK
 */

#include <SPI.h>
#include "RF24.h"

// ─── Pinos ────────────────────────────────────────────────────────────────────
#define CE_PIN       7
#define CSN_PIN      8
#define TRIGGER_PIN  3
#define ECHO_PIN     4

// ─── Identificadores ──────────────────────────────────────────────────────────
#define ID_PLACA   30
#define ID_GATEWAY 13
#define CANAL_RF   12

// ─── Threshold do sensor ──────────────────────────────────────────────────────
#define DIST_THRESHOLD_CM 20  // acima de 20cm = porta aberta

const uint64_t ADDR_P30     = 0x3030303030LL;
const uint64_t ADDR_GATEWAY = 0x3030303030LL;

// ─── Tipos de Mensagem ────────────────────────────────────────────────────────
#define MSG_RTS    0x01
#define MSG_CTS    0x02
#define MSG_DATA   0x03
#define MSG_ACK    0x04

// ─── Payload (8 bytes) ────────────────────────────────────────────────────────
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

// ─── Timeouts ─────────────────────────────────────────────────────────────────
#define TIMEOUT_CTS_MS  800
#define TIMEOUT_ACK_MS  800
#define MAX_RETRIES     5
#define DELAY_TX_MS     20
#define DELAY_RX_MS     20

RF24 radio(CE_PIN, CSN_PIN);
uint8_t seqNum = 0;
bool portaAbertaAntes = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────
uint8_t calcChecksum(Payload &p) {
  return p.seq ^ p.origem ^ p.destino ^ p.tipo ^ p.dado1 ^ p.dado2 ^ p.dado3;
}

bool checksumOk(Payload &p) {
  return p.checksum == calcChecksum(p);
}

void modoTX() {
  radio.stopListening();
  delay(DELAY_TX_MS);
  radio.openWritingPipe(ADDR_GATEWAY);
}

void modoRX() {
  radio.openReadingPipe(1, ADDR_P30);
  radio.startListening();
  delay(DELAY_RX_MS);
}

// ─── Carrier Sense ────────────────────────────────────────────────────────────
void esperarCanalLivre() {
  radio.openReadingPipe(1, ADDR_P30);
  radio.startListening();
  delay(1);
  while (radio.testCarrier()) {
    delay(random(5, 20));
  }
  radio.stopListening();
  delay(5);
}

// ─── Leitura do Sensor ────────────────────────────────────────────────────────
float lerDistanciaCM() {
  digitalWrite(TRIGGER_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIGGER_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIGGER_PIN, LOW);
  long duracao = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duracao == 0) return 0;
  return (duracao * 0.0343) / 2.0;
}

// ─── MACA Four-Way Handshake ──────────────────────────────────────────────────
bool enviarComMACA(uint8_t tipo, uint8_t d1, uint8_t d2, uint8_t d3) {
  for (int tentativa = 0; tentativa < MAX_RETRIES; tentativa++) {
    seqNum++;

    // PASSO 1: RTS
    esperarCanalLivre();

    Payload rts;
    rts.seq      = seqNum;
    rts.origem   = ID_PLACA;
    rts.destino  = ID_GATEWAY;
    rts.tipo     = MSG_RTS;
    rts.dado1    = tipo;
    rts.dado2    = 0;
    rts.dado3    = 0;
    rts.checksum = calcChecksum(rts);

    modoTX();
    radio.write(&rts, sizeof(rts));
    Serial.print(F("[MACA] RTS enviado seq="));
    Serial.println(seqNum);

    // PASSO 2: Aguarda CTS
    modoRX();
    unsigned long t0 = millis();
    bool cts_ok = false;
    while (millis() - t0 < TIMEOUT_CTS_MS) {
      if (radio.available()) {
        Payload resp;
        radio.read(&resp, sizeof(resp));
        Serial.print(F("[DBG] tipo=0x"));
        Serial.print(resp.tipo, HEX);
        Serial.print(F(" orig="));
        Serial.print(resp.origem);
        Serial.print(F(" dest="));
        Serial.print(resp.destino);
        Serial.print(F(" seq="));
        Serial.print(resp.seq);
        Serial.print(F(" chk="));
        Serial.println(checksumOk(resp) ? F("OK") : F("ERRO"));

        if (resp.tipo    == MSG_CTS    &&
            resp.origem  == ID_GATEWAY &&
            resp.destino == ID_PLACA   &&
            resp.seq     == seqNum     &&
            checksumOk(resp)) {
          cts_ok = true;
          Serial.println(F("[MACA] CTS recebido!"));
          break;
        }
      }
    }

    if (!cts_ok) {
      Serial.print(F("[MACA] Sem CTS, tentativa "));
      Serial.println(tentativa + 1);
      delay(random(20, 80) * (tentativa + 1));
      continue;
    }

    // PASSO 3: DATA (sem carrier sense — canal reservado pelo RTS/CTS)
    Payload data;
    data.seq      = seqNum;
    data.origem   = ID_PLACA;
    data.destino  = ID_GATEWAY;
    data.tipo     = tipo;
    data.dado1    = d1;
    data.dado2    = d2;
    data.dado3    = d3;
    data.checksum = calcChecksum(data);

    modoTX();
    radio.write(&data, sizeof(data));
    Serial.println(F("[MACA] DATA enviado"));

    // PASSO 4: Aguarda ACK
    modoRX();
    unsigned long t1 = millis();
    while (millis() - t1 < TIMEOUT_ACK_MS) {
      if (radio.available()) {
        Payload ack;
        radio.read(&ack, sizeof(ack));
        if (ack.tipo    == MSG_ACK    &&
            ack.origem  == ID_GATEWAY &&
            ack.destino == ID_PLACA   &&
            ack.seq     == seqNum     &&
            checksumOk(ack)) {
          Serial.println(F("[MACA] ACK recebido! Entrega confirmada."));
          return true;
        }
      }
    }

    Serial.print(F("[MACA] Sem ACK, tentativa "));
    Serial.println(tentativa + 1);
    delay(random(20, 80) * (tentativa + 1));
  }

  Serial.println(F("[MACA] FALHA: maximo de tentativas atingido."));
  return false;
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  radio.begin();
  radio.setPALevel(RF24_PA_HIGH);
  radio.setChannel(CANAL_RF);
  radio.setAutoAck(false);
  radio.setDataRate(RF24_250KBPS);
  radio.setRetries(0, 0);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.setPayloadSize(sizeof(Payload));
  modoRX();

  Serial.println(F("=== PLACA 30 - SENSOR DE PORTA ==="));
  Serial.print(F("Radio conectado: "));
  Serial.println(radio.isChipConnected() ? F("SIM") : F("NAO"));
  Serial.print(F("Canal RF: "));
  Serial.println(CANAL_RF);
  Serial.print(F("Threshold: "));
  Serial.print(DIST_THRESHOLD_CM);
  Serial.println(F(" cm"));
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  float distancia = lerDistanciaCM();
  bool portaAberta = (distancia > DIST_THRESHOLD_CM);

  if (portaAberta && !portaAbertaAntes) {
    Serial.print(F("[SENSOR] Porta ABERTA! Dist: "));
    Serial.print(distancia);
    Serial.println(F(" cm"));
    uint8_t dist_cm = (uint8_t)min((int)distancia, 255);
    enviarComMACA(MSG_DATA, dist_cm, 1, 0);
    delay(5000); // aguarda antes de ler sensor de novo
  }
  else if (!portaAberta && portaAbertaAntes) {
    Serial.print(F("[SENSOR] Porta FECHADA. Dist: "));
    Serial.print(distancia);
    Serial.println(F(" cm"));
    uint8_t dist_cm = (uint8_t)min((int)distancia, 255);
    enviarComMACA(MSG_DATA, dist_cm, 0, 0);
    delay(2000);
  }

  portaAbertaAntes = portaAberta;
  delay(200);
}