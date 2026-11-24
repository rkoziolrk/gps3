# OTA — zdalna aktualizacja firmware (bootloader, SMS + TCP)

Cel: aktualizować firmware zdalnie (urządzenie w aucie), bez podłączania
programatora. Wyzwalane SMS-em lub komendą z serwera; obraz pobierany po TCP
przez SIM900.

> **Status: łańcuch kompletny w kodzie.** Wyzwalacz SMS (`FWUPD*<PIN>`) czyta
> się w `gsm.c` i woła `ota_request()`; serwer serwuje obraz (`--fw`); bootloader
> pobiera i zapisuje przez SPM z weryfikacją CRC32 (zgodny z serwerem). Rdzeń
> bootloadera (SPM, CRC, skok, UART) jest pewny; **sekwencja modemu i pętla
> pobierania wymagają dostrojenia na stole** przed zaufaniem w terenie. Zalecany
> pierwszy test OTA: na biurku, z podglądem logu na USART0.

## Zasada bezpieczeństwa (najważniejsze)

**Bootloader nigdy nie nadpisuje samego siebie.** Leży w sekcji BOOT (NRWW),
która nie jest kasowana podczas aktualizacji aplikacji. Dzięki temu:

- jeśli pobieranie/zapis przerwie się (zanik zasięgu, reset), aplikacja może być
  uszkodzona, ale **bootloader zostaje** i przy następnym starcie ponowi OTA,
- urządzenie jest odzyskiwalne zdalnie, dopóki łapie GSM,
- bootloader sprawdza **CRC32** aplikacji przed oddaniem jej sterowania; zła suma
  → zostaje w trybie pobierania.

Pierwsze wgranie bootloadera i ustawienie fuse'ów robi się **raz, na stole**,
programatorem ISP. Potem wszystkie aktualizacje idą OTA.

## Mapa pamięci (ATmega64, 64 KB flash, strona 256 B)

| zakres (bajtowo) | zawartość |
|---|---|
| 0x0000..0xEFFF | aplikacja (RWW) — ~60 KB, firmware ma ~9 KB |
| 0xF000..0xFFFF | bootloader (NRWW, BOOT 2048 słów = 4 KB) |

- Reset trafia do bootloadera (fuse BOOTRST=0, wektor reset 0x7800 słowo = 0xF000 B).
- Bootloader (NRWW) może zapisywać sekcję aplikacji (RWW) podczas działania.
- Ostatnie 6 B aplikacji (lub ustalony adres) przechowuje `rozmiar` + `CRC32`
  obrazu — bootloader je weryfikuje.

## Fuse (ustawiane RAZ, ISP)

BOOTRST=0 (reset do boot), BOOTSZ=00 (2048 słów). Dla ATmega64 to **hfuse =
0x98** (z domyślnego 0x99 kasujemy bity BOOTRST, BOOTSZ0, BOOTSZ1).

```bash
# ZWERYFIKUJ z kartą katalogową przed użyciem!
avrdude -c usbasp -p m64 -U hfuse:w:0x98:m
```

Lock bity opcjonalnie chronią sekcję BOOT przed nadpisaniem przez aplikację.

## Wyzwalanie aktualizacji

1. **SMS**: `FWUPD*<PIN>` (PIN = 4 ostatnie cyfry IMEI) → aplikacja ustawia flagę
   w EEPROM i restartuje się (watchdog) do bootloadera.
2. **Serwer (TCP)**: w odpowiedzi na ramkę urządzenia serwer może odesłać
   `#FWUPD` → aplikacja ustawia flagę i restartuje.

Flaga w EEPROM: `ee_bootflag = 0xB0` (żądanie OTA). Bootloader po wejściu czyta
flagę; po udanej aktualizacji ją kasuje.

## Protokół pobierania (serwer -> bootloader)

Po TCP, tekstowo w nagłówku, potem surowe bajty:

```
-> FWREQ <imei> <currentCRC>\n          (bootloader prosi o obraz)
<- FW <rozmiar> <crc32hex>\n             (serwer: nagłówek)
<- <rozmiar bajtów obrazu .bin>          (surowo, w paczkach +IPD)
-> FWOK\n   albo  FWERR\n                (bootloader potwierdza wynik)
```

- Bootloader zapisuje obraz stronami (256 B) przez SPM w miarę napływania,
  liczy CRC32 i porównuje z nagłówkiem.
- Zgodność → zapis `rozmiar`+`CRC32` do pól kontrolnych, skasowanie flagi,
  skok do aplikacji. Niezgodność → `FWERR`, ponów przy następnym starcie.
- Serwer serwuje `build/tracker.bin` (patrz serwer: `--fw <plik>`).

> **Odbiór w przerwaniu (ISR + pierścień) — bo SPM gubiłby bajty.**
> SPM blokuje CPU na ~9 ms; przy 9600 bps w tym czasie wpada ~9 bajtów, a
> sprzętowy bufor USART ma tylko 2 bajty. Gdyby bootloader odbierał pollingiem,
> podczas zapisu każdej strony gubiłby bajty → CRC FAIL i ponawianie (objaw:
> log urywa się na `hdr ok, flash` i wraca do `modem?`).
>
> Dane płyną trybem **push** (`AT+CIPHEAD=0`, surowy strumień), a odbiór idzie
> przez ISR `USART1_RX_vect` z buforem pierścieniowym. ISR leży w sekcji BOOT
> (NRWW), więc wykonuje się także w trakcie programowania sekcji aplikacji (RWW)
> i wchłania bajty podczas SPM — nic nie ginie. Na czas OTA bootloader ustawia
> `IVSEL=1` (wektory w BOOT); `jump_to_app()` przywraca `IVSEL=0`.
>
> **Dlaczego nie CIPRXGET (pull):** rozważaliśmy tryb ręczny modemu
> (`AT+CIPRXGET=1`), gdzie host sam pobiera dane i SPM wypada w ciszy. Niestety
> **ten egzemplarz SIM900 odrzuca `AT+CIPRXGET` (ERROR)** — log bootloadera
> kończył się `rxget!` i wracał na starą aplikację. Push + ISR nie zależy od
> wsparcia CIPRXGET w firmwarze modemu, a surowy push na pewno dostarcza dane
> (stary bootloader ściągał cały obraz, padał tylko CRC — czyli właśnie na
> gubieniu bajtów, które ISR eliminuje).
>
> **Serwer wysyła cały obraz jednym ciągiem, bez pacingu.** Tempo i tak ogranicza
> UART 9600 i bufor modemu; CRC32 weryfikuje całość.

## Rdzeń bootloadera (`boot/boot.c`)

Gotowe, standardowe elementy:
- `spm_erase_write_page()` — kasowanie + zapis strony przez SPM (NRWW pisze RWW),
- `crc32()` — weryfikacja obrazu,
- `jump_to_app()` — skok pod 0x0000 z wyłączonymi przerwaniami,
- minimalny UART1 (ten sam port co aplikacja) do modemu.

Do dostrojenia na stole (oznaczone TODO w `boot/boot.c`):
- czasy i poziomy PWRKEY/zasilania modemu,
- dokładny parsing `+IPD,<n>:` i obsługa fragmentacji paczek TCP,
- timeouty i liczba ponowień.

## Procedura wdrożenia (raz, na stole)

```bash
make            # aplikacja -> build/tracker.hex + build/tracker.bin
make -C boot    # bootloader -> boot/boot.hex
# 1) fuse (raz):
avrdude -c usbasp -p m64 -U hfuse:w:0x98:m
# 2) bootloader (raz, ISP):
avrdude -c usbasp -p m64 -U flash:w:boot/boot.hex:i
# 3) pierwsza aplikacja (ISP lub od razu OTA):
avrdude -c usbasp -p m64 -U flash:w:build/tracker.hex:i
```

Dalej już tylko: `FWUPD*<PIN>` SMS-em (albo `#FWUPD` z serwera) → OTA.

## Decyzje projektowe

**Bootloader jest niezmienną kotwicą — aktualizowany tylko przewodowo (VVDI/ISP),
nigdy zdalnie.** Rozważaliśmy aktualizację bootloadera z poziomu działającego
urządzenia i świadomie z niej rezygnujemy:

- **Z samej aplikacji to niemożliwe sprzętowo.** Na AVR instrukcja SPM (zapis
  flasha) działa wyłącznie wykonywana z sekcji BOOT; kod aplikacji (RWW) nie
  zapisze żadnej strony flasha. Aplikacja nie odłoży sobie nawet obrazu
  bootloadera (EEPROM 2 KB < 4 KB bootloadera, brak innego magazynu).
- **Bootloader mógłby przepisać sam siebie** (sekcja BOOT może programować własne
  strony), ale to aktualizowanie *tego, co aktualizuje*: zanik zasilania w trakcie
  zapisu sekcji BOOT = brak działającego bootloadera, a żaden kod aplikacji tego
  nie naprawi (brak SPM poza BOOT). Ratunek tylko VVDI/ISP — czyli tracimy
  gwarancję „zawsze odzyskiwalny zdalnie", dla której bootloader w ogóle istnieje.
- **Wariant dwustopniowy** (mały niezmienny loader przepisujący resztę) odpada:
  ATmega64 ma jedną sekcję BOOT (max 2048 słów), tylko ona ma SPM, i jest już
  niemal pełna (≈4062/4096 B).

Podział odpowiedzialności: **aplikacja → OTA do woli (bootloader nietknięty,
zawsze odzyskiwalny); bootloader → kabel, tylko gdy on sam się zmienia (rzadko).**
Zmiana bootloadera na pull/CIPRXGET była takim wyjątkiem; docelowo bootloader ma
pozostać stały.
