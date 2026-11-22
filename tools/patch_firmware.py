#!/usr/bin/env python3
"""
Modyfikacja firmware lokalizatora (ATmega64, obraz flash 64 KiB):

  1. domyślny adres serwera  'lgps01.monitoring-gps.net' (0xbbe0) -> --host
  2. opcjonalnie domyślny port '3506' (0xbbda)                   -> --port
  3. zdjęcie blokady adresu producenta (funkcja 0x7b4c, przy każdym starcie):
       0x7ba8  brne .+4  ->  rjmp 0x7cb6   host nie jest porównywany z
                                           'lgpsNN.monitoring-gps.net'
       0x7d62  brne .+4  ->  rjmp .+4      port nie musi zaczynać się od '35'
                                           (kontrola długości = 4 zostaje)

Przed każdą zmianą skrypt sprawdza, czy w pliku są oryginalne bajty — inny
obraz (inna wersja firmware) zostanie odrzucony.

  python3 patch_firmware.py flash.bin flash_patched.bin --host example.entrydns.org
  python3 patch_firmware.py flash.bin flash_patched.bin --host example.org --hex flash_patched.hex
"""

import argparse
import sys

HOST_ADDR, HOST_ORIG, HOST_SPACE = 0xBBE0, b"lgps01.monitoring-gps.net\x00", 26
PORT_ADDR, PORT_ORIG = 0xBBDA, b"3506\x00"

CODE_PATCHES = [
    # (adres, oryginał, nowe bajty, opis)
    (0x7BA8, bytes.fromhex("11f4"), bytes.fromhex("86c0"),
     "0x7ba8: brne .+4 -> rjmp 0x7cb6 (host bez kontroli domeny producenta)"),
    (0x7D62, bytes.fromhex("11f4"), bytes.fromhex("02c0"),
     "0x7d62: brne .+4 -> rjmp .+4    (port bez wymogu prefiksu '35')"),
]
FORBIDDEN = set("${};\"*")


def check(img, addr, orig, what):
    if img[addr:addr + len(orig)] != orig:
        sys.exit(f"BŁĄD: pod 0x{addr:04x} nie ma oryginalnych bajtów ({what}). "
                 f"Jest: {img[addr:addr + len(orig)].hex()} — inny firmware?")


def ihex(data):
    """Intel HEX (rekordy po 16 B, puste 0xFF pomijane)."""
    out = []
    end = len(data.rstrip(b"\xff"))
    for a in range(0, end, 16):
        chunk = data[a:a + 16]
        if chunk == b"\xff" * len(chunk):
            continue
        rec = bytes([len(chunk), a >> 8, a & 0xFF, 0]) + chunk
        out.append(":" + rec.hex().upper() + f"{(-sum(rec)) & 0xFF:02X}")
    out.append(":00000001FF")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--host", required=True, help="nowy domyślny host (najwyżej 25 znaków)")
    ap.add_argument("--port", help="nowy domyślny port (4 cyfry); bez tej opcji zostaje 3506")
    ap.add_argument("--hex", help="dodatkowo zapisz Intel HEX")
    ap.add_argument("--keep-lock", action="store_true", help="tylko zmiana adresu, bez zdejmowania blokady")
    a = ap.parse_args()

    host = a.host.strip()
    if not 1 <= len(host) <= 25:
        sys.exit("BŁĄD: host musi mieć od 1 do 25 znaków (tyle mieści się w odpowiedzi na HSK)")
    if not all(33 <= ord(c) < 127 for c in host) or FORBIDDEN & set(host):
        sys.exit("BŁĄD: host może zawierać tylko drukowalne znaki ASCII bez ${};\"*")
    if a.port is not None and not (len(a.port) == 4 and a.port.isdigit()):
        sys.exit("BŁĄD: port musi mieć dokładnie 4 cyfry")

    img = bytearray(open(a.src, "rb").read())
    if len(img) != 65536:
        sys.exit(f"BŁĄD: obraz ma {len(img)} B, oczekiwano 65536 (ATmega64)")

    check(img, HOST_ADDR, HOST_ORIG, "domyślny host")
    check(img, PORT_ADDR, PORT_ORIG, "domyślny port")
    for addr, orig, _, desc in CODE_PATCHES:
        check(img, addr, orig, desc)

    img[HOST_ADDR:HOST_ADDR + HOST_SPACE] = host.encode().ljust(HOST_SPACE, b"\x00")
    print(f"0x{HOST_ADDR:04x}: domyślny host -> {host!r}")
    if a.port:
        img[PORT_ADDR:PORT_ADDR + 5] = a.port.encode() + b"\x00"
        print(f"0x{PORT_ADDR:04x}: domyślny port -> {a.port!r}")
    if not a.keep_lock:
        for addr, orig, new, desc in CODE_PATCHES:
            img[addr:addr + len(new)] = new
            print(desc)

    open(a.dst, "wb").write(img)
    print(f"zapisano {a.dst}")
    if a.hex:
        open(a.hex, "w").write(ihex(bytes(img)))
        print(f"zapisano {a.hex}")


if __name__ == "__main__":
    main()
