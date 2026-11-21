# Serwer — co musi robić

Wymagania odtworzone z firmware. Gotowa implementacja: `server/tracker_server.py`.

## Transport

- Jedna sesja TCP na raz, kilka ramek w jednej sesji (najpierw `HSK-02`,
  potem `CONFIG`, potem dane). Nie zamykaj połączenia po pierwszej ramce.
- Każda ramka od urządzenia kończy się `\r\n` (@0x3f8e dokleja CR, LF i
  Ctrl-Z; Ctrl-Z modem zjada). Strumień TCP trzeba dzielić na linie — jeden
  `recv()` może zawierać część ramki albo kilka ramek.
- **Odpowiedź musi być w klamrach: `{...}`.** Firmware (przerwanie USART1,
  tryb `0x1F`) ignoruje wszystko przed `{`, zapisuje znaki do `}` (najwyżej
  198) i na tym kończy. Bez `}` czeka 30 s (150 × 200 ms) i przyjmuje `ER`.

## Ramki i odpowiedzi

| ramka od urządzenia | odpowiedź serwera | gdy odpowiedź zła / brak |
|---|---|---|
| `HSK-02;<IMEI>;2.65;ORANGE;...` | `{HSK;<port>;<host>*<xor>}` (38 znaków w klamrach) | `-HSK-ERROR` / `HSK PROBLEM`, restart |
| `CONFIG;<IMEI>;05` | 81 znaków w klamrach, suma XOR na poz. 79–81 | `-CONFIG-ERROR`, restart (kod 14) |
| `PNG-01;<IMEI>;CAN;<n>;FORM;Er=...` | `{OK}` | kody restartu nie są zerowane |
| `LOK-02;<IMEI>$<rekord>$...` | `{OK}` | rekordy zostają; brak odpowiedzi → restart (kod 9) |
| `LOGUJ-;<IMEI>;<tekst>@`, `AUTO--;...@` | dowolna (`{OK}`) | firmware nie sprawdza |

### Odpowiedź na HSK

```
poz.  1-3   HSK
poz.  4     separator (dowolny)
poz.  5-8   port (4 cyfry)
poz.  9     separator (dowolny)
poz. 10-34  host; krótszy niż 25 znaków kończy się '$' (reszta dowolna)
poz. 35     *
poz. 36-38  XOR znaków 1-34, 3 cyfry
```

Jeśli host i port są **takie same jak w EEPROM** urządzenia → `- ADRESY OK -`
i praca dalej. Jeśli **inne** → urządzenie zapisuje nowy adres
(`- NOWE ADRESY -`, `Zapisano w pamieci`) i się restartuje — tak serwer może
przekierować urządzenie. Host dłuższy niż 25 znaków nie mieści się w odpowiedzi.

### Odpowiedź na CONFIG

Mapa pól: `docs/PROTOCOL.md`. Znane znaczenia:

| poz. | szer. | znaczenie | domyślnie w serwerze |
|---|---|---|---|
| 6 | 5 | czas do następnej sesji [s] (najpewniej) | `00060` |
| 12 | 5 | czas do następnego pobrania CONFIG [s] (najpewniej) | `03600` |
| 18 | 1 | liczba rekordów w ramce `LOK` (`0` → 6) | `2` |
| 25 | 1 | tryb roamingu 0–7 | `3` |
| 32 | 4 | pole tekstowe (`0000` = puste) | `0000` |

Pozostałe pola mają nieustalone znaczenie; serwer wysyła wartości, które
firmware ustawia sam przy starcie (`22`=06, `37`=018, `41`=09, `49`=45,
`59`=4), a resztę jako zera. Pozycje 6 i 12 to minutniki zmniejszane co
sekundę przez zegar (przerwanie TIMER1_OVF).

## Ramka LOK

`LOK-02;<IMEI>$`, potem do `n` rekordów (n z poz. 18 CONFIG), każdy
zakończony `$`. Rekord składa funkcja `0x210e` z liczb, pól z parsera NMEA
i danych CAN (część pól rozdzielona `!`). Dokładny układ pól rekordu nie jest
jeszcze rozpisany — serwer zapisuje rekordy w postaci surowej.
