"""
Backend IoT - Sistema de Alarme de Porta
Flask + Flask-SocketIO + SQLite + Serial (Gateway Arduino)

Instalar dependências:
    pip install flask flask-socketio pyserial

Executar:
    python3 app.py
"""

import os
import re
import threading
import time
import sqlite3
from datetime import datetime

import serial
import serial.tools.list_ports
from flask import Flask, jsonify, render_template, request
from flask_socketio import SocketIO, emit

# ─── Configurações ─────────────────────────────────────────────────────────────
SERIAL_BAUD    = 115200
SERIAL_TIMEOUT = 1
DB_PATH        = "alarme.db"
SENHA_CORRETA  = "1234"      # altere aqui
TEMPO_SENHA_S  = 10          # segundos para digitar a senha

# ─── Flask ────────────────────────────────────────────────────────────────────
app = Flask(__name__)
app.config["SECRET_KEY"] = "iot-alarme-secret"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# ─── Estado Global ────────────────────────────────────────────────────────────
estado = {
    "porta_aberta":      False,
    "alarme_ativo":      False,
    "aguardando_senha":  False,
    "tempo_senha_inicio": None,
    "ultima_distancia":  0,
    "serial_conectado":  False,
    "serial_porta":      None,
}
arduino_serial: serial.Serial | None = None
lock = threading.Lock()

# ─── Banco de Dados ───────────────────────────────────────────────────────────
def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS eventos (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            tipo      TEXT NOT NULL,
            detalhe   TEXT
        )
    """)
    c.execute("""
        CREATE TABLE IF NOT EXISTS tentativas_senha (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            sucesso   INTEGER NOT NULL
        )
    """)
    conn.commit()
    conn.close()

def log_evento(tipo: str, detalhe: str = ""):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute(
        "INSERT INTO eventos (timestamp, tipo, detalhe) VALUES (?, ?, ?)",
        (datetime.now().isoformat(), tipo, detalhe)
    )
    conn.commit()
    conn.close()

def log_senha(sucesso: bool):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute(
        "INSERT INTO tentativas_senha (timestamp, sucesso) VALUES (?, ?)",
        (datetime.now().isoformat(), int(sucesso))
    )
    conn.commit()
    conn.close()

def get_eventos(limit=50):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute(
        "SELECT timestamp, tipo, detalhe FROM eventos ORDER BY id DESC LIMIT ?",
        (limit,)
    )
    rows = [{"timestamp": r[0], "tipo": r[1], "detalhe": r[2]} for r in c.fetchall()]
    conn.close()
    return rows

# ─── Serial ───────────────────────────────────────────────────────────────────
def encontrar_arduino():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        if "arduino" in desc or "ch340" in desc or "cp210" in desc or "usb" in desc:
            return p.device
    if ports:
        return ports[0].device
    return None

def enviar_comando_serial(cmd: str):
    global arduino_serial
    if arduino_serial and arduino_serial.is_open:
        try:
            arduino_serial.write((cmd + "\n").encode())
            print(f"[SERIAL→Arduino] {cmd}")
        except Exception as e:
            print(f"[SERIAL] Erro ao enviar: {e}")

# ─── Lógica de Alarme ─────────────────────────────────────────────────────────
def acionar_alarme():
    with lock:
        estado["alarme_ativo"]     = True
        estado["aguardando_senha"] = False
    enviar_comando_serial("ALARME")
    log_evento("ALARME_ACIONADO")
    socketio.emit("estado_atualizado", get_estado_publico())
    print("[APP] 🚨 ALARME ACIONADO!")

def desarmar_alarme():
    with lock:
        estado["alarme_ativo"]     = False
        estado["aguardando_senha"] = False
        estado["porta_aberta"]     = False
    enviar_comando_serial("DESARMAR")
    log_evento("ALARME_DESARMADO")
    socketio.emit("estado_atualizado", get_estado_publico())
    print("[APP] ✅ Alarme desarmado.")

def thread_timeout_senha():
    """Dispara alarme se senha não for inserida a tempo."""
    inicio = estado["tempo_senha_inicio"]
    while True:
        time.sleep(0.5)
        with lock:
            if not estado["aguardando_senha"]:
                return
            elapsed  = time.time() - inicio
            restante = TEMPO_SENHA_S - elapsed

        socketio.emit("tempo_senha", {"restante": max(0, int(restante))})

        if restante <= 0:
            print("[APP] ⏰ Tempo esgotado! Acionando alarme.")
            log_evento("TIMEOUT_SENHA", f"Sem senha em {TEMPO_SENHA_S}s")
            acionar_alarme()
            return

def processar_mensagem_arduino(linha: str):
    linha = linha.strip()
    if not linha:
        return

    print(f"[SERIAL←Arduino] {linha}")

    if linha.startswith("PORTA_ABERTA:"):
        m = re.match(r"PORTA_ABERTA:(\d+)", linha)
        distancia = int(m.group(1)) if m else 0

        with lock:
            ja_aberta = estado["porta_aberta"]
            ja_aguardando = estado["aguardando_senha"]
            ja_alarme = estado["alarme_ativo"]

        if not ja_aberta:
            with lock:
                estado["porta_aberta"]    = True
                estado["ultima_distancia"] = distancia

            # Só inicia contagem se ainda não está em alarme ou aguardando senha
            if not ja_alarme and not ja_aguardando:
                with lock:
                    estado["aguardando_senha"]   = True
                    estado["tempo_senha_inicio"] = time.time()
                t = threading.Thread(target=thread_timeout_senha, daemon=True)
                t.start()

            log_evento("PORTA_ABERTA", f"distancia={distancia}cm")
            socketio.emit("estado_atualizado", get_estado_publico())
            socketio.emit("porta_aberta", {
                "distancia":    distancia,
                "tempo_limite": TEMPO_SENHA_S
            })
            print(f"[APP] 🚪 Porta aberta! Distância: {distancia}cm")

    elif linha == "PORTA_FECHADA":
        with lock:
            estado["porta_aberta"] = False
        log_evento("PORTA_FECHADA")
        socketio.emit("estado_atualizado", get_estado_publico())
        print("[APP] 🚪 Porta fechada.")

def thread_serial_reader():
    global arduino_serial
    while True:
        porta = encontrar_arduino()
        if porta:
            try:
                arduino_serial = serial.Serial(porta, SERIAL_BAUD, timeout=SERIAL_TIMEOUT)
                with lock:
                    estado["serial_conectado"] = True
                    estado["serial_porta"]     = porta
                print(f"[SERIAL] Conectado em {porta}")
                socketio.emit("serial_status", {"conectado": True, "porta": porta})

                while True:
                    try:
                        if arduino_serial.in_waiting:
                            linha = arduino_serial.readline().decode("utf-8", errors="ignore")
                            processar_mensagem_arduino(linha)
                        time.sleep(0.05)
                    except serial.SerialException:
                        break
            except Exception as e:
                print(f"[SERIAL] Erro: {e}")
            finally:
                with lock:
                    estado["serial_conectado"] = False
                    estado["serial_porta"]     = None
                if arduino_serial:
                    try:
                        arduino_serial.close()
                    except:
                        pass
                arduino_serial = None
                socketio.emit("serial_status", {"conectado": False, "porta": None})
                print("[SERIAL] Desconectado. Tentando reconectar em 3s...")
        time.sleep(3)

# ─── Helpers ──────────────────────────────────────────────────────────────────
def get_estado_publico():
    with lock:
        return {
            "porta_aberta":     estado["porta_aberta"],
            "alarme_ativo":     estado["alarme_ativo"],
            "aguardando_senha": estado["aguardando_senha"],
            "ultima_distancia": estado["ultima_distancia"],
            "serial_conectado": estado["serial_conectado"],
            "serial_porta":     estado["serial_porta"],
        }

# ─── Rotas REST ───────────────────────────────────────────────────────────────
@app.route("/api/estado")
def api_estado():
    return jsonify(get_estado_publico())

@app.route("/api/senha", methods=["POST"])
def api_senha():
    data = request.get_json(force=True)
    senha_digitada = str(data.get("senha", "")).strip()

    with lock:
        aguardando = estado["aguardando_senha"]

    if not aguardando:
        return jsonify({"ok": False, "msg": "Não está aguardando senha."}), 400

    sucesso = (senha_digitada == SENHA_CORRETA)
    log_senha(sucesso)

    if sucesso:
        log_evento("SENHA_CORRETA")
        desarmar_alarme()   # desativa flag, envia DESARMAR para Arduino
        socketio.emit("senha_resultado", {"ok": True, "msg": "Senha correta! Sistema ok."})
        print("[APP] ✅ Senha correta.")
        return jsonify({"ok": True, "msg": "Senha correta!"})
    else:
        log_evento("SENHA_ERRADA")
        socketio.emit("senha_resultado", {"ok": False, "msg": "Senha incorreta! Alarme acionado."})
        print("[APP] ❌ Senha errada! Acionando alarme.")
        threading.Thread(target=acionar_alarme, daemon=True).start()
        return jsonify({"ok": False, "msg": "Senha incorreta! Alarme acionado."})

@app.route("/api/desarmar", methods=["POST"])
def api_desarmar():
    data = request.get_json(force=True)
    senha_admin = str(data.get("senha", "")).strip()
    if senha_admin != SENHA_CORRETA:
        return jsonify({"ok": False, "msg": "Senha incorreta."}), 403
    desarmar_alarme()
    return jsonify({"ok": True, "msg": "Alarme desarmado."})

@app.route("/api/eventos")
def api_eventos():
    limit = int(request.args.get("limit", 50))
    return jsonify(get_eventos(limit))

@app.route("/api/simular_porta", methods=["POST"])
def api_simular():
    data = request.get_json(force=True)
    acao = data.get("acao", "abrir")
    if acao == "abrir":
        processar_mensagem_arduino("PORTA_ABERTA:35")
    else:
        processar_mensagem_arduino("PORTA_FECHADA")
    return jsonify({"ok": True})

# ─── WebSocket ────────────────────────────────────────────────────────────────
@socketio.on("connect")
def on_connect():
    emit("estado_atualizado", get_estado_publico())
    emit("serial_status", {
        "conectado": estado["serial_conectado"],
        "porta":     estado["serial_porta"]
    })

# ─── Rotas HTML ───────────────────────────────────────────────────────────────
@app.route("/app")
def app_page():
    return render_template("index.html")

@app.route("/")
def index():
    return jsonify({
        "status": "online",
        "app":    "/app",
        "api":    "/api/estado"
    })

# ─── Main ─────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    init_db()
    t = threading.Thread(target=thread_serial_reader, daemon=True)
    t.start()
    print("[APP] Backend iniciado em http://0.0.0.0:5000")
    print(f"[APP] Senha configurada: {SENHA_CORRETA}")
    print(f"[APP] Timeout senha: {TEMPO_SENHA_S}s")
    socketio.run(app, host="0.0.0.0", port=5000, debug=False, allow_unsafe_werkzeug=True)
