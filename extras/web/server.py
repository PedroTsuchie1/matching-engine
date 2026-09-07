#!/usr/bin/env python3
"""Optional local web adapter; the existing C++ core executes every order."""

from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit
import uuid


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MAX_VALUE = 10**12
MAX_BODY = 16 * 1024
HISTORY_LIMIT = 300
ERRORS = {
    "invalid_price": "Informe um preço positivo com até duas casas decimais.",
    "invalid_quantity": "Informe uma quantidade inteira positiva.",
    "invalid_id": "Identificador de ordem inválido.",
    "invalid_side": "Escolha compra ou venda.",
    "unsupported_peg": "Use peg bid para compra ou peg offer para venda.",
    "reference_unavailable": "Não há uma ordem limit de referência para esta peg.",
    "order_not_found": "Ordem não encontrada.",
    "order_not_open": "Esta ordem já foi preenchida ou cancelada.",
    "unsupported_amendment": "O preço de uma peg é automático; altere apenas a quantidade.",
    "empty_amendment": "Informe um novo preço, uma nova quantidade ou ambos.",
    "session_limit": "Limite de 10.000 ordens atingido. Inicie uma nova sessão.",
    "invalid_command": "Comando inválido.",
    "bridge_error": "A engine não conseguiu processar o comando.",
}


class BridgeUnavailable(RuntimeError):
    """A failed bridge is reported, never silently replaced with an empty book."""


def build_bridge() -> Path:
    """Build only this new executable; original source/build files are untouched."""
    compiler = shutil.which("g++")
    if compiler is None:
        raise RuntimeError("G++ não encontrado. Instale build-essential (GCC com C++20).")
    sources = [HERE / "bridge.cpp"] + [ROOT / "src" / name for name in (
        "order.cpp", "order_book.cpp", "matching_engine.cpp"
    )]
    dependencies = sources + list((ROOT / "include" / "matching_engine").glob("*.hpp"))
    directory = HERE / ".build"
    directory.mkdir(exist_ok=True)
    binary = directory / "bridge"
    if binary.exists() and all(path.stat().st_mtime_ns <= binary.stat().st_mtime_ns
                               for path in dependencies):
        return binary
    print("Compilando o adaptador C++…", flush=True)
    with tempfile.TemporaryDirectory(prefix="compile-", dir=directory) as temporary:
        target = Path(temporary) / "bridge"
        subprocess.run([
            compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic",
            "-I", str(ROOT / "include"), *map(str, sources), "-o", str(target),
        ], check=True)
        target.replace(binary)
    return binary


class Bridge:
    """One C++ process; caller serializes request/response exchanges."""

    def __init__(self, binary: Path):
        self.process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1,
        )
        self.responses: queue.Queue[str | None] = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self) -> None:
        try:
            for line in self.process.stdout:
                self.responses.put(line)
        finally:
            self.responses.put(None)

    def request(self, command: str) -> dict:
        if self.process.poll() is not None:
            raise BridgeUnavailable("Engine encerrada. Inicie uma nova sessão para continuar.")
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
            line = self.responses.get(timeout=10)
            if line is None:
                raise BridgeUnavailable("A engine encerrou durante a operação.")
            return json.loads(line)
        except (BrokenPipeError, OSError, queue.Empty, json.JSONDecodeError) as error:
            self.close()
            raise BridgeUnavailable("A engine ficou indisponível. Reinicie a sessão.") from error

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=1)
        self.process.stdin.close()
        self.process.stdout.close()


def integer(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{1,13}", value):
        raise ValueError(f"{label}: informe um inteiro positivo, sem separadores.")
    parsed = int(value)
    if not 0 < parsed <= MAX_VALUE:
        raise ValueError(f"{label}: o limite por ordem é {MAX_VALUE:,}.".replace(",", "."))
    return str(parsed)


def price_cents(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{1,13}(?:\.[0-9]{1,2})?", value):
        raise ValueError("Preço: use um valor positivo com ponto e até duas casas decimais.")
    whole, _, fraction = value.partition(".")
    cents = int(whole) * 100 + int(fraction.ljust(2, "0"))
    if not 0 < cents <= MAX_VALUE:
        raise ValueError("Preço fora do intervalo: de 0.01 até 10000000000.00.")
    return str(cents)


def command_line(payload: dict) -> str:
    action = payload.get("action")
    fields = {
        "limit": {"action", "side", "price", "quantity", "confirmation"},
        "market": {"action", "side", "quantity"},
        "peg": {"action", "side", "reference", "quantity"},
        "cancel": {"action", "id"},
        "amend": {"action", "id", "price", "quantity"},
    }
    if not isinstance(action, str) or action not in fields:
        raise ValueError("Comando desconhecido.")
    if set(payload) - fields[action]:
        raise ValueError("O comando contém campos desconhecidos.")
    if action in ("cancel", "amend"):
        order_id = integer(payload.get("id"), "ID")
        if action == "cancel":
            return f"cancel {order_id}"
        price = price_cents(payload["price"]) if "price" in payload else "-"
        quantity = integer(payload["quantity"], "Quantidade") if "quantity" in payload else "-"
        return f"amend {order_id} {price} {quantity}"
    side = payload.get("side")
    if side not in ("buy", "sell"):
        raise ValueError("Escolha compra ou venda.")
    quantity = integer(payload.get("quantity"), "Quantidade")
    if action == "limit":
        return f"limit {side} {price_cents(payload.get('price'))} {quantity}"
    if action == "market":
        return f"market {side} {quantity}"
    reference = payload.get("reference")
    if reference not in ("bid", "offer"):
        raise ValueError("Referência de peg inválida.")
    return f"peg {reference} {side} {quantity}"


def format_price(cents: str) -> str:
    value = int(cents)
    return f"{value // 100}.{value % 100:02d}"


class Session:
    """Web history and transport state, without any matching implementation."""

    def __init__(self, binary: Path):
        self.binary = binary
        self.lock = threading.RLock()
        self.bridge: Bridge | None = None
        self.revision = 0
        self._reset()

    def _reset(self) -> None:
        bridge = Bridge(self.binary)
        try:
            initial = bridge.request("state")
        except Exception:
            bridge.close()
            raise
        if self.bridge:
            self.bridge.close()
        self.bridge = bridge
        self.current = initial
        self.session_id = uuid.uuid4().hex
        self.events: deque[dict] = deque(maxlen=HISTORY_LIMIT)
        self.trades: deque[dict] = deque(maxlen=HISTORY_LIMIT)
        self.trade_count = 0
        self.executed_quantity = 0
        self.next_event_id = 1
        self.revision += 1
        self._event("system", "Sessão iniciada. O livro está vazio.")

    def _event(self, kind: str, message: str, order_id: str | None = None) -> None:
        event = {
            "id": self.next_event_id,
            "time": datetime.now(timezone.utc).isoformat(),
            "kind": kind, "message": message,
        }
        if order_id is not None:
            event["order_id"] = order_id
        self.next_event_id += 1
        self.events.appendleft(event)

    def state(self, *, ok: bool = True, error: str | None = None) -> dict:
        with self.lock:
            orders = self.current["orders"]
            state = {
                "ok": ok, "revision": self.revision, "session_id": self.session_id,
                "book": self.current["book"], "orders": orders,
                "trades": list(self.trades), "events": list(self.events),
                "metrics": {
                    "submitted": len(orders),
                    "open_orders": sum(order["status"] == "active" for order in orders),
                    "trade_count": self.trade_count,
                    "executed_quantity": str(self.executed_quantity),
                    "cancelled_orders": sum(order["status"] == "cancelled" for order in orders),
                },
            }
            if error:
                state["error"] = error
            return state

    def reject(self, message: str, status: int = 400) -> tuple[int, dict]:
        with self.lock:
            self._event("error", message)
            self.revision += 1
            return status, self.state(ok=False, error=message)

    def read_state(self) -> tuple[int, dict]:
        with self.lock:
            if self.bridge.process.poll() is not None:
                return 503, self.state(ok=False, error=
                    "Engine encerrada. Reinicie a sessão para continuar; o último estado está sendo exibido.")
            return 200, self.state()

    def _execute(self, payload: dict) -> tuple[int, dict]:
        line = command_line(payload)  # Validate the complete request before any mutation.
        if self.bridge.process.poll() is not None:
            raise BridgeUnavailable("Engine encerrada. Inicie uma nova sessão para continuar.")
        if payload["action"] == "limit":
            confirmation = self._limit_confirmation(payload, line)
            if confirmation is not None:
                return confirmation
        previous = {order["id"]: order for order in self.current["orders"]}
        result = self.bridge.request(line)
        self.current = result
        self.revision += 1
        if not result["ok"]:
            message = ERRORS.get(result.get("error_code"), ERRORS["bridge_error"])
            self._event("error", message, payload.get("id"))
            return 400, self.state(ok=False, error=message)

        action = payload["action"]
        order_id = result.get("order_id")
        orders = {order["id"]: order for order in result["orders"]}
        if action in ("limit", "market", "peg"):
            order = orders[order_id]
            side = "Compra" if order["side"] == "buy" else "Venda"
            price = format_price(order["price"]) if order["price"] else "mercado"
            self._event("created", f"{side} {action.upper()} #{order_id}: "
                        f"{order['original_quantity']} @ {price}.", order_id)
        elif action == "amend":
            fields = []
            if "price" in payload:
                fields.append(f"preço {payload['price']}")
            if "quantity" in payload:
                fields.append(f"quantidade restante {payload['quantity']}")
            self._event("amended", f"Ordem #{order_id} alterada: {', '.join(fields)}.", order_id)

        for trade in result["trades"]:
            self.trade_count += 1
            self.executed_quantity += int(trade["quantity"])
            self.trades.appendleft({**trade, "id": self.trade_count,
                                    "time": datetime.now(timezone.utc).isoformat()})
            self._event("trade", f"Trade, price: {format_price(trade['price'])}, "
                        f"qty: {trade['quantity']} · #{trade['aggressive_order_id']} "
                        f"× #{trade['resting_order_id']}", trade["aggressive_order_id"])
        reasons = {
            "user_requested": "solicitação do usuário",
            "market_remainder": "saldo de ordem a mercado",
            "reference_unavailable": "referência da peg indisponível",
        }
        for cancellation in result["cancellations"]:
            cid = cancellation["order_id"]
            reason = reasons[cancellation["reason"]]
            self._event("cancelled", f"Ordem #{cid}: {cancellation['quantity']} "
                        f"canceladas ({reason}).", cid)
        for oid, order in orders.items():
            old = previous.get(oid)
            if (old and order["type"] == "peg" and order["status"] == "active"
                    and old["price"] != order["price"]):
                self._event("repriced", f"Peg #{oid}: {format_price(old['price'])} → "
                            f"{format_price(order['price'])}; prioridade preservada.", oid)
        return 200, self.state()

    def _limit_confirmation(self, payload: dict, line: str) -> tuple[int, dict] | None:
        """Check consent against this exact command and snapshot under self.lock.

        This only checks crossing; the C++ engine still performs all matching.
        A preview neither sends a command to the bridge nor changes the session.
        """
        supplied = payload.get("confirmation")
        if "confirmation" in payload and (
            not isinstance(supplied, dict)
            or set(supplied) != {"session_id", "revision", "command"}
            or not isinstance(supplied["session_id"], str)
            or type(supplied["revision"]) is not int
            or not isinstance(supplied["command"], str)
        ):
            raise ValueError("Confirmação inválida. Revise e envie a ordem novamente.")
        expected = {"session_id": self.session_id, "revision": self.revision,
                    "command": line}
        levels = self.current["book"]["sells" if payload["side"] == "buy" else "buys"]
        limit = int(price_cents(payload["price"]))
        best = int(levels[0]["price"]) if levels else None
        crosses = best is not None and (limit >= best if payload["side"] == "buy" else limit <= best)
        if crosses and supplied != expected:
            result = self.state(ok=False, error="Esta limit cruza o livro. Confirme antes de enviar.")
            result["confirmation_required"] = {
                "token": expected, "best_price": str(best), "limit_price": str(limit),
                "side": payload["side"], "quantity": payload["quantity"],
                "changed": supplied is not None,
            }
            return 409, result
        if supplied is not None and supplied != expected:
            # Never reuse consent after a mutation, even if the book no longer crosses.
            return 409, self.state(ok=False, error=
                "O livro ou a ordem mudou. Feche a confirmação e envie novamente após revisar.")
        return None

    def post(self, path: str, payload: dict, expected_session: str | None = None) -> tuple[int, dict]:
        with self.lock:
            try:
                if expected_session is not None and expected_session != self.session_id:
                    return self.reject("A sessão foi reiniciada em outra aba. Confira o novo livro antes de continuar.", 409)
                if path == "/api/command":
                    return self._execute(payload)
                if payload:
                    raise ValueError("Esta operação não recebe campos adicionais.")
                if path == "/api/reset":
                    self._reset()
                    return 200, self.state()
                if path == "/api/demo":
                    if self.current["orders"]:
                        message = "A demonstração requer uma sessão vazia. Reinicie a sessão primeiro."
                        self._event("error", message)
                        self.revision += 1
                        return 409, self.state(ok=False, error=message)
                    self._event("system", "Demonstração: ordens ilustrativas enviadas à engine real.")
                    for command in demo_commands():
                        status, state = self._execute(command)
                        if status != 200:
                            return status, state
                    return 200, self.state()
                return 404, {"ok": False, "error": "Rota não encontrada."}
            except ValueError as error:
                return self.reject(str(error))
            except BridgeUnavailable as error:
                return self.reject(str(error), 503)

    def close(self) -> None:
        with self.lock:
            self.bridge.close()


def demo_commands() -> list[dict]:
    commands = []
    bids = [(10000, 240), (9995, 360), (9990, 500), (9980, 650),
            (9970, 900), (9955, 1200), (9940, 850), (9920, 1600)]
    asks = [(10010, 180), (10015, 420), (10025, 600), (10040, 780),
            (10060, 1100), (10080, 950), (10100, 1300), (10125, 1500)]
    for side, levels in (("buy", bids), ("sell", asks)):
        for price, quantity in levels:
            commands.append({"action": "limit", "side": side,
                             "price": format_price(str(price)), "quantity": str(quantity)})
    commands += [
        {"action": "peg", "side": "buy", "reference": "bid", "quantity": "150"},
        {"action": "peg", "side": "sell", "reference": "offer", "quantity": "120"},
        {"action": "market", "side": "buy", "quantity": "40"},
        {"action": "market", "side": "sell", "quantity": "60"},
    ]
    return commands


class Handler(BaseHTTPRequestHandler):
    server_version = "MatchingDesk/1.0"

    def _local_request(self) -> bool:
        port = self.server.server_port
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if self.headers.get("Host") not in hosts:
            return False
        origin = self.headers.get("Origin")
        return (origin is None or origin in {f"http://{host}" for host in hosts}) and \
            self.headers.get("Sec-Fetch-Site") != "cross-site"

    def _send(self, status: int, data: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; "
                         "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                         "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, status: int, payload: dict) -> None:
        self._send(status, json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def do_GET(self) -> None:
        if not self._local_request():
            self._json(403, {"ok": False, "error": "Acesso permitido apenas pela interface local."})
            return
        path = urlsplit(self.path).path
        if path == "/api/state":
            status, state = self.server.session.read_state()
            self._json(status, state)
            return
        files = {
            "/": ("index.html", "text/html"),
            "/index.html": ("index.html", "text/html"),
            "/app.js": ("app.js", "text/javascript"),
            "/styles.css": ("styles.css", "text/css"),
        }
        if path in files:
            name, mime = files[path]
            target = HERE / "static" / name
            if target.is_file():
                self._send(200, target.read_bytes(), mime + "; charset=utf-8")
                return
        if path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
            return
        self._json(404, {"ok": False, "error": "Rota não encontrada."})

    def do_POST(self) -> None:
        if not self._local_request():
            self._json(403, {"ok": False, "error": "Origem não permitida."})
            return
        path = urlsplit(self.path).path
        if path not in ("/api/command", "/api/demo", "/api/reset"):
            self._json(404, {"ok": False, "error": "Rota não encontrada."})
            return
        if self.headers.get_content_type() != "application/json":
            self._json(415, {"ok": False, "error": "Use Content-Type: application/json."})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= MAX_BODY:
                self._json(413, {"ok": False, "error": "Corpo vazio ou maior que 16 KiB."})
                return
            payload = json.loads(self.rfile.read(size))
            if not isinstance(payload, dict):
                raise ValueError("O corpo deve ser um objeto JSON.")
        except (ValueError, UnicodeDecodeError):
            status, state = self.server.session.reject("JSON inválido: envie um objeto com os campos do comando.")
            self._json(status, state)
            return
        status, state = self.server.session.post(path, payload, self.headers.get("X-Session-ID"))
        self._json(status, state)

    def log_message(self, format: str, *args) -> None:
        # Polling once per second should not flood the terminal.
        if len(args) > 1 and str(args[1]) not in ("200", "204"):
            super().log_message(format, *args)


def main() -> None:
    parser = argparse.ArgumentParser(description="Interface web local para a matching engine C++.")
    parser.add_argument("--host", default="127.0.0.1", choices=("127.0.0.1", "localhost"))
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("A porta deve estar entre 0 e 65535.")
    session = Session(build_bridge())
    try:
        server = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError:
        session.close()
        raise
    server.session = session
    print(f"Matching Desk · http://{args.host}:{server.server_port}", flush=True)
    print("Ctrl+C encerra o servidor e descarta a sessão web.", flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        session.close()


if __name__ == "__main__":
    main()
