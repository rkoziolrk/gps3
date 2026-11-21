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

### Budowa ramki `FORM;` (sekwencja @0x23cc..0x25e6)

Sklejana po kolei (`strcpy_P`/`strcat` + `itoa`):

```
FORM; Er=<status> ;A=1 CAN=1 R=3/<n>  DP=0 OD=0 DS=01 MEM=<bank>:<adr>
 SD=1 SN=0 WY=1 GG=1 LOGUJ-;<dane_pozycji>;@
```

Dynamiczne pola: `R=3/<n>` (licznik rekordu, wartość z @0x1dfe),
`MEM=<bank>:<adr>` (stan pamięci EEPROM), oraz treść po `LOGUJ-;`
(sparsowane pola GPS z bufora 0x0B42). Pozostałe (`DP/OD/DS/SD/SN/WY/GG`)
w tym buildzie są w większości stałe.

Handshake (`HSK-02;`) dokleja m.in.: `;2.65;ORANGE;` (wersja + operator),
`;SIM900;FGP`, `;PA6C`, `;1;TEMP;`. Odpowiedzi serwera:
`- ADRESY OK -`, `- NOWE ADRESY -`, `- HSK PROBLEM -5s -`, `-CONFIG-ERROR`.

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

| Treść SMS   | Offset | Działanie                                   |
|-------------|--------|---------------------------------------------|
| `RESET*`    | 0xc318 | restart lokalizatora (`KOMENDA RESET`)      |
| `RESET-`    | 0xc320 | wariant resetu                              |
| `NEW*ADR=`  | 0xc352 | zmiana adresu serwera + zapis do EEPROM     |

Po przetworzeniu: `SMS IS CLEAR`. Brak wiadomości: `[Brak SMS]`.

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
