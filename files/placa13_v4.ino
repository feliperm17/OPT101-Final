/*
 * PLACA 13 - GATEWAY
 * Protocolo MACA Four-Way Handshake
 *
 * Correções aplicadas:
 * - Remove envio automático de alarme para P7 (decisão fica no backend)
 * - Flag processandoAlarme para ignorar P30 enquanto alarme em andamento
 * - Filtro de origem ao aguardar CTS e ACK da P7
 * - Seq mais tolerante para evitar necessidade de reset
 * - Delay antes do DATA para P7 ter tempo de voltar ao RX
 */

#include <SPI.h>
#include "RF24.h"

#define CE_PIN     7
#define CSN_PIN    8
#define ID_GATEWAY 13
#define ID_P30     30
#define ID_P7      7
#define CANAL_RF   100

const uint64_t ADDR_GATEWAY = 0x1313131313LL;
const uint64_t ADDR_P30     = 0x3030303030LL;
const uint64_t ADDR_P7      = 0x0707070707LL;

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
#define TIMEOUT_CTS_MS  1500
#define TIMEOUT_ACK_MS  1500
#define MAX_RETRIES     5
#define DELAY_TX_MS     20
#define DELAY_RX_MS     20

RF24 radio(CE_PIN, CSN_PIN);
uint8_t seqNum = 0;
bool processandoAlarme = false; // flag: ignora P30 enquanto alarme ativo

uint8_t calcChecksum(Payload &p) {
  return p.seq ^ p.origem ^ p.destino ^ p.tipo ^ p.dado1 ^ p.dado2 ^ p.dado3;
}

bool checksumOk(Payload &p) {
  return p.checksum == calcChecksum(p);
}

// ─── Transições de modo ───────────────────────────────────────────────────────
void modoRX() {
  radio.openReadingPipe(1, ADDR_GATEWAY);
  radio.startListening();
  delay(DELAY_RX_MS);
}

void modoTX(uint64_t destAddr) {
  radio.stopListening();
  delay(DELAY_TX_MS);
  radio.openWritingPipe(destAddr);
}

// ─── Envia CTS ────────────────────────────────────────────────────────────────
void enviarCTS(uint8_t destino_id, uint64_t destino_addr, uint8_t seq) {
  Payload cts;
  cts.seq      = seq;
  cts.origem   = ID_GATEWAY;
  cts.destino  = destino_id;
  cts.tipo     = MSG_CTS;
  cts.dado1    = 0;
  cts.dado2    = 0;
  cts.dado3    = 0;
  cts.checksum = calcChecksum(cts);

  modoTX(destino_addr);
  bool ok = radio.write(&cts, sizeof(cts));
  Serial.print(F("[MACA] CTS write: "));
  Serial.println(ok ? F("OK") : F("FALHOU"));
  modoRX();
}

// ─── Envia ACK ────────────────────────────────────────────────────────────────
void enviarACK(uint8_t destino_id, uint64_t destino_addr, uint8_t seq) {
  Payload ack;
  ack.seq      = seq;
  ack.origem   = ID_GATEWAY;
  ack.destino  = destino_id;
  ack.tipo     = MSG_ACK;
  ack.dado1    = 0;
  ack.dado2    = 0;
  ack.dado3    = 0;
  ack.checksum = calcChecksum(ack);

  modoTX(destino_addr);
  bool ok = radio.write(&ack, sizeof(ack));
  Serial.print(F("[MACA] ACK write: "));
  Serial.println(ok ? F("OK") : F("FALHOU"));
  modoRX();
}

// ─── Processa mensagem da Placa 30 ────────────────────────────────────────────
void processarMensagemP30(Payload &data) {
  if (data.dado2 == 1) {
    processandoAlarme = true;   // para de aceitar novos pacotes da P30
    Serial.print(F("PORTA_ABERTA:"));
    Serial.println(data.dado1);
    // NÃO encaminha para P7 aqui — backend decide quando acionar
  } else {
    processandoAlarme = false;  // porta fechou, volta a aceitar P30
    Serial.println(F("PORTA_FECHADA"));
  }
}

// ─── MACA Emissor: Gateway → Placa 7 ─────────────────────────────────────────
bool enviarParaP7(uint8_t tipo, uint8_t d1, uint8_t d2, uint8_t d3) {
  for (int tentativa = 0; tentativa < MAX_RETRIES; tentativa++) {
    seqNum++;

    // PASSO 1: RTS
    Payload rts;
    rts.seq      = seqNum;
    rts.origem   = ID_GATEWAY;
    rts.destino  = ID_P7;
    rts.tipo     = MSG_RTS;
    rts.dado1    = tipo;
    rts.dado2    = 0;
    rts.dado3    = 0;
    rts.checksum = calcChecksum(rts);

    modoTX(ADDR_P7);
    radio.write(&rts, sizeof(rts));
    Serial.print(F("[MACA->P7] RTS seq="));
    Serial.println(seqNum);

    // PASSO 2: Aguarda CTS da P7 — ignora pacotes de outras origens
    modoRX();
    unsigned long t0 = millis();
    bool cts_ok = false;
    while (millis() - t0 < TIMEOUT_CTS_MS) {
      if (radio.available()) {
        Payload resp;
        radio.read(&resp, sizeof(resp));

        if (resp.origem != ID_P7) {
          Serial.println(F("[DBG->P7] Ignorando pacote de outro no"));
          continue;
        }

        Serial.print(F("[DBG->P7] tipo=0x"));
        Serial.print(resp.tipo, HEX);
        Serial.print(F(" origem="));
        Serial.print(resp.origem);
        Serial.print(F(" seq="));
        Serial.println(resp.seq);

        // Tolerante ao seq — aceita CTS da P7 independente do numero
        if (resp.tipo    == MSG_CTS    &&
            resp.origem  == ID_P7      &&
            resp.destino == ID_GATEWAY &&
            checksumOk(resp)) {
          cts_ok = true;
          Serial.println(F("[MACA->P7] CTS recebido!"));
          break;
        }
      }
    }

    if (!cts_ok) {
      Serial.print(F("[MACA->P7] Sem CTS, tentativa "));
      Serial.println(tentativa + 1);
      delay(random(20, 80) * (tentativa + 1));
      continue;
    }

    // PASSO 3: DATA
    // Delay para P7 ter tempo de voltar ao modo RX após enviar CTS
    delay(50);

    Payload data;
    data.seq      = seqNum;
    data.origem   = ID_GATEWAY;
    data.destino  = ID_P7;
    data.tipo     = tipo;
    data.dado1    = d1;
    data.dado2    = d2;
    data.dado3    = d3;
    data.checksum = calcChecksum(data);

    modoTX(ADDR_P7);
    radio.write(&data, sizeof(data));
    Serial.println(F("[MACA->P7] DATA enviado"));

    // PASSO 4: Aguarda ACK da P7 — ignora pacotes de outras origens
    modoRX();
    unsigned long t1 = millis();
    while (millis() - t1 < TIMEOUT_ACK_MS) {
      if (radio.available()) {
        Payload ack;
        radio.read(&ack, sizeof(ack));

        if (ack.origem != ID_P7) continue;

        if (ack.tipo    == MSG_ACK    &&
            ack.origem  == ID_P7      &&
            ack.destino == ID_GATEWAY &&
            checksumOk(ack)) {
          Serial.println(F("[MACA->P7] ACK recebido!"));
          return true;
        }
      }
    }

    Serial.print(F("[MACA->P7] Sem ACK, tentativa "));
    Serial.println(tentativa + 1);
    delay(random(20, 80) * (tentativa + 1));
  }

  Serial.println(F("[MACA->P7] FALHA apos maximo de tentativas"));
  return false;
}

// ─── Recepção MACA (Gateway como receptor) ────────────────────────────────────
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
  if (pkt.destino != ID_GATEWAY) return;

  // Ignora novos pacotes da P30 enquanto alarme está sendo processado
  if (pkt.origem == ID_P30 && processandoAlarme) {
    Serial.println(F("[GW] Ignorando P30, alarme em andamento"));
    return;
  }

  if (pkt.tipo == MSG_RTS) {
    Serial.print(F("[MACA] RTS recebido de placa "));
    Serial.println(pkt.origem);

    uint64_t addr_origem = (pkt.origem == ID_P30) ? ADDR_P30 : ADDR_P7;
    enviarCTS(pkt.origem, addr_origem, pkt.seq);

    // Aguarda DATA do mesmo remetente com mesmo seq
    unsigned long t = millis();
    while (millis() - t < TIMEOUT_DATA_MS) {
      if (radio.available()) {
        Payload data;
        radio.read(&data, sizeof(data));

        Serial.print(F("[RF] Aguardando DATA tipo=0x"));
        Serial.print(data.tipo, HEX);
        Serial.print(F(" origem="));
        Serial.print(data.origem);
        Serial.print(F(" seq="));
        Serial.println(data.seq);

        if (!checksumOk(data)) {
          Serial.println(F("[RF] Checksum invalido"));
          continue;
        }
        if (data.tipo    != MSG_RTS    &&
            data.origem  == pkt.origem &&
            data.seq     == pkt.seq    &&
            data.destino == ID_GATEWAY) {
          Serial.println(F("[MACA] DATA recebido!"));
          uint64_t addr_orig = (data.origem == ID_P30) ? ADDR_P30 : ADDR_P7;
          enviarACK(data.origem, addr_orig, data.seq);
          if (data.origem == ID_P30) {
            processarMensagemP30(data);
          }
          break;
        }
      }
    }
  }
}

// ─── Comandos Serial do Backend ───────────────────────────────────────────────
void verificarComandosSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == F("ALARME")) {
    Serial.println(F("[CMD] Acionando alarme na Placa 7"));
    enviarParaP7(MSG_ALARM, 1, 0, 0);
  } else if (cmd == F("DESARMAR")) {
    processandoAlarme = false; // libera Gateway para aceitar P30 novamente
    Serial.println(F("[CMD] Desarmando alarme na Placa 7"));
    enviarParaP7(MSG_DISARM, 0, 0, 0);
  }
}

void setup() {
  Serial.begin(115200);

  radio.begin();
  radio.setPALevel(RF24_PA_LOW);
  radio.setChannel(CANAL_RF);
  radio.setAutoAck(false);
  radio.setDataRate(RF24_250KBPS);
  radio.setRetries(0, 0);
  radio.setPayloadSize(sizeof(Payload));
  modoRX();

  Serial.println(F("=== GATEWAY 13 PRONTO ==="));
  Serial.print(F("Radio conectado: "));
  Serial.println(radio.isChipConnected() ? F("SIM") : F("NAO"));
  Serial.print(F("Canal RF: "));
  Serial.println(CANAL_RF);
}

void loop() {
  verificarMensagensRF();
  verificarComandosSerial();
}
