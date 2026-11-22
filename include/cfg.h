/*
 * cfg.h — konfiguracja budowania i wartości domyślne (nadpisywalne z EEPROM).
 *
 * To są DOMYŚLNE ustawienia zaszyte w obrazie. Część z nich (host, port, tryb
 * roamingu, progi logowania) jest wczytywana z wewnętrznego EEPROM przy starcie
 * i może być zmieniona zdalnie — patrz src/cfg.c, docs/EEPROM.md.
 */
#ifndef CFG_H
#define CFG_H

/* --- Tożsamość / wersja -------------------------------------------------- */
#define FW_VERSION    "3.0"        /* nowy firmware (nie mylić z oryg. 2.65)  */
#define PROTO_VERSION 1            /* wersja protokołu serwera (nowy, czysty) */
#define GPS_MODULE    "PA6C"
#define GSM_MODULE    "SIM900"

/* --- Sieć / serwer (domyślne, nadpisywalne z EEPROM i SMS) --------------- */
#define APN_DEFAULT         "internet"
#define SERVER_HOST_DEFAULT "example.entrydns.org"   /* bez blokady vendora */
#define SERVER_PORT_DEFAULT 3506

/* Limity długości pól konfiguracyjnych (z kontrolą — brak przepełnień!). */
#define HOST_MAXLEN 48
#define PORT_MAXLEN 5
#define IMEI_LEN    15
#define APN_MAXLEN  16

/* --- Polityka zapisu pozycji (logowanie adaptacyjne, docs/REWRITE.md) ---- *
 * Rekord zapisywany, gdy spełniony KTÓRYKOLWIEK warunek w ruchu.            *
 * Wartości domyślne — do strojenia na sprzęcie i konfigurowalne z serwera.  */
#define TRK_COURSE_DEG   20    /* zmiana kursu [°] wyzwalająca zapis         */
#define TRK_MAX_SECS      5    /* maks. odstęp czasowy w ruchu [s]          */
#define TRK_MIN_DIST_M   20    /* min. dystans między zapisami [m]          */
#define TRK_MOVE_SPD      3    /* próg prędkości uznania za ruch [km/h]     */
#define TRK_STOP_SECS   120    /* odstęp zapisu na postoju [s]              */

/* --- Debug / zbieranie danych (USART0) ----------------------------------- *
 * DBG_RAW_CAN=1 -> zrzut surowych ramek modułu CAN na port debug (do analizy *
 * paliwa i pól CAN). DBG_RAW_LINE=1 -> zrzut wszystkich odebranych linii.    */
#define DBG_LEVEL     2           /* 0=cisza 1=info 2=debug 3=verbose        */
#define DBG_RAW_CAN   1
#define DBG_RAW_LINE  1

#endif /* CFG_H */
