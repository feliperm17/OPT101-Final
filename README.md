# 🔐 GuardaPorta IoT

Sistema de alarme de porta com detecção por sensor ultrassônico, protocolo de comunicação sem fio MACA com four-way handshake e painel de controle web para smartphone.

Desenvolvido como trabalho prático da disciplina de IoT utilizando Arduino Nano e módulos de rádio nRF24L01.

---

## 📡 Arquitetura

```
[Placa 30 - Sensor]         [Placa 7 - Alarme]
  HC-SR04 (porta)             LED verde + vermelho
        |                            |
        |     RF 2.4GHz (MACA)       |
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
           [App Web no Smartphone]
            http://<IP>:5000/app
```

### Fluxo da aplicação

```
1. Sensor detecta porta aberta (distância > 20cm)
2. Placa 30 envia alerta ao Gateway via MACA
3. Gateway repassa ao backend via Serial
4. Backend notifica o app web via WebSocket
5. Usuário tem 10 segundos para digitar a senha
   ├── Senha CORRETA → alarme não dispara, sistema OK
   └── Senha ERRADA ou timeout → backend envia ALARME ao Gateway
                                  → Gateway aciona Placa 7 via MACA
                                  → LEDs piscam em alternância
```

---

## 📡 Protocolo MACA com Four-Way Handshake

Cada transmissão segue obrigatoriamente a sequência:

```
Emissor                    Receptor
   |                           |
   |── Carrier Sense ─────────>|  escuta canal, espera silêncio
   |──── RTS (seq) ───────────>|  Request to Send
   |<─── CTS (seq) ────────────|  Clear to Send
   |──── DATA (seq + payload) >|  Dados reais
   |<─── ACK (seq) ────────────|  Acknowledgment
```

Em caso de falha, o emissor faz backoff exponencial e tenta novamente (até 5 tentativas).

---

## 🔧 Hardware necessário

| Componente      | Quantidade |
|-----------------|-----------|
| Arduino Nano    | 3         |
| Módulo nRF24L01 | 3         |
| Sensor HC-SR04  | 1         |
| LED vermelho    | 1         |
| LED verde       | 1         |
| Resistor 220Ω   | 2         |
| Cabos jumper    | vários    |

### Pinagem — Placa 30 (Sensor)

```
Arduino Nano    nRF24L01        Arduino Nano    HC-SR04
D7          →   CE              D3          →   TRIG
D8          →   CSN             D4          →   ECHO
D13         →   SCK             5V          →   VCC
D11         →   MOSI            GND         →   GND
D12         →   MISO
3.3V        →   VCC
GND         →   GND
```

### Pinagem — Placa 13 (Gateway)

```
Arduino Nano    nRF24L01
D7          →   CE
D8          →   CSN
D13         →   SCK
D11         →   MOSI
D12         →   MISO
3.3V        →   VCC
GND         →   GND
```

### Pinagem — Placa 7 (Alarme)

```
Arduino Nano    nRF24L01        Arduino Nano    LEDs
D7          →   CE              D5  → LED verde  → resistor 220Ω → GND
D8          →   CSN             D6  → LED vermelho → resistor 220Ω → GND
D13         →   SCK
D11         →   MOSI
D12         →   MISO
3.3V        →   VCC
GND         →   GND
```

---

## 🗂 Estrutura do projeto

```
OPT101-Final/
├── arduino/
│   ├── placa30_sensor/
│   │   └── placa30_sensor.ino    # Placa 30: sensor de porta
│   ├── placa13_gateway/
│   │   └── placa13_gateway.ino   # Placa 13: gateway central
│   └── placa7_alarme/
│       └── placa7_alarme.ino     # Placa 7: LEDs de alarme
│
└── backend/
    ├── app.py                    # Servidor Flask principal
    ├── requirements.txt          # Dependências Python
    └── templates/
        └── index.html            # Frontend web responsivo
```

---

## 🚀 Como rodar

### 1. Pré-requisitos

- Python 3.8+
- Arduino IDE 1.8+ ou Arduino IDE 2
- Biblioteca RF24 instalada:
  - Arduino IDE → Tools → Manage Libraries → buscar **RF24** → instalar a de **TMRh20**

---

### 2. Gravar os códigos nas placas

Abra o Arduino IDE e grave cada sketch na placa correspondente:

| Sketch                     | Placa    | Processor                    |
|----------------------------|----------|------------------------------|
| `arduino/placa30_sensor/`  | Placa 30 | ATmega328P (Old Bootloader)  |
| `arduino/placa13_gateway/` | Placa 13 | ATmega328P (Old Bootloader)  |
| `arduino/placa7_alarme/`   | Placa 7  | ATmega328P (Old Bootloader)  |

> ⚠️ Feche o Monitor Serial do Arduino IDE antes de rodar o backend.

---

### 3. Rodar o backend

```bash
# Entra na pasta do backend
cd backend/

# Cria e ativa o ambiente virtual
python3 -m venv venv
source venv/bin/activate        # Linux/Mac
# venv\Scripts\activate         # Windows

# Instala as dependências
pip install -r requirements.txt

# Conecta a Placa 13 (Gateway) via USB e roda
python3 app.py
```

Saída esperada:
```
[APP] Backend iniciado em http://0.0.0.0:5000
[APP] Senha configurada: 1234
[APP] Timeout senha: 10s
[SERIAL] Conectado em /dev/ttyUSB0
[SERIAL←Arduino] === GATEWAY 13 PRONTO ===
```

---

### 4. Acessar o painel no smartphone

Descubra o IP do computador:
```bash
hostname -I   # Linux/Mac
ipconfig      # Windows
```

Abra no navegador do celular **(mesma rede Wi-Fi)**:
```
http://<SEU_IP>:5000/app
```

---

### 5. Testar sem hardware

O painel tem uma seção de simulação na parte inferior. Clique em **"▶ abrir porta"** para simular sem o sensor físico.

Ou via terminal:
```bash
# Simula porta abrindo
curl -X POST http://localhost:5000/api/simular_porta \
  -H "Content-Type: application/json" \
  -d '{"acao": "abrir"}'

# Simula porta fechando
curl -X POST http://localhost:5000/api/simular_porta \
  -H "Content-Type: application/json" \
  -d '{"acao": "fechar"}'
```

---

## 🔑 Senha padrão

```
1234
```

Para alterar, edite no `backend/app.py`:
```python
SENHA_CORRETA = "1234"   # ← mude aqui
```

---

## 🌐 API REST

| Método | Rota                   | Descrição                      |
|--------|------------------------|--------------------------------|
| GET    | `/api/estado`          | Estado atual do sistema        |
| POST   | `/api/senha`           | Enviar tentativa de senha      |
| POST   | `/api/desarmar`        | Desarmar alarme (requer senha) |
| GET    | `/api/eventos?limit=N` | Histórico de eventos           |
| POST   | `/api/simular_porta`   | Simular abertura/fechamento    |

---

## 📶 Configuração do canal RF

Canal padrão: **100** (2.500 GHz). Para mudar, altere o `#define` nas **três placas**:

```cpp
#define CANAL_RF  100  // mude nas 3 placas simultaneamente
```

Canais recomendados (evitam Wi-Fi 2.4GHz): `90`, `95`, `105`, `110`.

---

## 🛠 Solução de problemas

| Problema | Solução |
|---|---|
| Placa não aparece no PC | Instalar driver CH340 |
| `Permission denied` na serial | `sudo usermod -aG dialout $USER` + logout/login |
| Backend não encontra a porta | Fechar Monitor Serial do Arduino IDE |
| `ModuleNotFoundError: flask` | `rm -rf venv && python3 -m venv venv && pip install -r requirements.txt` |
| Placas não se comunicam | Verificar se nRF24 está em **3.3V** (não 5V) |
| Porta serial ocupada | `sudo fuser -k /dev/ttyUSB0` |
