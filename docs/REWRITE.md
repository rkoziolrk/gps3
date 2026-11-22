# Przepisanie firmware — decyzje projektowe

Dokument zbiera ustalenia do napisania firmware od nowa (czysty C, avr-gcc),
w miejsce odtworzonego z obrazu BASCOM-a. **Status: wstrzymane** — to notatki
projektowe, nie plan wykonawczy. Kodu nie piszemy, dopóki rewrite nie zostanie
odwieszony.

## Platforma (fakty sprzętowe)

- ATmega64 @ 8 MHz, architektura avr5.
- **Jeden UART na komunikację**: GPS (GlobalTop PA6C) i modem (SIM900) wiszą na
  wspólnej magistrali USART1, demultipleksowane po prefiksie linii
  (`$…` = NMEA z GPS, reszta = odpowiedzi AT z modemu).
- **USART0 wolny** → przeznaczony na debug/log (patrz niżej, zamiast LCD).
- **LCD nie jest montowany** na płytce docelowej. W odtworzonym firmware HD44780
  4-bit wisi na PORTC, ale fizycznie go nie ma — cała obsługa LCD do usunięcia.
- CAN: ATmega64 nie ma kontrolera CAN; dane przychodzą **już zdekodowane,
  tekstowo, z zewnętrznego modułu** na współdzielonej linii USART1 (szczegóły i
  dowód w `docs/CAN.md`). Firmware nie dekoduje surowego CAN auta.

## Decyzje

| Obszar | Decyzja |
|---|---|
| Protokół | **Nowy, czysty** — bez naleciałości typu `SESJA -` / końcówek `" -"`, bez stałopozycyjnych pól bez separatorów. Jawne, rozdzielane ramki. |
| CAN | **Zostaje** — przydatne dane z auta (prędkość, pedał, przebieg; reszta pól do ustalenia). |
| LCD | **Usunąć**, zastąpić **debugiem po USART0** (log tekstowy zamiast komunikatów na wyświetlacz). |
| Roaming | **Zostaje** — obsługa pracy za granicą (tabela krajów, wybór operatora). |

## Polityka zapisu pozycji (główna poprawka)

### Problem w obecnym firmware

Wyzwalanie zapisu rekordu (`docs/MEMORY_MAP.md`, `@0x5a0–0x8ba`) jest
**dystansową maszyną stanów**: rekord zapisywany dopiero, gdy licznik ruchu
przekroczy próg z pola **poz. 22 odpowiedzi CONFIG**. Brak jakiegokolwiek
wyzwalacza od **zmiany kierunku jazdy**.

Skutek — zmierzony na przejeździe testowym (ok. 2,8 km, 8,3 min jazdy,
tylko **16 zapisanych pozycji**):

| miara | wartość |
|---|---|
| odstęp czasowy między zapisami | mediana **37 s**, maks **51 s** |
| odległość między zapisami (w linii prostej) | mediana **122 m**, maks **398 m** |
| najgorszy przeskok | 51 s / 398 m prostej ≈ **~680 m łuku** przy 48 km/h |
| udział rzeczywistej drogi „ścięty" prostymi | **~22 %** |

Na zakrętach urządzenie nie zapisuje nic, aż „uzbiera" próg dystansu, i łączy
dwa odległe punkty prostą — zakręty znikają. A PA6C daje fiks **co 1 s**, czyli
dane są dostępne ~30× gęściej, firmware je wyrzuca.

### Nowa logika — logowanie adaptacyjne

Zapis rekordu, gdy spełniony **którykolwiek** z warunków (w ruchu):

1. **Zmiana kursu > ~15–25°** (course z RMC/VTG) — łapie zakręty; to brakujący
   element powodujący ścinanie.
2. **Maksymalny czas** w ruchu, np. co **2–5 s** — gwarantuje gęstość na prostej.
3. **Minimalny dystans** ~**15–25 m** — tłumi dublowanie punktów w korku/pełzaniu.
4. **Zdarzenia start/stop** (ruszenie, zatrzymanie) oraz gwałtowne
   przyspieszenie/hamowanie.
5. **Postój** — rzadko (co kilka min), dla oszczędności transmisji.

Progi konfigurowalne z serwera (jak dziś poz. 22, ale z osobnymi polami na kąt,
czas i dystans). Efekt: wierne zakręty, gęsto tylko tam, gdzie trasa się „dzieje",
rozsądny wolumen danych.

## Otwarte pytania (do rozstrzygnięcia przed kodowaniem)

- **Podłączenie modułu CAN** — ustalone, że dane wchodzą tekstowo na USART1
  (`docs/CAN.md`); do potwierdzenia pozostaje fizyczny punkt wpięcia modułu
  (czy dzieli tę samą linię RX co GPS/GSM) i dokładny format ramki od modułu.
- **Identyfikacja pola paliwa** — kod potwierdza, że moduł podaje **jawne pole
  „Paliwo"** (`docs/CAN.md`), a `?B` z sekcji CAN2 jest jego silnym kandydatem.
  Skala jest jednak własnością modułu, więc ~287 nie przeliczymy na litry bez
  punktu odniesienia: potrzebny log przy innym poziomie baku albo liczba z
  oryginalnej platformy.
