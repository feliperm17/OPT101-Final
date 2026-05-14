# GuardaPorta IoT — Documentação Completa

## Visão Geral

Sistema de alarme de porta usando 3 Arduino Nano com rádio nRF24L01,
protocolo MACA com four-way handshake, e interface web para smartphone.

```
[Placa 30 - Sensor]    [Placa 7 - Alarme]
  HC-SR04 (porta)         LED1 + LED2
       |                       |
       |   RF 2.4GHz (MACA)    |
       └──────→ [Placa 13 - Gateway] ←──┘
                     |
               USB Serial
                     |
            [Backend Python]
             Flask + SQLite
             + Flask-SocketIO
                     |
              HTTP / WebSocket
                     |
           [App Web no Celular]
            http://<IP>:5000
```

---

## Protocolo MACA com Four-Way Handshake

Cada transmissão de dados segue obrigatoriamente a sequência:

```
Emissor                         Receptor
   |                               |
   |── Carrier Sense ─────────────>|  (escuta o canal, espera silêncio)
   |                               |
   |──── RTS (seq, origem, dest) ─>|  Request to Send
   |                               |
   |<─── CTS (seq, origem, dest) ──|  Clear to Send
   |                               |
   |── Carrier Sense ─────────────>|
   |                               |
   |──── DATA (seq + payload) ────>|  Dados reais
   |                               |
   |<─── ACK (seq, origem, dest) ──|  Acknowledgment
   |                               |
```

Se CTS ou ACK não chegarem dentro do timeout, o emissor faz backoff
exponencial randômico e tenta novamente (até MAX_RETRIES = 5).

---

## Estrutura do Payload (8 bytes)

| Byte | Campo    | Descrição                               |
|------|----------|-----------------------------------------|
| 0    | seq      | Número de sequência (0–255, ciclico)    |
| 1    | origem   | ID da placa emissora (7, 13 ou 30)      |
| 2    | destino  | ID da placa receptora                   |
| 3    | tipo     | Tipo da mensagem (ver tabela abaixo)    |
| 4    | dado1    | Dado 1 (distância em cm, ou flag)       |
| 5    | dado2    | Dado 2 (status 0=fechada 1=aberta)      |
| 6    | dado3    | Reservado / flags extras                |
| 7    | checksum | XOR dos bytes 0–6 (detecção de erro)   |

### Tipos de mensagem

| Constante   | Valor | Uso                            |
|-------------|-------|--------------------------------|
| MSG_RTS     | 0x01  | Request to Send                |
| MSG_CTS     | 0x02  | Clear to Send                  |
| MSG_DATA    | 0x03  | Dados do sensor de porta       |
| MSG_ACK     | 0x04  | Confirmação de recebimento     |
| MSG_ALARM   | 0x10  | Comando: acionar LEDs alarme   |
| MSG_DISARM  | 0x11  | Comando: desligar LEDs alarme  |

---

## Endereços de Pipe por Placa

| Placa         | ID | Endereço (64-bit)   |
|---------------|----|---------------------|
| Placa 30      | 30 | 0x3030303030LL      |
| Gateway 13    | 13 | 0x1313131313LL      |
| Alarme 7      |  7 | 0x0707070707LL      |

Cada placa abre o seu pipe de leitura no próprio endereço.
Ao transmitir, aponta o pipe de escrita para o endereço do destino.

---

## Fluxo da Aplicação

```
1. [Placa 30] Mede distância com HC-SR04 a cada 200ms
2. [Placa 30] Detecta porta aberta (dist > 20cm)
3. [Placa 30] → MACA 4-way → [Gateway 13]: DATA(dist, status=1)
4. [Gateway 13] → Serial USB → [Backend Python]: "PORTA_ABERTA:35"
5. [Backend] Inicia timer de 15 segundos
6. [Backend] → WebSocket → [App Web]: evento "porta_aberta"
7. [Usuário] Teclado numérico aparece no celular
   ├── Senha CORRETA → "PORTA_FECHADA" registrada, tudo ok
   └── Senha ERRADA ou timeout →
       7a. [Backend] → Serial USB → [Gateway 13]: "ALARME"
       7b. [Gateway 13] → MACA 4-way → [Placa 7]: MSG_ALARM
       7c. [Placa 7] Aciona LEDs em alternância (alarme visual)
8. [Admin] Pode desarmar via painel web com senha
```

---

## Montagem de Hardware

### Placa 30 — Sensor
```
Arduino Nano    HC-SR04
    3.3V  ──→   VCC
     GND  ──→   GND
      D3  ──→   TRIG
      D4  ──→   ECHO

Arduino Nano    nRF24L01
    3.3V  ──→   VCC
     GND  ──→   GND
      D7  ──→   CE
      D8  ──→   CSN
     D13  ──→   SCK
     D11  ──→   MOSI
     D12  ──→   MISO
```

### Placa 7 — Alarme
```
Arduino Nano    LEDs
      D5  ──→   LED1 (vermelho) → resistor 220Ω → GND
      D6  ──→   LED2 (amarelo)  → resistor 220Ω → GND
```

### Placa 13 — Gateway
```
Arduino Nano    nRF24L01  (mesmos pinos D7/D8/D11/D12/D13)
USB             Conectado ao PC/servidor (backend Python)
```

---

## Instalação do Backend

### 1. Instalar dependências Python
```bash
pip install flask flask-socketio pyserial
```

### 2. Ajustar a senha (app.py, linha ~25)
```python
SENHA_CORRETA  = "1234"     # ← mude aqui
TEMPO_SENHA_S  = 15         # ← tempo para digitar
```

### 3. Executar
```bash
cd backend/
python app.py
```

### 4. Descobrir o IP do servidor
```bash
# Linux/Mac
hostname -I

# Windows
ipconfig
```

### 5. Acessar no celular
Abra `http://<SEU_IP>:5000/app` no navegador do celular.
**O celular e o PC devem estar na mesma rede Wi-Fi.**

---

## Estrutura de Arquivos

```
projeto/
├── arduino/
│   ├── placa30_sensor.ino      ← Placa 30: HC-SR04 + MACA
│   ├── placa13_gateway.ino     ← Placa 13: Gateway + lógica
│   └── placa7_alarme.ino       ← Placa 7:  LEDs + MACA receptor
│
└── backend/
    ├── app.py                  ← Servidor Flask principal
    ├── alarme.db               ← SQLite (criado automaticamente)
    └── templates/
        └── index.html          ← Frontend web responsivo
```

---

## API REST do Backend

| Método | Rota                  | Descrição                        |
|--------|-----------------------|----------------------------------|
| GET    | /api/estado           | Estado atual do sistema          |
| POST   | /api/senha            | Enviar tentativa de senha        |
| POST   | /api/desarmar         | Desarmar alarme (requer senha)   |
| GET    | /api/eventos?limit=N  | Histórico de eventos (SQLite)    |
| POST   | /api/simular_porta    | Simular abertura/fechamento      |

### WebSocket Events (SocketIO)

| Evento              | Direção      | Dados                          |
|---------------------|--------------|--------------------------------|
| estado_atualizado   | Servidor→Web | Objeto com estado completo     |
| porta_aberta        | Servidor→Web | {distancia, tempo_limite}      |
| tempo_senha         | Servidor→Web | {restante} (segundos)          |
| senha_resultado     | Servidor→Web | {ok, msg}                      |
| serial_status       | Servidor→Web | {conectado, porta}             |

---

## Configuração do Canal RF

O canal está fixado em **100** (2.500 GHz) em todos os sketches:
```cpp
#define CANAL_RF  100
radio.setChannel(CANAL_RF);
```

Para as etapas de avaliação com interferências, mude este valor nos
três sketches simultaneamente. Evite canais 1–13 (Wi-Fi) e 36–40 (Bluetooth).
Boas opções: 90–120 (fora do Wi-Fi padrão).

Canais comuns do Wi-Fi 2.4GHz que conflitam:
- Canal 1:  2.412 GHz → RF canal  12
- Canal 6:  2.437 GHz → RF canal  37
- Canal 11: 2.462 GHz → RF canal  62

Ou seja, canal RF 100 = 2.500 GHz ✓ (não conflita com Wi-Fi padrão).

---

## Sobre a Avaliação

### Etapa 1 — Canal sem interferências
Configuração padrão, canal 100. Deve funcionar normalmente.

### Etapa 2 — Canal com outras equipes
Se houver colisão, o MACA cuida: carrier sense + backoff exponencial.
O four-way handshake garante entrega confiável.

### Etapa 3 — Canal com ruído
O checksum XOR detecta corrupção de pacotes.
Pacotes com checksum inválido são descartados e retransmitidos.
Aumente MAX_RETRIES se necessário.

### Etapa 4 — Canal Wi-Fi
Mude o canal para algo próximo do Wi-Fi (ex: 37) para demonstrar
degradação e resiliência. O backoff exponencial amortiza colisões.
