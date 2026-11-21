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
**stałym offsetem znakowym** o znanej szerokości (nie ma separatorów). Firmware
wycina pola funkcją `get_field(src, off, width)` (@0xb7ee) i konwertuje `atoi`
(@0xb714). Odtworzona mapa (funkcja parsera @0x33b6):

| # | offset | szer. | RAM     | uwagi                                   |
|---|--------|-------|---------|-----------------------------------------|
| 0 | 6      | 5     | 0x0242  | wartość 32-bit (interwał/licznik)       |
| 1 | 12     | 5     | 0x023E  | wartość 32-bit (interwał/licznik)       |
| 2 | 18     | 1     | 0x0257  | gdy `0` → domyślnie `6`                  |
| 3 | 20     | 1     | 0x0B5E  |                                         |
| 4 | 22     | 2     | 0x0268  |                                         |
| 5 | 25     | 1     | 0x0268+ |                                         |
| 6 | 27     | 2     | 0x0B63  | dodatkowo skalowany (`__mulhi3`)        |
| 7 | 30     | 1     | 0x0C17  |                                         |
| – | 33     | 2     | flaga   | `strcmp(pole, ";05")` → blokada roaming |
| 8 | 37     | 3     | 0x025B  |                                         |
| 9 | 41     | 2     | 0x025C  |                                         |
|10 | 44     | 2     | 0x0B76  |                                         |
|11 | 47     | 1     | 0x0B77  |                                         |
|12 | 49     | 2     | 0x025E  |                                         |
|13 | 52     | 1     | 0x0C14  |                                         |
|14 | 54     | 3     | 0x0C15  |                                         |
|15 | 59     | 1     | 0x0269  |                                         |
|16 | 60     | 1     | 0x0B86  |                                         |
|17 | 61     | 1     | 0x0B87  |                                         |
|18 | 62     | 1     | 0x0C22  |                                         |
|19 | 63     | 1     | 0x0C23  |                                         |

Błąd formatu → `-CONFIG-ERROR `. Powiązany parametr: `RoamingPam=` (0xbdae).

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
