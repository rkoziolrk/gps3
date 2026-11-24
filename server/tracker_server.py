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

Interfejs WWW (mapa + odczyt CAN + log na żywo) rusza domyślnie na porcie 8080
z tego samego procesu — tylko biblioteka standardowa, bez zależności, więc
chodzi też na routerze OpenWRT. Uruchomienie na routerze: docs/OPENWRT.md.
"""

import argparse
import asyncio
import collections
import datetime
import json
import os
import signal
import sys

LOG_FILE = None


def now_iso():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

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


def decode_can(c1, c2):
    """Rozbija sekcje CAN na pola. Układ odtworzony z logu jazdy (CAN podłączony),
    znaczenia potwierdzone korelacją z prędkością/pedałem — część niepewna.
      CAN1 (22): spd3 pedal4 k1 cons4 odo10
      CAN2 (11): '+' '99' fuel4 stat4
    """
    out = {}
    if len(c1) == 22:
        out["can_speed"] = c1[0:3]      # prędkość z CAN [km/h] (koreluje z GPS)
        out["pedal"] = c1[3:7]          # pedał gazu (0 = puszczony)
        out["_k"] = c1[7:8]             # stała '6' — nieustalone
        out["f_a"] = c1[8:12]           # zmienna szybko, odwrotnie do prędkości — nieustalone
        out["odo"] = c1[12:22]          # przebieg w metrach (/1000 = km, POTWIERDZONE)
    if len(c2) == 11 and c2[0] == "+":
        out["f_b"] = c2[3:7]            # stabilne przy pracującym silniku — nieustalone (NIE paliwo)
        out["stat"] = c2[7:11]          # status/kod — nieustalone
    return out


def decode_record(rec):
    parts = rec.split("!")
    pos = parts[0]
    out = {"can1": parts[1] if len(parts) > 1 else "",
           "can2": parts[2] if len(parts) > 2 else ""}
    i = 0
    for name, width in POS_LAYOUT:
        out[name] = pos[i:i + width]
        i += width
    out.update(decode_can(out["can1"], out["can2"]))
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
    if "can_speed" in r:
        can = (f"CAN[v={int(r['can_speed'])} ped={int(r['pedal'])} "
               f"przebieg={int(r['odo'])/1000:.1f}km  ?A={r['f_a']} ?B={r.get('f_b', '')}]")
    elif r["can1"] or r["can2"]:
        can = f"CAN=[{r['can1']}|{r['can2']}]"
    else:
        can = "CAN=brak"
    return (f"{when} UTC  {r['lat_deg']:.6f}, {r['lon_deg']:.6f}  "
            f"v={r['speed']} sat={r['sats']} alt={r['alt_m']}m  {can}")


# ---------------------------------------------------------------------------
# Obsługa ramek
# ---------------------------------------------------------------------------
HSK_FIELDS = ["wersja", "operator", "model", "modem", "typ", "MCC", "MNC",
              "?", "?", "GPS", "?", "temp", "?", "?", "wer.modemu"]


def detail(lines):
    """Dodatkowe linie pokazywane tylko przy -vv (VERBOSITY >= 3)."""
    return ("\n    " + "\n    ".join(lines)) if VERBOSITY >= 3 and lines else ""


def nproto_xor(body):
    """XOR znaków między '#' a '*' — suma nowego protokołu (jak w firmware)."""
    x = 0
    for ch in body:
        x ^= ord(ch)
    return x


def nproto_reply(typ):
    body = typ
    return f"#{body}*{nproto_xor(body):02X}\r\n"


def handle_new_proto(frame, state, conn):
    """Nowy protokół firmware 3.x: #TYP,pola*XOR. Zwraca (opis, odpowiedź).

    POS/RAW nie niosą IMEI — wiążemy je z urządzeniem po połączeniu TCP
    (IMEI zapamiętany z HELLO w conn["imei"])."""
    core, star, cs = frame[1:].partition("*")
    if star:
        want = int(cs[:2], 16) if len(cs) >= 2 and all(c in "0123456789abcdefABCDEF" for c in cs[:2]) else -1
        got = nproto_xor(core)
        if want >= 0 and want != got:
            return f"#?  zła suma (jest {got:02X}, w ramce {cs[:2]}): {frame}", nproto_reply("ERR")
    f = core.split(",")
    typ = f[0]

    if typ == "HELLO":
        # HELLO,proto,imei,fw,gps,gsm
        imei = f[2] if len(f) > 2 else "?"
        conn["imei"] = imei
        state.on_hello(imei, conn["who"], f[1] if len(f) > 1 else None,
                       f[3] if len(f) > 3 else None,
                       f[4] if len(f) > 4 else None,
                       f[5] if len(f) > 5 else None)
        return (f"HELLO  IMEI={imei}  proto={f[1] if len(f) > 1 else '?'} "
                f"fw={f[3] if len(f) > 3 else '?'} {'/'.join(f[4:])}"), nproto_reply("OK")

    if typ == "PING":
        imei = f[1] if len(f) > 1 else (conn.get("imei") or "?")
        conn.setdefault("imei", imei)
        state.on_ping(imei, conn["who"], f[2] if len(f) > 2 else None)
        return f"PING  IMEI={imei}  uptime={f[2] if len(f) > 2 else '?'}s", nproto_reply("OK")

    if typ == "RAW":
        # RAW,<src>,<payload...>  — payload (ramka CAN) może zawierać przecinki
        src = f[1] if len(f) > 1 else "?"
        payload = core.split(",", 2)[2] if len(f) > 2 else ""
        info = f"RAW {src}  {payload}"
        can = decode_can(*payload.split("!")[:2]) if "!" in payload else {}
        if can:
            info += "   " + " ".join(f"{k}={v}" for k, v in can.items())
        state.on_can(conn.get("imei") or "?", conn["who"], can or None, payload)
        return info, nproto_reply("OK")

    if typ == "POS":
        # POS,utc,lat1e7,lon1e7,alt,sat,spd,course,flag[,CAN:<raw>]
        try:
            utc = f[1]
            lat = int(f[2]) / 1e7
            lon = int(f[3]) / 1e7
            when = f"{utc[:2]}:{utc[2:4]}:{utc[4:]}" if len(utc) == 6 else utc
            info = (f"POS  {when} UTC  {lat:.6f}, {lon:.6f}  "
                    f"alt={f[4]}m sat={f[5]} v={f[6]} kurs={f[7]} {f[8]}")
            pos = {"t": when, "lat": round(lat, 6), "lon": round(lon, 6),
                   "alt": f[4], "sat": f[5], "spd": f[6], "course": f[7],
                   "flag": f[8] if len(f) > 8 else "", "can": None, "can_raw": ""}
            if "CAN:" in core:
                craw = core.split("CAN:", 1)[1]
                info += f"   CAN[{craw}]"
                can = decode_can(*craw.split("!")[:2]) if "!" in craw else {}
                pos["can"] = can or None
                pos["can_raw"] = craw
                if can:
                    info += "  " + " ".join(f"{k}={v}" for k, v in can.items())
            state.on_pos(conn.get("imei") or "?", conn["who"], pos)
        except (ValueError, IndexError) as e:
            info = f"POS nieczytelny ({e!r}): {frame}"
        return info, nproto_reply("OK")

    return f"#? nieznany typ: {frame}", nproto_reply("OK")


def handle_frame(frame, args, state, conn):
    """Zwraca (opis do logu, odpowiedź do wysłania)."""
    if frame.startswith("FWREQ"):
        # Bootloader prosi o obraz: odsyłamy "FW <rozmiar> <crc32>\n" + bajty.
        if not state.ota_armed or not state.ota_fw:
            return "FWREQ — OTA nieuzbrojone / brak obrazu, odmawiam", "FWERR\n"
        try:
            with open(state.ota_fw, "rb") as f:
                data = f.read()
        except OSError as e:
            return f"FWREQ — nie mogę otworzyć {state.ota_fw}: {e}", "FWERR\n"
        import zlib
        crc = zlib.crc32(data) & 0xFFFFFFFF
        header = f"FW {len(data)} {crc:08x}\n"
        reply = header + data.decode("latin-1")     # bajty 1:1 w latin-1
        state.ota_last = {"t": now_iso(), "size": len(data), "crc": f"{crc:08x}",
                          "who": conn.get("who"), "imei": conn.get("imei"),
                          "fw": state.ota_fw}
        state.publish("ota", state.ota_status())
        return f"FWREQ -> wysyłam {len(data)} B, crc={crc:08x}", reply

    if frame.startswith("#"):
        return handle_new_proto(frame, state, conn)

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
        imei = head[7:]
        conn.setdefault("imei", imei)
        records = [r for r in rest.split("$") if r]
        lines = [f"{kind}  IMEI={imei}  rekordów={len(records)}"]
        for i, r in enumerate(records):
            lines.append(f"    rekord {i + 1}: -> {describe_record(r)}")
            d = decode_record(r)
            if "lat_deg" in d:
                t, dt = d.get("utc", ""), d.get("date", "")
                when = (f"{t[:2]}:{t[2:4]}:{t[4:]}" if len(t) == 6 else t)
                can = {k: d[k] for k in ("can_speed", "pedal", "odo", "f_a", "f_b", "stat") if k in d}
                state.on_pos(imei, conn["who"], {
                    "t": when, "lat": d["lat_deg"], "lon": d["lon_deg"],
                    "alt": d.get("alt_m", ""), "sat": d.get("sats", ""),
                    "spd": d.get("speed", ""), "course": "", "flag": "",
                    "can": can or None, "can_raw": (d.get("can1", "") + "!" + d.get("can2", "")) if d.get("can1") else ""})
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


async def answer(frame_raw, who, writer, args, state, conn):
    """Obsłuż jedną kompletną ramkę (zakończoną '\\n') i odeślij odpowiedź."""
    log(f"[{who}] {RX}ramka: {frame_raw!r}", DIM, level=2)
    frame = frame_raw.decode("latin-1").strip("\r\n\x00\x1a ")
    if not frame:
        log(f"[{who}] {RX}pusta linia — pomijam", YELLOW, level=2)
        return
    try:
        info, reply = handle_frame(frame, args, state, conn)
    except Exception as e:                  # błąd jednej ramki nie zabija serwera
        log(f"[{who}] {RX}błąd obsługi ramki: {e!r}", BOLD, RED, level=0)
        info, reply = f"BŁĄD: {frame}", "{OK}"
    log(f"[{who}] {RX}{safe(info)}", BOLD, FRAME_COLORS.get(info.split()[0]), level=0)
    state.note_raw(who, "<=", safe(info), conn.get("imei"))

    # OTA: nagłówek osobno, potem firmware w kawałkach RÓWNYCH stronie flasha
    # (256 B) z przerwą na SPM. Bootloader zapisuje stronę (SPM ~9 ms blokuje
    # CPU i gubiłby bajty), więc nadajemy chunk = 1 strona, a w przerwie modem
    # nic nie sypie -> zapis wypada w ciszy, zero gubienia bajtów.
    if frame.startswith("FWREQ") and reply.startswith("FW "):
        nl = reply.index("\n") + 1
        header = reply[:nl].encode("latin-1")       # "FW <size> <crc>\n"
        fw = reply[nl:].encode("latin-1")           # surowy obraz
        writer.write(header)
        await writer.drain()
        await asyncio.sleep(0.45)
        for i in range(0, len(fw), 256):            # 256 = SPM_PAGESIZE
            writer.write(fw[i:i + 256])
            await writer.drain()
            await asyncio.sleep(0.45)               # czas na odbiór strony + SPM
        log(f"[{who}] {TX}OTA: wysłano {len(fw)} B (strony 256 B + przerwa)", BOLD, GREEN, level=0)
        return

    writer.write(reply.encode("latin-1"))
    await writer.drain()
    log(f"[{who}] {TX}odpowiedź: {reply}", BOLD, GREEN, level=1)


async def client(reader, writer, args, state):
    peer = writer.get_extra_info("peername")
    who = f"{peer[0]}:{peer[1]}" if peer else "?"
    conn = {"who": who, "imei": None}
    state.conns[who] = writer
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
                await answer(line + b"\n", who, writer, args, state, conn)
            if len(buf) > 65536:
                log(f"[{who}] {RX}ponad 64 KiB bez \\r\\n — odrzucam bufor: {buf[:200]!r}...", BOLD, RED, level=0)
                buf = b""
    except (ConnectionResetError, BrokenPipeError) as e:
        log(f"[{who}] {NN}połączenie zerwane: {e!r}", RED, level=1)
    finally:
        writer.close()
        state.conns.pop(who, None)
        state.on_disconnect(who)
        if buf:
            log(f"[{who}] {RX}niepełna ramka przed rozłączeniem: {buf!r}", BOLD, YELLOW, level=0)
        if total == 0:
            log(f"{NN}Rozłączono {who} — nie przyszedł ani jeden bajt", BOLD, YELLOW, level=0)
        else:
            log(f"{NN}Rozłączono {who} — odebrano {total} B, ramek: {frames}", CYAN, level=0)


# ===========================================================================
# Wspólny stan + interfejs WWW (stdlib, bez zależności — pod OpenWRT)
# ===========================================================================
class State:
    """Pamięć podręczna widoku: urządzenia, ich ostatnia pozycja/CAN, trasa i
    surowy log. Wszystko w RAM i OGRANICZONE (router ma mało pamięci)."""

    def __init__(self, max_track=500, max_raw=300):
        self.devices = {}                       # imei -> rekord urządzenia
        self.conns = {}                         # who -> writer (aktywne poł. urządzeń)
        self.raw = collections.deque(maxlen=max_raw)
        self.subs = set()                       # kolejki SSE (po jednej na przeglądarkę)
        self.sse_writers = set()                # writery SSE (do flush przy zamykaniu)
        self.max_track = max_track
        self.ota_fw = None
        self.ota_armed = False
        self.ota_last = None
        self.seq = 0

    # --- urządzenia --------------------------------------------------------
    def _dev(self, imei):
        d = self.devices.get(imei)
        if d is None:
            d = {"imei": imei, "fw": None, "proto": None, "gps": None, "gsm": None,
                 "first_seen": now_iso(), "last_seen": now_iso(), "peer": None,
                 "online": True, "uptime": None, "pos": None, "can": None,
                 "track": collections.deque(maxlen=self.max_track), "frames": 0}
            self.devices[imei] = d
        return d

    def _pub_dev(self, d):
        pd = {k: d[k] for k in ("imei", "fw", "proto", "gps", "gsm", "first_seen",
                                "last_seen", "peer", "online", "uptime", "pos",
                                "can", "frames")}
        return pd

    # --- publikacja do przeglądarek (SSE) ----------------------------------
    def publish(self, kind, payload):
        self.seq += 1
        msg = json.dumps({"seq": self.seq, "kind": kind, "payload": payload},
                         ensure_ascii=False)
        for q in list(self.subs):
            try:
                q.put_nowait(msg)
            except asyncio.QueueFull:
                pass                            # wolny klient gubi aktualizacje

    # --- zdarzenia z protokołu --------------------------------------------
    def on_hello(self, imei, peer, proto, fw, gps, gsm):
        d = self._dev(imei)
        d.update(peer=peer, proto=proto, fw=fw, gps=gps, gsm=gsm,
                 online=True, last_seen=now_iso())
        d["frames"] += 1
        self.publish("device", self._pub_dev(d))

    def on_ping(self, imei, peer, uptime):
        d = self._dev(imei)
        d.update(peer=peer, uptime=uptime, online=True, last_seen=now_iso())
        d["frames"] += 1
        self.publish("device", self._pub_dev(d))

    def on_pos(self, imei, peer, pos):
        d = self._dev(imei)
        d.update(peer=peer, online=True, last_seen=now_iso(), pos=pos)
        d["frames"] += 1
        if pos.get("lat") is not None:
            d["track"].append([pos["lat"], pos["lon"], pos.get("t")])
        if pos.get("can"):
            d["can"] = pos["can"]
        self.publish("pos", {"imei": imei, "pos": pos})

    def on_can(self, imei, peer, can, raw):
        d = self._dev(imei)
        d.update(peer=peer, online=True, last_seen=now_iso())
        d["frames"] += 1
        if can:
            d["can"] = can
        self.publish("can", {"imei": imei, "can": can, "raw": raw})

    def on_disconnect(self, who):
        for d in self.devices.values():
            if d.get("peer") == who and d.get("online"):
                d["online"] = False
                self.publish("device", self._pub_dev(d))

    def note_raw(self, who, direction, text, imei=None):
        item = {"t": now_iso(), "who": who, "dir": direction,
                "imei": imei, "text": text}
        self.raw.append(item)
        self.publish("raw", item)

    # --- migawki -----------------------------------------------------------
    def ota_status(self):
        return {"fw": self.ota_fw, "armed": self.ota_armed, "last": self.ota_last}

    def snapshot(self):
        devs = []
        for d in self.devices.values():
            pd = self._pub_dev(d)
            pd["track"] = list(d["track"])
            devs.append(pd)
        return {"devices": devs, "raw": list(self.raw),
                "ota": self.ota_status(), "seq": self.seq}


def http_response(status="200 OK", ctype="text/html; charset=utf-8", body=b"",
                  extra=""):
    if isinstance(body, str):
        body = body.encode("utf-8")
    head = (f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n"
            f"Cache-Control: no-store\r\n{extra}\r\n")
    return head.encode("latin-1") + body


async def read_http_request(reader):
    """Minimalny parser HTTP/1.1 (bez zależności). Zwraca (method, path, headers, body)."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = await reader.read(2048)
        if not chunk:
            return None
        data += chunk
        if len(data) > 65536:                   # ochrona przed zalaniem pamięci
            break
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1", "replace").split("\r\n")
    parts = (lines[0].split(" ") + ["", "", ""])[:3]
    method, path = parts[0], parts[1]
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    body = rest
    clen = int(headers.get("content-length", "0") or 0)
    while len(body) < clen:
        chunk = await reader.read(clen - len(body))
        if not chunk:
            break
        body += chunk
    return method, path, headers, body


async def sse_stream(writer, state):
    """Server-Sent Events: po jednej kolejce na przeglądarkę. Pierwsza wiadomość
    to pełna migawka, potem przyrostowo. Keepalive co 15 s (komentarz SSE)."""
    q = asyncio.Queue(maxsize=400)
    state.subs.add(q)
    state.sse_writers.add(writer)
    try:
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     b"Cache-Control: no-store\r\nConnection: keep-alive\r\n"
                     b"Access-Control-Allow-Origin: *\r\n\r\n")
        snap = json.dumps(state.snapshot(), ensure_ascii=False)
        writer.write(f"event: snapshot\ndata: {snap}\n\n".encode("utf-8"))
        await writer.drain()
        while True:
            try:
                msg = await asyncio.wait_for(q.get(), 15)
                writer.write(f"data: {msg}\n\n".encode("utf-8"))
            except asyncio.TimeoutError:
                writer.write(b": keepalive\n\n")
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, asyncio.CancelledError):
        pass
    finally:
        state.subs.discard(q)
        state.sse_writers.discard(writer)
        try:
            writer.close()
        except Exception:
            pass


async def http_client(reader, writer, state, args):
    try:
        req = await read_http_request(reader)
        if not req:
            writer.close()
            return
        method, path, headers, body = req
        qpath = path.split("?", 1)[0]

        if method == "GET" and qpath in ("/", "/index.html"):
            writer.write(http_response(body=INDEX_HTML))
        elif method == "GET" and qpath == "/api/state":
            writer.write(http_response(ctype="application/json",
                         body=json.dumps(state.snapshot(), ensure_ascii=False)))
        elif method == "GET" and qpath == "/api/events":
            await sse_stream(writer, state)
            return
        elif qpath == "/api/ota":
            if method == "POST":
                try:
                    j = json.loads(body.decode("utf-8") or "{}")
                except ValueError:
                    j = {}
                if "armed" in j:
                    state.ota_armed = bool(j["armed"])
                if j.get("fw"):
                    state.ota_fw = str(j["fw"])
                state.publish("ota", state.ota_status())
                log(f"{NN}WWW: OTA armed={state.ota_armed} fw={state.ota_fw}", BOLD, YELLOW, level=0)
            writer.write(http_response(ctype="application/json",
                         body=json.dumps(state.ota_status(), ensure_ascii=False)))
        else:
            writer.write(http_response("404 Not Found", "text/plain; charset=utf-8",
                                       "nie znaleziono"))
        await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    except Exception as e:                      # błąd jednego żądania nie kładzie serwera
        log(f"{NN}WWW błąd: {e!r}", RED, level=1)
    finally:
        try:
            writer.close()
        except Exception:
            pass


# Strona WWW — pojedynczy plik, bez buildu. Leaflet z CDN (przeglądarka musi mieć
# internet do kafelków mapy); bez sieci i tak działa lista, CAN i surowy log.
INDEX_HTML = r"""<!doctype html>
<html lang="pl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Lokalizator — podgląd</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"
      integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY=" crossorigin="">
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --panel2:#1c232c; --line:#2b3440;
    --fg:#e6edf3; --dim:#8b98a5; --accent:#2f81f7; --ok:#3fb950; --warn:#d29922; --bad:#f85149;
  }
  *{box-sizing:border-box}
  html,body{margin:0;height:100%;font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
  #app{display:grid;grid-template-columns:320px 1fr;grid-template-rows:auto 1fr 180px;height:100%}
  header{grid-column:1/3;display:flex;align-items:center;gap:14px;padding:8px 14px;background:var(--panel);border-bottom:1px solid var(--line)}
  header h1{font-size:15px;margin:0;font-weight:600}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--bad);display:inline-block;margin-right:6px}
  .dot.on{background:var(--ok)}
  #side{grid-row:2/4;background:var(--panel);border-right:1px solid var(--line);overflow:auto}
  #map{grid-row:2/3}
  #raw{grid-row:3/4;background:#0a0d12;border-top:1px solid var(--line);overflow:auto;font:12px/1.35 ui-monospace,Menlo,Consolas,monospace;padding:6px 10px}
  .dev{padding:9px 12px;border-bottom:1px solid var(--line);cursor:pointer}
  .dev:hover{background:var(--panel2)}
  .dev.sel{background:var(--panel2);box-shadow:inset 3px 0 0 var(--accent)}
  .dev .imei{font-weight:600}
  .dev .meta{color:var(--dim);font-size:12px;margin-top:2px}
  .badge{font-size:11px;padding:1px 6px;border-radius:10px;background:#30363d;color:var(--dim)}
  .badge.on{background:rgba(63,185,80,.15);color:var(--ok)}
  .badge.off{background:rgba(248,81,73,.15);color:var(--bad)}
  .sect{padding:10px 12px;border-bottom:1px solid var(--line)}
  .sect h2{font-size:12px;text-transform:uppercase;letter-spacing:.05em;color:var(--dim);margin:0 0 8px}
  table.kv{width:100%;border-collapse:collapse}
  table.kv td{padding:2px 0;vertical-align:top}
  table.kv td.k{color:var(--dim);width:92px;white-space:nowrap}
  table.kv td.v{font-family:ui-monospace,monospace}
  .can td.v{color:var(--warn)}
  button{font:inherit;background:var(--accent);color:#fff;border:0;border-radius:6px;padding:6px 12px;cursor:pointer}
  button.ghost{background:#30363d}
  button:disabled{opacity:.5;cursor:default}
  .line{margin:1px 0;white-space:pre-wrap;word-break:break-all}
  .line .tt{color:var(--dim)} .line .rx{color:var(--ok)} .line .imei{color:var(--accent)}
  a{color:var(--accent)}
  .ota small{color:var(--dim)}
  @media(max-width:720px){#app{grid-template-columns:1fr;grid-template-rows:auto 220px 1fr 150px}#side{grid-row:2/3}#map{grid-row:3/4}#raw{grid-row:4/5}}
</style>
</head>
<body>
<div id="app">
  <header>
    <h1>Lokalizator — podgląd na żywo</h1>
    <span><span id="dot" class="dot"></span><span id="status">łączenie…</span></span>
    <span class="badge" id="devcount">0 urządzeń</span>
    <span style="flex:1"></span>
    <span class="ota" id="ota"></span>
  </header>
  <div id="side"></div>
  <div id="map"></div>
  <div id="raw"></div>
</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"
        integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=" crossorigin=""></script>
<script>
const CAN_LABELS = {can_speed:"prędkość CAN [km/h]", pedal:"pedał gazu", odo:"przebieg [m]",
                    f_a:"pole ?A", f_b:"pole ?B (nie paliwo)", stat:"status", _k:"stała"};
const devices = {};
let selected = null, map = null, layers = {}, es = null, retryTimer = null;

function el(html){const t=document.createElement("template");t.innerHTML=html.trim();return t.content.firstChild;}
function esc(s){return String(s==null?"":s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}

function initMap(){
  if(typeof L === "undefined"){document.getElementById("map").innerHTML =
    '<div style="padding:16px;color:var(--dim)">Mapa niedostępna (brak internetu do kafelków). Lista i CAN działają.</div>';return;}
  map = L.map("map",{zoomControl:true}).setView([52.0,19.0],6);
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    {maxZoom:19, attribution:"© OpenStreetMap"}).addTo(map);
}

function devLayer(imei){
  if(!map) return null;
  if(!layers[imei]) layers[imei] = {track:L.polyline([],{color:"#2f81f7",weight:4,opacity:.85}).addTo(map),
                                    marker:L.circleMarker([0,0],{radius:7,color:"#fff",weight:2,fillColor:"#2f81f7",fillOpacity:1})};
  return layers[imei];
}

function renderSide(){
  const side = document.getElementById("side");
  const list = Object.values(devices).sort((a,b)=>(b.last_seen||"").localeCompare(a.last_seen||""));
  document.getElementById("devcount").textContent = list.length + " urządzeń";
  let h = "";
  for(const d of list){
    const on = d.online ? '<span class="badge on">online</span>' : '<span class="badge off">offline</span>';
    const v = d.pos ? ("v="+esc(d.pos.spd)) : "";
    h += `<div class="dev ${d.imei===selected?'sel':''}" data-imei="${esc(d.imei)}">
      <div class="imei">${esc(d.imei)} ${on}</div>
      <div class="meta">fw ${esc(d.fw||"?")} · ${esc(d.last_seen||"")} ${v}</div></div>`;
  }
  // panel szczegółów wybranego
  const d = devices[selected];
  if(d){
    const p = d.pos || {};
    h += `<div class="sect"><h2>Wybrane: ${esc(d.imei)}</h2><table class="kv">
      <tr><td class="k">online</td><td class="v">${d.online?"tak":"nie"}</td></tr>
      <tr><td class="k">peer</td><td class="v">${esc(d.peer||"?")}</td></tr>
      <tr><td class="k">proto/fw</td><td class="v">${esc(d.proto||"?")} / ${esc(d.fw||"?")}</td></tr>
      <tr><td class="k">GPS/GSM</td><td class="v">${esc(d.gps||"?")} / ${esc(d.gsm||"?")}</td></tr>
      <tr><td class="k">uptime</td><td class="v">${esc(d.uptime||"?")} s</td></tr>
      <tr><td class="k">ramek</td><td class="v">${esc(d.frames||0)}</td></tr>
      <tr><td class="k">pozycja</td><td class="v">${p.lat!=null?p.lat.toFixed(6)+", "+p.lon.toFixed(6):"—"}</td></tr>
      <tr><td class="k">czas/sat</td><td class="v">${esc(p.t||"?")} / sat ${esc(p.sat||"?")}</td></tr>
      ${p.lat!=null?`<tr><td class="k">mapa</td><td class="v"><a href="https://www.openstreetmap.org/?mlat=${p.lat}&mlon=${p.lon}#map=16/${p.lat}/${p.lon}" target="_blank">OSM</a></td></tr>`:""}
      </table></div>`;
    const can = d.can || {};
    if(Object.keys(can).length){
      let cr = "";
      for(const k in can) cr += `<tr><td class="k">${esc(CAN_LABELS[k]||k)}</td><td class="v">${esc(can[k])}</td></tr>`;
      h += `<div class="sect"><h2>CAN</h2><table class="kv can">${cr}</table></div>`;
    }
  }
  side.innerHTML = h;
  side.querySelectorAll(".dev").forEach(e=>e.onclick=()=>{selected=e.dataset.imei;focusDev();renderSide();});
}

function focusDev(){
  const d = devices[selected]; if(!d||!map) return;
  const L2 = devLayer(selected);
  const pts = (d.track||[]).filter(t=>t[0]!=null).map(t=>[t[0],t[1]]);
  if(L2){ L2.track.setLatLngs(pts);
    if(d.pos&&d.pos.lat!=null){L2.marker.setLatLng([d.pos.lat,d.pos.lon]).addTo(map);}
    if(pts.length) map.fitBounds(L2.track.getBounds().pad(0.2)); }
}

function updateTrack(imei){
  const d=devices[imei]; if(!d||!map) return;
  const L2=devLayer(imei); if(!L2) return;
  const pts=(d.track||[]).filter(t=>t[0]!=null).map(t=>[t[0],t[1]]);
  L2.track.setLatLngs(pts);
  if(d.pos&&d.pos.lat!=null) L2.marker.setLatLng([d.pos.lat,d.pos.lon]).addTo(map);
  if(imei===selected && pts.length) map.panTo([d.pos.lat,d.pos.lon]);
}

function addRaw(it){
  const box=document.getElementById("raw");
  const near=box.scrollTop+box.clientHeight>=box.scrollHeight-20;
  box.appendChild(el(`<div class="line"><span class="tt">${esc(it.t)}</span> <span class="rx">${esc(it.dir)}</span> ${it.imei?`<span class="imei">[${esc(it.imei)}]</span> `:""}${esc(it.text)}</div>`));
  while(box.childElementCount>400) box.removeChild(box.firstChild);
  if(near) box.scrollTop=box.scrollHeight;
}

function renderOta(o){
  const box=document.getElementById("ota"); if(!o) return;
  const armed=o.armed;
  const last=o.last?` · ostatni: ${esc(o.last.t)} ${esc(o.last.size)}B`:"";
  box.innerHTML=`OTA: <b style="color:${armed?'var(--ok)':'var(--dim)'}">${armed?'UZBROJONE':'wyłączone'}</b>
    <button class="ghost" id="otabtn">${armed?'rozbrój':'uzbrój'}</button> <small>${esc(o.fw||'brak obrazu')}${last}</small>`;
  document.getElementById("otabtn").onclick=()=>{
    fetch("/api/ota",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({armed:!armed})}).then(r=>r.json()).then(renderOta);
  };
}

function applySnapshot(s){
  for(const k in devices) delete devices[k];
  for(const d of s.devices){ d.track=d.track||[]; devices[d.imei]=d; }
  if(!selected && s.devices.length) selected=s.devices[0].imei;
  renderSide(); renderOta(s.ota);
  for(const im in devices) updateTrack(im);
  focusDev();
  const box=document.getElementById("raw"); box.innerHTML="";
  (s.raw||[]).forEach(addRaw);
}

function applyUpdate(m){
  const p=m.payload;
  if(m.kind==="device"){ devices[p.imei]=Object.assign(devices[p.imei]||{track:[]},p);
    if(!selected) selected=p.imei; renderSide(); }
  else if(m.kind==="pos"){ const d=devices[p.imei]||(devices[p.imei]={imei:p.imei,track:[]});
    d.pos=p.pos; if(p.pos.can) d.can=p.pos.can; d.last_seen=p.pos.t;
    if(p.pos.lat!=null){ d.track.push([p.pos.lat,p.pos.lon,p.pos.t]); if(d.track.length>500)d.track.shift(); }
    updateTrack(p.imei); renderSide(); }
  else if(m.kind==="can"){ const d=devices[p.imei]; if(d){ if(p.can)d.can=p.can; renderSide(); } }
  else if(m.kind==="raw"){ addRaw(p); }
  else if(m.kind==="ota"){ renderOta(p); }
  else if(m.kind==="shutdown"){ onShutdown(); }
}

function setStatus(on, text){
  document.getElementById("dot").classList.toggle("on", !!on);
  document.getElementById("status").textContent = text;
}
function markAllOffline(){
  for(const im in devices) devices[im].online=false;
  renderSide();
}
function scheduleRetry(sec){
  if(retryTimer) return;                 // już zaplanowane
  let left=sec;
  setStatus(false, `rozłączono — ponawiam za ${left}s`);
  retryTimer=setInterval(()=>{
    left--;
    if(left<=0){ clearInterval(retryTimer); retryTimer=null; connect(); }
    else setStatus(false, `rozłączono — ponawiam za ${left}s`);
  },1000);
}
function onShutdown(){
  markAllOffline();
  setStatus(false,"serwer się wyłącza…");
  if(es){ es.close(); es=null; }
  scheduleRetry(60);                     // zgodnie z firmware: próba co 60 s
}

function connect(){
  es=new EventSource("/api/events");
  es.addEventListener("snapshot",e=>applySnapshot(JSON.parse(e.data)));
  es.onmessage=e=>applyUpdate(JSON.parse(e.data));
  es.onopen=()=>setStatus(true,"połączono");
  es.onerror=()=>{                       // zerwanie — nie pozwól przeglądarce
    if(es){ es.close(); es=null; }       // młócić co ~3s; ponawiaj co 60s
    scheduleRetry(60);
  };
}
initMap(); connect();
</script>
</body>
</html>
"""


async def graceful_shutdown(state, servers):
    """Przy zamykaniu: powiadom klientów, że serwer schodzi.
      - urządzenia: ramka '#BYE' (firmware przechodzi w backoff i ponawia co 60 s),
      - przeglądarki (SSE): zdarzenie 'shutdown' (strona ponawia co 60 s).
    Potem domknij połączenia po swojej stronie."""
    for s in servers:                           # przestań przyjmować nowe połączenia
        s.close()
    log(f"{NN}Zamykanie — informuję klientów (#BYE / shutdown)...", BOLD, YELLOW, level=0)

    # Przeglądarki (SSE): wyślij 'shutdown' WPROST na gniazdo i domknij je.
    # (nie przez kolejkę taska w tle — proces zaraz kończy pętlę i gubiłby bufor)
    state.seq += 1
    smsg = json.dumps({"seq": state.seq, "kind": "shutdown",
                       "payload": {"t": now_iso()}}, ensure_ascii=False)
    for w in list(state.sse_writers):
        try:
            w.write(f"data: {smsg}\n\n".encode("utf-8"))
            await asyncio.wait_for(w.drain(), 2)
            w.close()
        except Exception:
            pass

    # Urządzenia: ramka '#BYE' (firmware -> backoff, ponawia co 60 s) i domknięcie.
    bye = nproto_reply("BYE").encode("latin-1")  # "#BYE*XX\r\n"
    for who, writer in list(state.conns.items()):
        try:
            writer.write(bye)
            await asyncio.wait_for(writer.drain(), 2)
            log(f"[{who}] {TX}#BYE (serwer schodzi)", YELLOW, level=0)
            writer.close()
        except Exception:
            pass

    # Poczekaj aż gniazda faktycznie się domkną (flush + FIN), z limitem czasu.
    for w in list(state.sse_writers) + list(state.conns.values()):
        try:
            await asyncio.wait_for(w.wait_closed(), 2)
        except Exception:
            pass
    log(f"{NN}Serwer zatrzymany.", BOLD, GREEN, level=0)


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
    ap.add_argument("--fw", default=None,
                    help="plik obrazu firmware serwowany bootloaderowi na 'FWREQ' "
                         "(np. ../build/tracker.bin) — zdalna aktualizacja OTA")
    ap.add_argument("--web-port", type=int, default=8080,
                    help="port interfejsu WWW (0 = wyłącz)")
    ap.add_argument("--web-listen", default="0.0.0.0",
                    help="adres nasłuchu WWW (na routerze ogranicz do LAN!)")
    ap.add_argument("--max-track", type=int, default=500,
                    help="ile ostatnich punktów trasy trzymać na urządzenie (RAM)")
    ap.add_argument("--max-raw", type=int, default=300,
                    help="ile ostatnich linii surowego logu trzymać (RAM)")
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

    state = State(max_track=args.max_track, max_raw=args.max_raw)
    state.ota_fw = args.fw
    state.ota_armed = bool(args.fw)                     # z --fw startuje uzbrojone

    server = await asyncio.start_server(lambda r, w: client(r, w, args, state),
                                        args.listen, args.port)
    log(f"Nasłuch na {args.listen}:{args.port}  (HSK odsyła {args.device_host}:{args.device_port})", BOLD, GREEN, level=0)

    web = None
    if args.web_port:
        web = await asyncio.start_server(lambda r, w: http_client(r, w, state, args),
                                         args.web_listen, args.web_port)
        log(f"WWW na http://{args.web_listen}:{args.web_port}  (mapa + log na żywo)", BOLD, GREEN, level=0)
        if args.web_listen in ("0.0.0.0", "::"):
            log("  UWAGA: WWW słucha na wszystkich interfejsach — na routerze ogranicz w firewallu do LAN.", YELLOW, level=0)
    if LOG_FILE:
        log(f"Log: {LOG_FILE}", DIM, level=0)

    servers = [server] + ([web] if web else [])

    # Graceful shutdown na SIGINT/SIGTERM (np. zatrzymanie usługi na OpenWRT):
    # ustaw zdarzenie, wyślij #BYE do urządzeń i 'shutdown' do przeglądarek.
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, stop.set)
        have_signals = True
    except NotImplementedError:                 # np. Windows — fallback niżej
        have_signals = False

    serve_tasks = [asyncio.ensure_future(s.serve_forever()) for s in servers]
    try:
        if have_signals:
            await stop.wait()
        else:
            await asyncio.gather(*serve_tasks)   # Ctrl+C złapie KeyboardInterrupt
    finally:
        await graceful_shutdown(state, servers)
        for t in serve_tasks:
            t.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Zatrzymano przez Ctrl+C", YELLOW, level=0)
    except ValueError as e:
        sys.exit(f"Błąd: {e}")
