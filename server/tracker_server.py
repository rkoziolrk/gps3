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

# Poziom gadatliwości terminala: 0=cicho, 1=normalnie, 2=-v (surowe bajty),
# 3=-vv (rozbicie pól). Plik logu dostaje zawsze poziom >= 2.
VERBOSITY = 1
FILE_LEVEL = 2

# --- kolory w terminalu (plik logu zawsze bez kolorów) -----------------------
USE_COLOR = False
RESET, BOLD, DIM = "\033[0m", "\033[1m", "\033[2m"
RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN = (f"\033[3{n}m" for n in (1, 2, 3, 4, 5, 6))

FRAME_COLORS = {"HSK": MAGENTA, "CONFIG": BLUE, "PNG": CYAN, "LOK": GREEN,
                "LOGUJ-": YELLOW, "AUTO--": YELLOW, "NIEZNANA": RED, "BŁĄD:": RED}

RX = "<= "   # odebrane od urządzenia
TX = "=> "   # wysłane do urządzenia
NN = "   "   # bez kierunku (połączenia, podsumowania)


def paint(text, *codes):
    codes = [c for c in codes if c]
    return "".join(codes) + text + RESET if USE_COLOR and codes else text


def safe(text):
    """Znaki sterujące z odebranych danych jako \\xNN — żeby nie sterowały terminalem."""
    return "".join(c if c == "\n" or 32 <= ord(c) < 127 or ord(c) >= 160 else f"\\x{ord(c):02x}"
                   for c in text)


def log(msg, *codes, level=1):
    """level = minimalna gadatliwość, przy której wpis trafia na terminal."""
    if level > VERBOSITY and level > FILE_LEVEL:
        return
    stamp = f"[{datetime.datetime.now():%Y-%m-%d %H:%M:%S}]"
    if level <= VERBOSITY:
        first, *rest = msg.split("\n")
        out = [paint(stamp, DIM) + " " + paint(first, *codes)]
        for line in rest:                   # dalsze linie bloku (rekordy LOK)
            green = line.lstrip().startswith(("->", "rekord"))
            out.append(paint(line, GREEN) if green else paint(line, DIM))
        print("\n".join(out), flush=True)
    if LOG_FILE and level <= FILE_LEVEL:
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
#   poz. 1-3 "HSK", 4 '=', 5-8 port, 9 ';'   (format oryginalnego serwera,
#   potwierdzony odpowiedzią "{HSK=;*086}" z logu urządzenia),
#   poz. 10-34 host (krótszy kończy się '$'), 35 '*', 36-38 XOR(1..34)
# ---------------------------------------------------------------------------
def hsk_reply(host, port):
    if len(port) != 4 or not port.isdigit():
        raise ValueError("port musi mieć dokładnie 4 cyfry")
    if len(host) > 25:
        raise ValueError("host może mieć najwyżej 25 znaków")
    field = host if len(host) == 25 else (host + "$").ljust(25, "0")
    body = "HSK=" + port + ";" + field           # 34 znaki
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
# Rekord pozycji z ramek LOK/LOO (funkcja 0x210e).
# Układ POTWIERDZONY na prawdziwej ramce LOO-02 z urządzenia.
# Część pozycyjna (45 znaków) ma stałe szerokości; po niej dwie sekcje CAN
# rozdzielone '!'. Przy odłączonym CAN sekcje CAN są bezużyteczne.
#
#   lon 9  (ddd mm.mmmm bez kropki) + E/W 1
#   lat 8  (dd  mm.mmmm bez kropki) + N/S 1
#   alt 4  [m]   sat 2   speed 3    utc 6 (hhmmss)   date 6 (ddmmrr)
#   tail 5 — licznik/flagi (nieustalone)
#   ! <CAN 22> ! <CAN 11>           (paliwo/RPM/pedał/sonda — docs/PROTOCOL.md)
# ---------------------------------------------------------------------------
POS_LAYOUT = [             # (nazwa, szerokość) — część pozycyjna, 45 znaków
    ("lon", 9), ("ew", 1), ("lat", 8), ("ns", 1),
    ("alt_m", 4), ("sats", 2), ("speed", 3), ("utc", 6), ("date", 6), ("tail", 5),
]


def decode_record(rec):
    parts = rec.split("!")
    pos = parts[0]
    out = {"can1": parts[1] if len(parts) > 1 else "",
           "can2": parts[2] if len(parts) > 2 else ""}
    i = 0
    for name, width in POS_LAYOUT:
        out[name] = pos[i:i + width]
        i += width
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
        return "nieczytelny: " + repr(rec)
    t, dt = r["utc"], r["date"]
    when = f"{dt[:2]}.{dt[2:4]}.20{dt[4:]} {t[:2]}:{t[2:4]}:{t[4:]}" if len(t) == 6 and len(dt) == 6 else "?"
    return (f"{when} UTC  {r['lat_deg']:.6f}, {r['lon_deg']:.6f}  "
            f"v={r['speed']} sat={r['sats']} alt={r['alt_m']}m  "
            f"CAN=[{r['can1']}|{r['can2']}]")


# ---------------------------------------------------------------------------
# Obsługa ramek
# ---------------------------------------------------------------------------
HSK_FIELDS = ["wersja", "operator", "model", "modem", "typ", "MCC", "MNC",
              "?", "?", "GPS", "?", "temp", "?", "?", "wer.modemu"]


def detail(lines):
    """Dodatkowe linie pokazywane tylko przy -vv (VERBOSITY >= 3)."""
    return ("\n    " + "\n    ".join(lines)) if VERBOSITY >= 3 and lines else ""


def handle_frame(frame, args):
    """Zwraca (opis do logu, odpowiedź do wysłania)."""
    if frame.startswith("HSK-02;"):
        p = frame.split(";")
        info = f"HSK  IMEI={p[1] if len(p) > 1 else '?'}  " + ";".join(p[2:])
        info += detail(f"{n:14}= {v}" for n, v in zip(HSK_FIELDS, p[2:]))
        return info, hsk_reply(args.device_host, args.device_port)

    if frame.startswith("CONFIG;"):
        p = frame.split(";")
        reply = config_reply({25: str(args.roaming_mode),
                              6: f"{args.session_interval:05d}",
                              12: f"{args.config_interval:05d}"})
        info = f"CONFIG  IMEI={p[1] if len(p) > 1 else '?'}"
        info += detail([f"wysyłam: {reply}"]
                       + [f"poz.{pos:<3}szer.{w}= {reply[pos:pos + w]!r}  {desc}"
                          for pos, w, _, desc in CONFIG_FIELDS])
        return info, reply

    if frame.startswith("PNG-01;"):
        p = frame.split(";")
        # PNG-01;IMEI;CAN;<n>;FORM;Er=<kod PL>/<kod inne>;<pola stanu>
        er = next((x[3:] for x in p if x.startswith("Er=")), "?")
        info = (f"PNG  IMEI={p[1] if len(p) > 1 else '?'}  "
                f"kody_restartu(PL/inne)={er}  stan={p[-1] if len(p) > 6 else ''}")
        info += detail(f"{x}" for x in p[2:])
        return info, "{OK}"

    if frame.startswith(("LOK-02;", "LOO-02;")):
        kind = frame[:6]                       # LOK = bieżące, LOO = zaległe z pamięci
        head, _, rest = frame.partition("$")
        records = [r for r in rest.split("$") if r]
        lines = [f"{kind}  IMEI={head[7:]}  rekordów={len(records)}"]
        for i, r in enumerate(records):
            lines.append(f"    rekord {i + 1}: -> {describe_record(r)}")
            if VERBOSITY >= 3:                  # -vv: surowy rekord i rozbicie pól
                d = decode_record(r)
                lines.append(f"        raw   = {r}")
                lines += [f"        {k:6}= {d.get(k, '')}" for k, _ in POS_LAYOUT]
                lines += [f"        {k:6}= {d.get(k, '')}" for k in ("can1", "can2")]
        return "\n".join(lines), "{OK}"

    if frame.startswith(("LOO-BE;", "LOO-EN;")):
        # znaczniki początku/końca paczki rekordów zaległych
        return f"{frame[:6]}  (znacznik paczki OLD)", "{OK}"

    if frame.startswith("SESJA-;"):
        # znacznik początku sesji: SESJA-;IMEI;<lon9>E<lat8>N<utc6><date6><tail4>@
        p = frame.rstrip("@").split(";")
        body = p[2] if len(p) > 2 else ""
        info = f"SESJA  IMEI={p[1] if len(p) > 1 else '?'}"
        try:
            lon, ew, lat, ns = body[:9], body[9], body[10:18], body[18]
            t, dt = body[19:25], body[25:31]
            latd = (int(lat[:2]) + float(lat[2:]) / 10000 / 60) * (-1 if ns == "S" else 1)
            lond = (int(lon[:3]) + float(lon[3:]) / 10000 / 60) * (-1 if ew == "W" else 1)
            info += (f"  start {dt[:2]}.{dt[2:4]}.20{dt[4:]} {t[:2]}:{t[2:4]}:{t[4:]} UTC"
                     f"  {latd:.6f}, {lond:.6f}")
        except (ValueError, IndexError):
            info += f"  (nieczytelny: {body!r})"
        return info, "{OK}"

    if frame.startswith(("LOGUJ-;", "AUTO--;")):
        p = frame.rstrip("@").split(";", 2)
        return f"{p[0]}  IMEI={p[1] if len(p) > 1 else '?'}  {p[2] if len(p) > 2 else ''}", "{OK}"

    return f"NIEZNANA ramka: {frame}", "{OK}"


async def answer(frame_raw, who, writer, args):
    """Obsłuż jedną kompletną ramkę (zakończoną '\\n') i odeślij odpowiedź."""
    log(f"[{who}] {RX}ramka: {frame_raw!r}", DIM, level=2)
    frame = frame_raw.decode("latin-1").strip("\r\n\x00\x1a ")
    if not frame:
        log(f"[{who}] {RX}pusta linia — pomijam", YELLOW, level=2)
        return
    try:
        info, reply = handle_frame(frame, args)
    except Exception as e:                  # błąd jednej ramki nie zabija serwera
        log(f"[{who}] {RX}błąd obsługi ramki: {e!r}", BOLD, RED, level=0)
        info, reply = f"BŁĄD: {frame}", "{OK}"
    log(f"[{who}] {RX}{safe(info)}", BOLD, FRAME_COLORS.get(info.split()[0]), level=0)
    writer.write(reply.encode("latin-1"))
    await writer.drain()
    log(f"[{who}] {TX}odpowiedź: {reply}", BOLD, GREEN, level=1)


async def client(reader, writer, args):
    peer = writer.get_extra_info("peername")
    who = f"{peer[0]}:{peer[1]}" if peer else "?"
    log(f"{NN}Połączenie od {who}", BOLD, CYAN, level=0)
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
                    log(f"[{who}] {RX}niepełna ramka (brak \\r\\n od {idle:.0f} s): {buf!r}", BOLD, YELLOW, level=0)
                    warned = True
                if idle >= args.idle_timeout:
                    log(f"[{who}] {NN}brak danych przez {args.idle_timeout} s — zamykam", YELLOW, level=1)
                    break
                continue

            if not chunk:                   # urządzenie zamknęło połączenie
                break

            # Każdy kawałek trafia do logu od razu — nawet jeśli to nie jest ramka.
            total += len(chunk)
            last_rx = loop.time()
            warned = False
            log(f"[{who}] {RX}odebrano {len(chunk)} B: {chunk!r}", DIM, level=2)

            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                frames += 1
                await answer(line + b"\n", who, writer, args)
            if len(buf) > 65536:
                log(f"[{who}] {RX}ponad 64 KiB bez \\r\\n — odrzucam bufor: {buf[:200]!r}...", BOLD, RED, level=0)
                buf = b""
    except (ConnectionResetError, BrokenPipeError) as e:
        log(f"[{who}] {NN}połączenie zerwane: {e!r}", RED, level=1)
    finally:
        writer.close()
        if buf:
            log(f"[{who}] {RX}niepełna ramka przed rozłączeniem: {buf!r}", BOLD, YELLOW, level=0)
        if total == 0:
            log(f"{NN}Rozłączono {who} — nie przyszedł ani jeden bajt", BOLD, YELLOW, level=0)
        else:
            log(f"{NN}Rozłączono {who} — odebrano {total} B, ramek: {frames}", CYAN, level=0)


async def main():
    global LOG_FILE, USE_COLOR, VERBOSITY, FILE_LEVEL
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
    ap.add_argument("--log-file", default=None,
                    help="plik logu (domyślnie tracker_server_<data>_<godz>.log; "
                         "w podanej nazwie {time} jest zastępowane znacznikiem czasu; "
                         "'' albo 'none' wyłącza zapis)")
    ap.add_argument("--no-color", action="store_true", help="bez kolorów w terminalu")
    ap.add_argument("-v", "--verbose", action="count", default=0,
                    help="więcej szczegółów: -v surowe bajty, -vv rozbicie pól")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="tylko połączenia, ramki i błędy (bez odpowiedzi i surowych bajtów)")
    args = ap.parse_args()
    VERBOSITY = 0 if args.quiet else 1 + args.verbose       # 0=cicho,1=norm,2=-v,3=-vv
    FILE_LEVEL = max(2, VERBOSITY)                           # plik logu: zawsze >= 2
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    if args.log_file is None:
        LOG_FILE = f"tracker_server_{stamp}.log"
    elif args.log_file.lower() in ("", "none"):
        LOG_FILE = None
    else:
        LOG_FILE = args.log_file.replace("{time}", stamp)
    USE_COLOR = sys.stdout.isatty() and not args.no_color and "NO_COLOR" not in os.environ
    if USE_COLOR and os.name == "nt":
        os.system("")                       # włącza kody ANSI w konsoli Windows

    hsk_reply(args.device_host, args.device_port)       # walidacja argumentów
    server = await asyncio.start_server(lambda r, w: client(r, w, args),
                                        args.listen, args.port)
    log(f"Nasłuch na {args.listen}:{args.port}  (HSK odsyła {args.device_host}:{args.device_port})", BOLD, GREEN, level=0)
    if LOG_FILE:
        log(f"Log: {LOG_FILE}", DIM, level=0)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Zatrzymano przez Ctrl+C", YELLOW, level=0)
    except ValueError as e:
        sys.exit(f"Błąd: {e}")
