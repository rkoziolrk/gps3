#!/usr/bin/env python3
"""
Klient testowy: udaje lokalizator i sprawdza odpowiedzi serwera regułami
odtworzonymi z firmware (docs/SERVER.md).

  python3 test_client.py --server 127.0.0.1 --port 3506 --device-host example.entrydns.org

Dane są zmyślone: IMEI z samych zer, trasa w centrum Warszawy.
Układ rekordu pozycji jest prawdopodobny (docs/SERVER.md, "Ramka LOK").
"""

import argparse
import socket
import sys
import time

IMEI = "000000000000000"


# --- reguły firmware ---------------------------------------------------------
def xor_sum(text):
    x = 0
    for ch in text.encode("latin-1"):
        x ^= ch
    return x


def capture(buf):
    """Odbiór jak w przerwaniu USART1 (tryb 0x1F): pomiń do '{', zbierz do '}'."""
    i = buf.find("{")
    j = buf.find("}", i + 1)
    return None if i < 0 or j < 0 else buf[i + 1:j]


def check_hsk(r, host, port):
    if r is None:
        raise AssertionError("brak klamer -> firmware czeka 30 s i przyjmuje 'ER'")
    if len(r) != 38:
        raise AssertionError(f"długość {len(r)} zamiast 38")
    if r[34] != "*":
        raise AssertionError("brak '*' na pozycji 35")
    if xor_sum(r[:34]) != int(r[-3:]):
        raise AssertionError("zła suma kontrolna HSK")
    w = r[:34] + "$"                    # firmware dokleja '$'
    if w[:3] != "HSK":
        raise AssertionError("nie zaczyna się od HSK")
    got_port, got_host = w[4:8], w[9:w.index("$", 9)]
    if (got_port, got_host) != (port, host):
        raise AssertionError(f"inny adres {got_host}:{got_port} -> urządzenie zapisze go i zrestartuje się")
    return "- ADRESY OK -"


def check_cfg(r):
    if r is None:
        raise AssertionError("brak klamer -> -CONFIG-ERROR, restart (kod 14)")
    if len(r) < 81:
        raise AssertionError(f"za krótka: {len(r)} znaków")
    if xor_sum(r[:77]) != int(r[78:81]):
        raise AssertionError("zła suma kontrolna -> -CONFIG-ERROR, restart (kod 14)")
    return f"suma OK, sesja co {int(r[5:10])} s, rekordów w LOK: {r[17]}, tryb roamingu: {r[24]}"


def check_ok(r, what):
    if r != "OK":
        raise AssertionError(f"{what}: oczekiwano {{OK}}, jest {r!r}")
    return "OK"


# --- dane przykładowe --------------------------------------------------------
def nmea_coord(deg, width_deg):
    """52.230000 -> '52138000' (ddmm.mmmm bez kropki)."""
    d = int(deg)
    m = round((deg - d) * 60 * 10000)
    return f"{d:0{width_deg}d}{m:06d}"


def record(lat, lon, alt, sats, kmh, utc, date):
    """Rekord pozycji w prawdopodobnym układzie firmware + zmyślona reszta."""
    return (nmea_coord(lon, 3) + "E" + nmea_coord(lat, 2) + "N"
            + f"{alt:04d}{sats:02d}{kmh:03d}" + utc + date
            + "00000!!000")             # reszta: licznik, liczniki, CAN — zmyślone


ROUTE = [  # (lat, lon, km/h, czas UTC) — przejazd i postój, centrum Warszawy
    (52.230000, 21.011667, 0,  "120000"),
    (52.231500, 21.013000, 32, "120030"),
    (52.233200, 21.014800, 48, "120100"),
    (52.235000, 21.016500, 51, "120130"),
    (52.236100, 21.017300, 12, "120200"),
    (52.236200, 21.017400, 0,  "120230"),
]


# --- sesja -------------------------------------------------------------------
class Device:
    def __init__(self, server, port):
        self.s = socket.create_connection((server, port), timeout=10)

    def send(self, data, chunks=1):
        raw = data.encode("latin-1")
        step = max(1, len(raw) // chunks)
        for i in range(0, len(raw), step):
            self.s.sendall(raw[i:i + step])
            time.sleep(0.05)

    def reply(self):
        buf = ""
        while "}" not in buf:
            part = self.s.recv(4096)
            if not part:
                break
            buf += part.decode("latin-1")
        return buf

    def close(self):
        self.s.close()


def step(n, title):
    print(f"\n--- krok {n}: {title}")


def run_session(args):
    dev = Device(args.server, args.port)

    step(1, "HSK-02 (wysłane w 2 kawałkach)")
    dev.send(f"HSK-02;{IMEI};2.65;ORANGE;MT02-51083;SIM900;FGP;260;03;0;0;PA6C;1;TEMP;R26;B13\r\n", chunks=2)
    r = capture(dev.reply())
    print("    odpowiedź:", r)
    print("    firmware:", check_hsk(r, args.device_host, args.device_port))

    step(2, "CONFIG")
    dev.send(f"CONFIG;{IMEI};05\r\n")
    r = capture(dev.reply())
    print("    odpowiedź:", r)
    print("    firmware:", check_cfg(r))
    per_frame = int(r[17]) or 6

    step(3, f"jazda: {len(ROUTE)} punktów, ramka LOK co {per_frame} rekordy")
    recs = [record(lat, lon, 110, 7, kmh, utc, "010126") for lat, lon, kmh, utc in ROUTE]
    for i in range(0, len(recs), per_frame):
        batch = recs[i:i + per_frame]
        frame = f"LOK-02;{IMEI}$" + "".join(x + "$" for x in batch) + "\r\n"
        print(f"    wysyłam LOK z {len(batch)} rekordami: {frame.strip()}")
        dev.send(frame)
        print("    firmware:", check_ok(capture(dev.reply()), "LOK"), "-> rekordy skasowane jako wysłane")

    step(4, "PNG-01 (stan) i LOGUJ (komunikat) w jednym pakiecie")
    dev.send(f"PNG-01;{IMEI};CAN;25;FORM;Er=000/000;A=1 CAN=1 R=3/3 DP=0 OD=0 DS=01 "
             f"MEM=0:0 SD=1 SN=0WY=1 GG=1\r\nLOGUJ-;{IMEI};POSTOJ@\r\n")
    buf = dev.reply()
    while buf.count("}") < 2:
        buf += dev.s.recv(4096).decode("latin-1")
    print("    odpowiedzi:", buf, "->", check_ok(capture(buf), "PNG"))

    dev.close()
    print("\nsesja zakończona — połączenie zamknęło urządzenie, nie serwer")


def run_negative(args):
    print("\n=== test odwrotny: czy reguły odrzucają złe odpowiedzi ===")
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    import tracker_server as srv
    good = srv.hsk_reply(args.device_host, args.device_port)
    cases = [
        ("odpowiedź 'OK\\r\\n' (bez klamer)", "OK\r\n", "hsk"),
        ("poprawna odpowiedź HSK", good, "hsk"),
        ("HSK z jednym zmienionym znakiem", good[:12] + chr(ord(good[12]) ^ 0x01) + good[13:], "hsk"),
        # Dwa takie same przekłamania bitu znoszą się w sumie XOR, a separatorów
        # (poz. 4 i 9) firmware nie sprawdza — urządzenie też by to przyjęło.
        ("HSK: oba separatory ';'->':' (XOR*)", good.replace(";", ":", 2), "hsk-ok"),
        ("HSK z innym hostem", srv.hsk_reply("inny.host.pl", args.device_port), "hsk"),
        ("HSK krótsza o 1 znak", good[:-2] + "}", "hsk"),
        ("CONFIG z popsutą sumą", srv.config_reply()[:-4] + "000}", "cfg"),
    ]
    failed = 0
    for name, reply, kind in cases:
        r = capture(reply)
        try:
            res = check_hsk(r, args.device_host, args.device_port) if kind.startswith("hsk") else check_cfg(r)
            print(f"  PRZESZŁO    {name:36} -> {res}")
            failed += name.startswith(("odpowiedź 'OK", "HSK z", "HSK k", "CONFIG z"))
        except AssertionError as e:
            print(f"  ODRZUCONO   {name:36} -> {e}")
    print("  (*) słabość sumy XOR w firmware, nie błąd serwera")
    return failed


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3506)
    ap.add_argument("--device-host", required=True)
    ap.add_argument("--device-port", default="3506")
    a = ap.parse_args()
    try:
        run_session(a)
    except AssertionError as e:
        sys.exit(f"\nBŁĄD: {e}")
    if run_negative(a):
        sys.exit("\nBŁĄD: reguła przepuściła złą odpowiedź")
    print("\nwszystkie sprawdzenia zgodne z oczekiwaniami")
