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
import sys

LOG_FILE = None


def log(msg):
    line = f"[{datetime.datetime.now():%Y-%m-%d %H:%M:%S}] {msg}"
    print(line, flush=True)
    if LOG_FILE:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")


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
        lines += [f"    rekord {i + 1}: {r}" for i, r in enumerate(records)]
        return "\n".join(lines), "{OK}"

    if frame.startswith(("LOGUJ-;", "AUTO--;")):
        p = frame.rstrip("@").split(";", 2)
        return f"{p[0]}  IMEI={p[1] if len(p) > 1 else '?'}  {p[2] if len(p) > 2 else ''}", "{OK}"

    return f"NIEZNANA ramka: {frame}", "{OK}"


async def client(reader, writer, args):
    peer = writer.get_extra_info("peername")
    who = f"{peer[0]}:{peer[1]}" if peer else "?"
    log(f"Połączenie od {who}")
    try:
        while True:
            try:
                raw = await asyncio.wait_for(reader.readuntil(b"\n"), args.idle_timeout)
            except asyncio.IncompleteReadError as e:
                if e.partial:
                    log(f"[{who}] niepełna ramka przed rozłączeniem: {e.partial!r}")
                break
            except asyncio.LimitOverrunError:
                raw = await reader.read(65536)
                log(f"[{who}] za długa linia, RAW: {raw!r}")
                continue
            except asyncio.TimeoutError:
                log(f"[{who}] brak danych przez {args.idle_timeout} s — zamykam")
                break

            log(f"[{who}] RAW: {raw!r}")
            frame = raw.decode("latin-1").strip("\r\n\x00\x1a ")
            if not frame:
                continue
            try:
                info, reply = handle_frame(frame, args)
            except Exception as e:                  # błąd jednej ramki nie zabija serwera
                log(f"[{who}] błąd obsługi ramki: {e!r}")
                info, reply = f"BŁĄD: {frame}", "{OK}"
            log(f"[{who}] {info}")
            writer.write(reply.encode("latin-1"))
            await writer.drain()
            log(f"[{who}] odpowiedź: {reply}")
    except (ConnectionResetError, BrokenPipeError) as e:
        log(f"[{who}] połączenie zerwane: {e!r}")
    finally:
        writer.close()
        log(f"Rozłączono {who}")


async def main():
    global LOG_FILE
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
    ap.add_argument("--idle-timeout", type=int, default=600)
    ap.add_argument("--log-file", default="tracker_server.log")
    args = ap.parse_args()
    LOG_FILE = args.log_file

    hsk_reply(args.device_host, args.device_port)       # walidacja argumentów
    server = await asyncio.start_server(lambda r, w: client(r, w, args),
                                        args.listen, args.port)
    log(f"Nasłuch na {args.listen}:{args.port}  (HSK odsyła {args.device_host}:{args.device_port})")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Zatrzymano przez Ctrl+C")
    except ValueError as e:
        sys.exit(f"Błąd: {e}")
