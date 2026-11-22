#!/usr/bin/env python3
"""
Serwer testowy dla lokalizatora (ATmega64 + SIM900, firmware 2.65).

Protokół odtworzony z firmware (szczegóły: docs/PROTOCOL.md, docs/SERVER.md):

  urządzenie -> serwer   ramki tekstowe zakończone "\\r\\n", jedna sesja TCP:
      HSK-02;<IMEI>;...          handshake            (pierwsza ramka sesji)
      CONFIG;<IMEI>;05           prośba o konfigurację
      PNG-01;<IMEI>;CAN;...      ramka stanu (FORM)
      LOK-02;<IMEI>$<rekord>$... rekordy pozycji
      LOGUJ-;<IMEI>;<tekst>@     komunikat dziennika
  serwer -> urządzenie   odpowiedź MUSI być w klamrach: {...}
      Firmware ignoruje wszystko przed '{' i kończy odbiór na '}'.
      Bez klamer czeka 30 s, uznaje to za brak odpowiedzi i zwykle się restartuje.

Uruchomienie:
  python3 tracker_server.py --device-host <host z EEPROM> --device-port 3506

--device-host / --device-port MUSZĄ być takie, jakie urządzenie ma zapisane:
odpowiedź na HSK zawiera adres serwera, a jeśli różni się od zapisanego,
urządzenie zapisuje nowy adres i się restartuje.
"""

import argparse
import asyncio
import datetime
import os
import sys

LOG_FILE = None

# --- kolory w terminalu (plik logu zawsze bez kolorów) -----------------------
USE_COLOR = False
RESET, BOLD, DIM = "\033[0m", "\033[1m", "\033[2m"
RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN = (f"\033[3{n}m" for n in (1, 2, 3, 4, 5, 6))

FRAME_COLORS = {"HSK": MAGENTA, "CONFIG": BLUE, "PNG": CYAN, "LOK": GREEN,
                "LOGUJ-": YELLOW, "AUTO--": YELLOW, "NIEZNANA": RED, "BŁĄD:": RED}


def paint(text, *codes):
    codes = [c for c in codes if c]
    return "".join(codes) + text + RESET if USE_COLOR and codes else text


def safe(text):
    """Znaki sterujące z odebranych danych jako \\xNN — żeby nie sterowały terminalem."""
    return "".join(c if c == "\n" or 32 <= ord(c) < 127 or ord(c) >= 160 else f"\\x{ord(c):02x}"
                   for c in text)


def log(msg, *codes):
    stamp = f"[{datetime.datetime.now():%Y-%m-%d %H:%M:%S}]"
    first, *rest = msg.split("\n")
    out = [paint(stamp, DIM) + " " + paint(first, *codes)]
    for line in rest:                       # dalsze linie bloku (rekordy LOK)
        out.append(paint(line, GREEN) if line.lstrip().startswith("->") else paint(line, DIM))
    print("\n".join(out), flush=True)
    if LOG_FILE:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"{stamp} {msg}\n")


def xor_sum(text):
    """Suma kontrolna jak w firmware (@0xb498): XOR kodów wszystkich znaków."""
    x = 0
    for ch in text.encode("latin-1"):
        x ^= ch
    return x


# ---------------------------------------------------------------------------
# Odpowiedź na HSK-02 (@0x2f30..0x31c2)
#   treść między klamrami: dokładnie 38 znaków
#   poz. 1-3 "HSK", 4 separator, 5-8 port, 9 separator,
#   poz. 10-34 host (krótszy kończy się '$'), 35 '*', 36-38 XOR(1..34)
# ---------------------------------------------------------------------------
def hsk_reply(host, port):
    if len(port) != 4 or not port.isdigit():
        raise ValueError("port musi mieć dokładnie 4 cyfry")
    if len(host) > 25:
        raise ValueError("host może mieć najwyżej 25 znaków")
    field = host if len(host) == 25 else (host + "$").ljust(25, "0")
    body = "HSK;" + port + ";" + field           # 34 znaki
    assert len(body) == 34
    return "{" + body + "*" + f"{xor_sum(body):03d}" + "}"


# ---------------------------------------------------------------------------
# Odpowiedź na CONFIG (@0x33dc..0x38c6)
#   pozycje liczone od 1; XOR znaków 1..77 jako 3 cyfry na pozycjach 79-81.
#   Przy złej sumie: "-CONFIG-ERROR" i restart urządzenia (kod 14).
#   Wartości domyślne = te, które firmware ustawia sam przy starcie;
#   pola oznaczone "?" mają nieustalone znaczenie (po starcie są zerami).
# ---------------------------------------------------------------------------
CONFIG_FIELDS = [
    # (pozycja, szerokość, wartość, opis)
    (6,  5, "00060", "czas do nastepnej sesji [s] (najpewniej)"),
    (12, 5, "03600", "czas do nastepnego pobrania CONFIG [s] (najpewniej)"),
    (18, 1, "2",     "liczba rekordow w ramce LOK (0 -> 6)"),
    (20, 1, "0",     "?"),
    (22, 2, "06",    "?"),
    (25, 1, "3",     "tryb roamingu 0..7 (docs/ROAMING.md)"),
    (27, 2, "00",    "?"),
    (30, 1, "0",     "?"),
    (32, 4, "0000",  "pole tekstowe, 0000 = puste"),
    (37, 3, "018",   "?"),
    (41, 2, "09",    "?"),
    (44, 2, "00",    "?"),
    (47, 1, "0",     "?"),
    (49, 2, "45",    "?"),
    (52, 1, "0",     "?"),
    (54, 3, "000",   "?"),
    (59, 1, "4",     "?"),
    (60, 1, "0",     "?"),
    (61, 1, "0",     "?"),
    (62, 1, "0",     "?"),
    (63, 1, "0",     "?"),
    (64, 1, "0",     "?"),
]


def config_reply(overrides=None):
    vals = {pos: val for pos, _, val, _ in CONFIG_FIELDS}
    vals.update(overrides or {})
    buf = list(";" * 77)
    for pos, width, _, _ in CONFIG_FIELDS:
        v = vals[pos]
        if len(v) != width or "{" in v or "}" in v:
            raise ValueError(f"pole na pozycji {pos} musi mieć {width} znaków")
        buf[pos - 1:pos - 1 + width] = v
    body = "".join(buf)                           # pozycje 1..77
    return "{" + body + "*" + f"{xor_sum(body):03d}" + "}"


# ---------------------------------------------------------------------------
# Rekord pozycji z ramki LOK (funkcja 0x210e) — układ PRAWDOPODOBNY.
# Szerokości z wartości domyślnych parsera NMEA (@0x4b20), znaczenia z etykiet
# LCD. Początek ma stałą szerokość; reszta (licznik, liczniki zdarzeń, część
# "!...!" i dane CAN) nie jest rozpisana i jest zwracana w postaci surowej.
# ---------------------------------------------------------------------------
RECORD_LAYOUT = [          # (nazwa, szerokość)
    ("lon", 9),            # dddmm.mmmm bez kropki      (0x0BA1)
    ("ew", 1),             # E / W                      (0x0BAF)
    ("lat", 8),            # ddmm.mmmm bez kropki       (0x0B95)
    ("ns", 1),             # N / S                      (0x0BAD)
    ("alt_m", 4),          # wysokość [m]   — niepewne  (0x0BCF)
    ("sats", 2),           # liczba satelitów           (0x0BB1)
    ("speed_kmh", 3),      # prędkość [km/h]            (0x0BB5)
    ("utc", 6),            # hhmmss                     (0x0B8D)
    ("date", 6),           # ddmmrr         — niepewne  (0x0BBD)
]


def decode_record(rec):
    out, pos = {}, 0
    for name, width in RECORD_LAYOUT:
        out[name] = rec[pos:pos + width]
        pos += width
    out["rest"] = rec[pos:]
    try:
        lat = int(out["lat"][:2]) + int(out["lat"][2:]) / 10000 / 60
        lon = int(out["lon"][:3]) + int(out["lon"][3:]) / 10000 / 60
        out["lat_deg"] = round(-lat if out["ns"] == "S" else lat, 6)
        out["lon_deg"] = round(-lon if out["ew"] == "W" else lon, 6)
    except ValueError:
        pass
    return out


def describe_record(rec):
    r = decode_record(rec)
    if "lat_deg" not in r:
        return rec
    t, dt = r["utc"], r["date"]
    return (f"{dt[:2]}.{dt[2:4]}.20{dt[4:]} {t[:2]}:{t[2:4]}:{t[4:]} UTC  "
            f"{r['lat_deg']:.6f}, {r['lon_deg']:.6f}  {int(r['speed_kmh'] or 0)} km/h  "
            f"sat={r['sats']} alt={r['alt_m']}  reszta={r['rest']!r}")


# ---------------------------------------------------------------------------
# Obsługa ramek
# ---------------------------------------------------------------------------
def handle_frame(frame, args):
    """Zwraca (opis do logu, odpowiedź do wysłania)."""
    if frame.startswith("HSK-02;"):
        p = frame.split(";")
        info = f"HSK  IMEI={p[1] if len(p) > 1 else '?'}  " + ";".join(p[2:])
        return info, hsk_reply(args.device_host, args.device_port)

    if frame.startswith("CONFIG;"):
        p = frame.split(";")
        return f"CONFIG  IMEI={p[1] if len(p) > 1 else '?'}", config_reply(
            {25: str(args.roaming_mode),
             6: f"{args.session_interval:05d}",
             12: f"{args.config_interval:05d}"})

    if frame.startswith("PNG-01;"):
        p = frame.split(";")
        # PNG-01;IMEI;CAN;<n>;FORM;Er=<kod PL>/<kod inne>;<pola stanu>
        er = next((x[3:] for x in p if x.startswith("Er=")), "?")
        info = (f"PNG  IMEI={p[1] if len(p) > 1 else '?'}  "
                f"kody_restartu(PL/inne)={er}  stan={p[-1] if len(p) > 6 else ''}")
        return info, "{OK}"

    if frame.startswith("LOK-02;"):
        head, _, rest = frame.partition("$")
        records = [r for r in rest.split("$") if r]
        lines = [f"LOK  IMEI={head[7:]}  rekordow={len(records)}"]
        for i, r in enumerate(records):
            lines.append(f"    rekord {i + 1}: {r}")
            lines.append(f"              -> {describe_record(r)}")
        return "\n".join(lines), "{OK}"

    if frame.startswith(("LOGUJ-;", "AUTO--;")):
        p = frame.rstrip("@").split(";", 2)
        return f"{p[0]}  IMEI={p[1] if len(p) > 1 else '?'}  {p[2] if len(p) > 2 else ''}", "{OK}"

    return f"NIEZNANA ramka: {frame}", "{OK}"


async def answer(frame_raw, who, writer, args):
    """Obsłuż jedną kompletną ramkę (zakończoną '\\n') i odeślij odpowiedź."""
    log(f"[{who}] ramka: {frame_raw!r}", DIM)
    frame = frame_raw.decode("latin-1").strip("\r\n\x00\x1a ")
    if not frame:
        log(f"[{who}] pusta linia — pomijam", YELLOW)
        return
    try:
        info, reply = handle_frame(frame, args)
    except Exception as e:                  # błąd jednej ramki nie zabija serwera
        log(f"[{who}] błąd obsługi ramki: {e!r}", BOLD, RED)
        info, reply = f"BŁĄD: {frame}", "{OK}"
    log(f"[{who}] {safe(info)}", BOLD, FRAME_COLORS.get(info.split()[0]))
    writer.write(reply.encode("latin-1"))
    await writer.drain()
    log(f"[{who}] odpowiedź: {reply}", GREEN)


async def client(reader, writer, args):
    peer = writer.get_extra_info("peername")
    who = f"{peer[0]}:{peer[1]}" if peer else "?"
    log(f"Połączenie od {who}", BOLD, CYAN)
    loop = asyncio.get_running_loop()
    buf = b""                               # dane jeszcze bez '\n'
    total = frames = 0
    last_rx = loop.time()
    warned = False
    try:
        while True:
            try:
                chunk = await asyncio.wait_for(reader.read(4096), 1.0)
            except asyncio.TimeoutError:
                idle = loop.time() - last_rx
                if buf and not warned and idle >= args.partial_warn:
                    log(f"[{who}] niepełna ramka (brak \\r\\n od {idle:.0f} s): {buf!r}", BOLD, YELLOW)
                    warned = True
                if idle >= args.idle_timeout:
                    log(f"[{who}] brak danych przez {args.idle_timeout} s — zamykam", YELLOW)
                    break
                continue

            if not chunk:                   # urządzenie zamknęło połączenie
                break

            # Każdy kawałek trafia do logu od razu — nawet jeśli to nie jest ramka.
            total += len(chunk)
            last_rx = loop.time()
            warned = False
            log(f"[{who}] odebrano {len(chunk)} B: {chunk!r}", DIM)

            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                frames += 1
                await answer(line + b"\n", who, writer, args)
            if len(buf) > 65536:
                log(f"[{who}] ponad 64 KiB bez \\r\\n — odrzucam bufor: {buf[:200]!r}...", BOLD, RED)
                buf = b""
    except (ConnectionResetError, BrokenPipeError) as e:
        log(f"[{who}] połączenie zerwane: {e!r}", RED)
    finally:
        writer.close()
        if buf:
            log(f"[{who}] niepełna ramka przed rozłączeniem: {buf!r}", BOLD, YELLOW)
        if total == 0:
            log(f"Rozłączono {who} — nie przyszedł ani jeden bajt", BOLD, YELLOW)
        else:
            log(f"Rozłączono {who} — odebrano {total} B, ramek: {frames}", CYAN)


async def main():
    global LOG_FILE, USE_COLOR
    ap = argparse.ArgumentParser(description="Serwer testowy lokalizatora GPS")
    ap.add_argument("--listen", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=3506, help="port nasłuchu")
    ap.add_argument("--device-host", required=True,
                    help="host serwera zapisany w urządzeniu (odsyłany w odpowiedzi HSK)")
    ap.add_argument("--device-port", default="3506",
                    help="port zapisany w urządzeniu (4 cyfry)")
    ap.add_argument("--roaming-mode", type=int, default=3, choices=range(8))
    ap.add_argument("--session-interval", type=int, default=60)
    ap.add_argument("--config-interval", type=int, default=3600)
    ap.add_argument("--idle-timeout", type=int, default=600,
                    help="zamknij połączenie po tylu sekundach ciszy")
    ap.add_argument("--partial-warn", type=int, default=3,
                    help="ostrzeż, gdy dane bez \\r\\n czekają w buforze tyle sekund")
    ap.add_argument("--log-file", default="tracker_server.log")
    ap.add_argument("--no-color", action="store_true", help="bez kolorów w terminalu")
    args = ap.parse_args()
    LOG_FILE = args.log_file
    USE_COLOR = sys.stdout.isatty() and not args.no_color and "NO_COLOR" not in os.environ
    if USE_COLOR and os.name == "nt":
        os.system("")                       # włącza kody ANSI w konsoli Windows

    hsk_reply(args.device_host, args.device_port)       # walidacja argumentów
    server = await asyncio.start_server(lambda r, w: client(r, w, args),
                                        args.listen, args.port)
    log(f"Nasłuch na {args.listen}:{args.port}  (HSK odsyła {args.device_host}:{args.device_port})", BOLD, GREEN)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Zatrzymano przez Ctrl+C", YELLOW)
    except ValueError as e:
        sys.exit(f"Błąd: {e}")
