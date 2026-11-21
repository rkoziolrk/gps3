# Układ EEPROM (zweryfikowany ze zrzutu)

Źródło: `reverse/eeprom.bin` — realny zrzut 2 KiB z ATmega64. Potwierdza, że
EEPROM to **magazyn konfiguracji**, a nie kolejka rekordów. Dostęp przez
sprzętowy EEPROM AVR (prymitywy @0xb80c / @0xb818 / @0xb83a → `<avr/eeprom.h>`).

Poza obszarem konfiguracji (0x00–0x6A) cała pamięć = `0xFF` (skasowana).

| offset | dług. | zawartość w zrzucie           | znaczenie                      |
|--------|-------|-------------------------------|--------------------------------|
| 0x01   | 5     | `3506`                        | **port TCP serwera**           |
| 0x0A   | 26    | `lgps03.monitoring-gps.net`   | adres serwera (host)           |
| 0x2F   | 1     | `1`                           | flaga (tryb / aktywny)         |
| 0x3C   | 3     | `52 6e 3c` (bin)              | parametr binarny (3 bajty)     |
| 0x42   | 3     | `021`                         | parametr numeryczny            |
| 0x5A   | 6     | `000001`                      | licznik sesji / rekordu        |
| 0x64   | 1     | `3`                           | flaga                          |
| 0x67   | 3     | `020`                         | parametr numeryczny            |

Struktura `struct eeconfig` w `src/tracker.c` odwzorowuje te offsety 1:1
(sprawdzane `_Static_assert` przy kompilacji). Host i port tworzą razem adres
serwera: `lgps03.monitoring-gps.net:3506`. Wartości domyślne (flash 0xbbda):
host `lgps01.monitoring-gps.net`, port `3506`.

## Walidacja przy starcie (@0x7c80..0x7d40)

| warunek                        | komunikat LCD          | reakcja              |
|--------------------------------|------------------------|----------------------|
| host pusty / skasowany         | `ADRES MEM ERROR`      | host = `lgps01...`   |
| długość portu ≠ 4              | `PORT MEM LEN ERROR`   | port = `3506`        |
| `atoi(port) == 0`              | `PORT MEM VAL ERROR`   | port = `3506`        |
| poprawne                       | `ADRES MEM OK` / `PORT MEM OK` | —            |

Potem `AT+CIPSTART=TCP,<host>,<port>` (@0x3d34).

## Obserwacje

- Serwer skonfigurowany to `lgps03` (flashowy default był `lgps01`) — czyli
  urządzenie przeszło konfigurację/HSK i zapisało nowy host.
- Licznik `000001` — niski, urządzenie krótko po uruchomieniu/kasacji.
- Brak rekordów pozycji w EEPROM → kolejka „OLD" była pusta albo rekordy są
  buforowane w RAM (ulotnie). W kodzie modelujemy ją w RAM.
