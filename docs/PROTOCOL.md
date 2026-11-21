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
AT+CIPSTART=...      otwarcie TCP do serwera
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
