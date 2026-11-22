# Protokół i interfejsy

Odtworzone z tablicy stringów obrazu (`reverse/flash.bin`). Offsety w nawiasach
to adresy w obrazie flash.

## Ramki serwera (`*.monitoring-gps.net`)

Protokół tekstowy, znaki `;` jako separatory. Rodzaje ramek:

| Ramka       | Offset  | Znaczenie                                             |
|-------------|---------|-------------------------------------------------------|
| `PNG-01;`   | 0xbc14  | ping / heartbeat podtrzymujący sesję                  |
| `FORM;`     | 0xbc24  | pełny rekord pozycji (format danych)                  |
| `HSK-02;`   | 0xbcde  | handshake — zawiera wersję/operatora/typ modułów      |
| `LOK-02;`   | 0xbcca  | bieżąca lokalizacja                                   |
| `LOO-02;`   | 0xbe1c  | rekordy zaległe z pamięci (`LOO-BE`…`LOO-EN`)          |
| `CONFIG;`   | 0xbd9c  | pobranie / ustawienie konfiguracji                    |
| `CAN=1`     | 0xbc34  | sekcja danych CAN                                     |

Pola stanu spotykane w ramkach: `A=1` (aktywny), `R=3/`, `DP=0`, `OD=0`,
`DS=01`, `MEM=`, `SD=1 SN=0`, `WY=1`, `GG=1`, `LOGUJ-;`, `AUTO--;`.

### Ramki potwierdzone logiem z urządzenia

```
HSK-02;<IMEI>;2.65;ORANGE;MT02-51083;SIM900;FGP;260;03;0;0;PA6C;1;TEMP;R26;B13
PNG-01;<IMEI>;CAN;25;FORM;Er=012/016;A=1 CAN=1 R=3/2 DP=0 OD=0 DS=01 MEM=0:0 SD=1 SN=0WY=1 GG=1
```

- `HSK-02` — handshake: IMEI, wersja `2.65`, operator, model `MT02-51083`,
  modem, `FGP`, MCC;MNC sieci (`260;03` = Orange PL), typ GPS `PA6C`.
  `B13` to wersja oprogramowania modemu: `Mid(odpowiedź AT+CGMR, 14, 3)`
  (`Revision:1137B13SIM900M64_ST`, wywołanie przed ramką HSK @0x2d7e).
  Pola `0;0`, `1;TEMP` i `R26` — znaczenie nieustalone.
- `PNG-01` — ramka okresowa (@0x2372..0x24fe): IMEI, `CAN;<licznik>`
  (RAM 0x0BE9), potem część `FORM;`: `Er=<kod PL>/<kod inne>` (kody przyczyn
  restartu, `docs/EEPROM.md`), `R=3/<tryb roamingu>`, `MEM=<bank>:<adr>`
  oraz stałe pola stanu. Brak spacji w `SN=0WY=1` jest zgodny z firmware.

## Połączenie TCP (SIM900)

Sekwencja z logu:

```
AT+CSTT="internet","internet","internet"   -> OK
AT+CIICR                                   -> OK
AT+CIFSR                                   -> <adres IP z sieci operatora>
AT+CIPHEAD=1                               -> OK
AT+CIPSTART="TCP","<host>","<port>"        -> OK   (przyjęcie komendy)
                                              CONNECT OK / CONNECT FAIL  (wynik, przychodzi później)
AT+CIPSEND                                 -> ">"  (gdy połączenie jest) albo ERROR
```

`OK` po `AT+CIPSTART` oznacza tylko przyjęcie komendy; o nawiązaniu
połączenia świadczy dopiero `CONNECT OK`. Firmware wysyła `AT+CIPSEND`
ok. 9 s po `CIPSTART` i ponawia 3 razy co ok. 2,5 s. Z `AT+CIPHEAD=1`
dane przychodzące od serwera modem poprzedza nagłówkiem `+IPD,<długość>:`.

## Komendy AT (SIM900)

Sekwencja z offsetów 0xbe52..0xc21e:

```
ATE0                 echo off
AT+IPR=9600          stały baud
AT+CSQ               jakość sygnału  -> csq
AT+GSN               IMEI            -> "IMEI>"
AT+CGMR              wersja firmware modemu ("ModemVer=")
AT+CREG=2            rejestracja + info o komórce
AT+CENG=1 / AT+CENG? tryb engineering (LAC/CID)
AT+CIPMUX=0          pojedyncze połączenie
AT+CGATT=1           podłączenie do GPRS
AT+CSTT="internet"   APN
AT+CIICR             podniesienie łącza
AT+CIFSR             lokalny IP
AT+CIPHEAD=1         nagłówek w odbieranych danych
AT+CIPSTART=TCP,<host>,<port>   otwarcie TCP (host+port z EEPROM, domyślnie lgps01...:3506)
AT+CIPSEND           wysyłka (kończona Ctrl-Z, 0x1A)
AT+COPS=1,2,/4,2,    wybór operatora (kontrola roamingu)
```

Obsługa SMS: `AT+CMGF=1`, `AT+CMGR=1`, `AT+CMGD=` (kasowanie).

## Komendy SMS (zdalne sterowanie)

Odczyt `@0x80ac`, wykonanie `@0x84dc`, kasowanie `@0x883a` (`AT+CMGD=<n>,3`).

| Treść SMS | Działanie |
|---|---|
| `NEW*ADR=<host>$<port>$` | zapis hosta i portu w EEPROM, tryb roamingu 3, skasowanie SMS, restart (kod 28) |
| `RESET*<PIN>` / `RESET-<PIN>` | ustawienia fabryczne (`lgps01.monitoring-gps.net:3506`, tryb 3), skasowanie SMS, `KOMENDA RESET`, restart (kod 27) |
| inny tekst | przy aktywnej sesji wysyłany na serwer jako `LOGUJ-;<IMEI>;<tekst>@` |

- **PIN** = 4 ostatnie cyfry IMEI (`Mid(IMEI,12,4)`, na LCD `PIN>`).
  `NEW*ADR=` PIN-u nie wymaga.
- Przykład: `NEW*ADR=example.entrydns.org$3506$` — host najwyżej 35 znaków,
  port dokładnie 4 cyfry, **oba zakończone `$`**.
- Odczytywana jest **tylko pozycja 1** na karcie SIM (`AT+CMGR=1`). Dopóki
  SMS z pozycji 1 nie zostanie skasowany, kolejne SMS-y nie są czytane.
- **Uwaga — błąd w firmware:** brak kontroli długości. SMS `NEW*ADR=` bez `$`
  (albo z za długim hostem/portem) przepełnia bufory (host 36 B, port 5 B).
  Urządzenie resetuje się, zanim skasuje SMS, i przy każdym starcie czyta go
  od nowa — pętla restartów bez połączenia z serwerem. Wyjście: skasować SMS
  z karty SIM (np. w telefonie).

Po skasowaniu na LCD: `SMS IS CLEAR`. Brak wiadomości: `[Brak SMS]`.

## Odpowiedź `CONFIG` — format stałopozycyjny

Serwer odsyła konfigurację jako napis, w którym każdy parametr siedzi pod
**stałą pozycją znakową** o znanej szerokości (bez separatorów). Pozycje są
liczone **od 1**. Pola wycina procedura `0xb7ee`, liczby zamienia `@0xb714`.
Funkcja parsera: `@0x33b6`.

| poz. | szer. | cel      | uwagi                                              |
|------|-------|----------|----------------------------------------------------|
| 6    | 5     | 0x0242   | liczba 32-bit                                      |
| 12   | 5     | 0x023E   | liczba 32-bit                                      |
| 18   | 1     | 0x0257   | licznik cykli do raportu; `0` → `6`                |
| 20   | 1     | 0x0B5E   |                                                    |
| 22   | 2     | 0x0268   |                                                    |
| 25   | 1     | —        | **tryb roamingu** → EEPROM `0x64` (`docs/ROAMING.md`) |
| 27   | 2     | 0x0B63   | dodatkowo przeskalowany mnożeniem                  |
| 30   | 1     | 0x0C17   |                                                    |
| 32   | 4     | 0x0BEB   | tekst; `0000` → pusty. Użycie nieustalone          |
| 37   | 3     | 0x025B   |                                                    |
| 41   | 2     | 0x025C   |                                                    |
| 44   | 2     | 0x0B76   |                                                    |
| 47   | 1     | 0x0B77   |                                                    |
| 49   | 2     | 0x025E   |                                                    |
| 52   | 1     | 0x0C14   |                                                    |
| 54   | 3     | 0x0C15   |                                                    |
| 59   | 1     | 0x0269   |                                                    |
| 60   | 1     | 0x0B86   |                                                    |
| 61   | 1     | 0x0B87   |                                                    |
| 62   | 1     | 0x0C22   |                                                    |
| 63   | 1     | 0x0C23   |                                                    |
| 64   | 1     | 0x0C24   |                                                    |
| 79   | 3     | —        | **suma kontrolna**: XOR znaków na pozycjach 1–77   |

**Suma kontrolna** (`@0x37aa..0x37ca`): firmware liczy XOR pierwszych 77
znaków odpowiedzi i porównuje z 3-cyfrową liczbą z pozycji 79–81.
- zgodna → tryb roamingu zapisany do EEPROM, na LCD `RoamingPam=<n>`,
- niezgodna → `-CONFIG-ERROR <xor>` i restart z kodem 14.

Pozostałe pola trafiają do RAM jeszcze przed sprawdzeniem sumy. Pozycje
1–5, 11, 17, 19, 21, 24, 26, 29, 31, 36, 40, 43, 46, 48, 51, 53, 57–58,
65–78 nie są czytane (najpewniej separatory, nagłówek lub pola nieużywane).

## GPS — NMEA / PMTK

Parsowane zdania: `$GPRMC`, `$GPGGA`, `$GPVTG`. Błędy: ` - RMC ERROR - `,
` - GGA ERROR - `, ` - VTG ERROR - `. Pola: UTC, LAT, LON, ALT, SAT, SPEED,
COURSE, DELTA; detekcja ruchu `RUCH` / `POSTOJ`.

Konfiguracja modułu PA6C (offsety 0xbeaa..0xbf4c):

```
$PMTK314,0,1,0,0,...*29    wybór zdań NMEA (tu: RMC)
$PMTK101*32                hot start
$PMTK104*37                full cold reset
```

## CAN

Stringi: `CAN=` (0xbcac), `BRAK CAN` (0xc3d4), `RAMKA CAN ERROR` (0xc3de),
`Paliwo=`, `RPM=`, `Pedal=`, `SONDA1=`, `W_can=`, `WYGLADZ= J=` (uśrednianie
poziomu paliwa), `W_sonda1=`.
