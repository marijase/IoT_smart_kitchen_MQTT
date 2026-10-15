/*
 * aplikacija.c — Konzolna korisnička aplikacija (HLR-7)
 *
 * MQTT : subscribe TOPIC_APP_STATUS    — prima stanje od kontrolera
 *        publish  TOPIC_APP_GET_STATUS — zahteva stanje (SWR-7.1)
 *        publish  TOPIC_APP_CMD_*      — salje komande kontroleru
 *
 * Aplikacija NE CUVA stanje — dobavlja ga od kontrolera na zahtev
 * (zahtev profesora: "informacije ne cuvati u aplikaciji").
 *
 * Aplikacija komunicira iskljucivo sa KONTROLEROM putem MQTT.
 * Kontroler dalje prosledjuje komande aktuatorima ako su aktivni.
 *
 * SWR-7.2: Prati status konekcije na broker (Connected/Disconnected).
 *
 * Kompajliranje:
 *   gcc aplikacija.c -o aplikacija -lmosquitto
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <mosquitto.h>

/* Koristimo define-ove iz pravila.h bez ukljucivanja SSDP zavisnosti */
#define TOPIC_APP_GET_STATUS    "kuhinja/app/get_status"
#define TOPIC_APP_STATUS        "kuhinja/app/status"
#define TOPIC_APP_CMD_ASPIRATOR "kuhinja/app/aspirator"
#define TOPIC_APP_CMD_UTICNICA  "kuhinja/app/uticnica"
#define TOPIC_APP_CMD_SVETLO    "kuhinja/app/svetlo"
#define MQTT_BROKER             "localhost"
#define MQTT_PORT               1883

/* =========================================================
 * Lokalna kopija stanja — SAMO za prikaz, ne cuva se trajno.
 * Osvezava se na svaki zahtev get_status.
 * ========================================================= */
static struct {
    /* Aspirator */
    char asp_ukl[4];       /* "ON" / "OFF" / "N/A" */
    int  asp_brzina;       /* 0-3                   */
    /* Uticnica */
    char utic_ukl[4];
    /* Svetlo */
    char svetlo_ukl[4];
    /* Senzori */
    float vlaga;
    float struja;
    float snaga;
    /* Meta */
    int   primljeno;       /* 1 = stigao odgovor    */
    int   broker_ok;       /* 1 = broker dostupan   */
} prikaz = {
    .asp_ukl    = "N/A",
    .asp_brzina = 0,
    .utic_ukl   = "N/A",
    .svetlo_ukl = "N/A",
    .vlaga  = 0.0f,
    .struja = 0.0f,
    .snaga  = 0.0f,
    .primljeno  = 0,
    .broker_ok  = 0
};

static struct mosquitto *mosq = NULL;

/* =========================================================
 * MQTT on_message — prima JSON status od kontrolera
 *
 * Ocekivani format:
 * {
 *   "aspirator":{"ukljucenost":"ON","brzina":2},
 *   "uticnica":{"ukljucenost":"OFF"},
 *   "svetlo":{"ukljucenost":"ON"},
 *   "vlaga":65.3,
 *   "struja":7.80,
 *   "snaga":1711.50
 * }
 * ========================================================= */
static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg) {
    (void)m; (void)obj;

    if (strcmp(msg->topic, TOPIC_APP_STATUS) != 0) return;

    const char *p = (const char *)msg->payload;

    /* Parsiranje — sscanf na specificnim podstringovima */
    char asp_ukl[4], utic_ukl[4], svetlo_ukl[4];
    int  asp_brz = 0;
    float vlaga = 0, struja = 0, snaga = 0;

    /* Trazimo svaki kljuc zasebno — robustno na raspored polja */
    const char *ptr;

    ptr = strstr(p, "\"aspirator\":{\"ukljucenost\":\"");
    if (ptr) sscanf(ptr, "\"aspirator\":{\"ukljucenost\":\"%3[^\"]\"",
                    asp_ukl);

    ptr = strstr(p, "\"brzina\":");
    if (ptr) sscanf(ptr, "\"brzina\":%d", &asp_brz);

    ptr = strstr(p, "\"uticnica\":{\"ukljucenost\":\"");
    if (ptr) sscanf(ptr, "\"uticnica\":{\"ukljucenost\":\"%3[^\"]\"",
                    utic_ukl);

    ptr = strstr(p, "\"svetlo\":{\"ukljucenost\":\"");
    if (ptr) sscanf(ptr, "\"svetlo\":{\"ukljucenost\":\"%3[^\"]\"",
                    svetlo_ukl);

    ptr = strstr(p, "\"vlaga\":");
    if (ptr) sscanf(ptr, "\"vlaga\":%f", &vlaga);

    ptr = strstr(p, "\"struja\":");
    if (ptr) sscanf(ptr, "\"struja\":%f", &struja);

    ptr = strstr(p, "\"snaga\":");
    if (ptr) sscanf(ptr, "\"snaga\":%f", &snaga);

    /* Azuriramo prikaz */
    strncpy(prikaz.asp_ukl,    asp_ukl,    sizeof(prikaz.asp_ukl)-1);
    strncpy(prikaz.utic_ukl,   utic_ukl,   sizeof(prikaz.utic_ukl)-1);
    strncpy(prikaz.svetlo_ukl, svetlo_ukl, sizeof(prikaz.svetlo_ukl)-1);

    /* KLJUČNA LOGIKA */
    if (strcmp(asp_ukl, "OFF") == 0) {
        prikaz.asp_brzina = 0;
    } else {
        prikaz.asp_brzina = asp_brz;
    }

    prikaz.vlaga      = vlaga;
    prikaz.struja     = struja;
    prikaz.snaga      = snaga;
    prikaz.primljeno  = 1;
}

/* =========================================================
 * Callback za konekciju — SWR-7.2
 * ========================================================= */
static void on_connect(struct mosquitto *m, void *obj, int rc) {
    (void)obj;
    if (rc == 0) {
        prikaz.broker_ok = 1;
        printf("[APP][MQTT] Povezana na broker.\n");
        mosquitto_subscribe(m, NULL, TOPIC_APP_STATUS, 0);
    } else {
        prikaz.broker_ok = 0;
        fprintf(stderr, "[APP][MQTT] Greska konekcija: %d\n", rc);
    }
}

static void on_disconnect(struct mosquitto *m, void *obj, int rc) {
    (void)m; (void)obj;
    prikaz.broker_ok = 0;
    printf("[APP][MQTT] Veza prekinuta (kod: %d)\n", rc);
}

/* =========================================================
 * Pomocne funkcije
 * ========================================================= */

/* Zahteva stanje od kontrolera i ceka odgovor (max 2s) */
static void osvezi(void) {
    prikaz.primljeno = 0;
    
    // Postavljamo sve na podrazumevano u slucaju da kontroler ne odgovori
    // tako da korisnik odmah vidi realno stanje ("N/A")
    strcpy(prikaz.asp_ukl, "N/A");
    prikaz.asp_brzina = 0;
    strcpy(prikaz.utic_ukl, "N/A");
    strcpy(prikaz.svetlo_ukl, "N/A");
    prikaz.vlaga = 0.0f;
    prikaz.struja = 0.0f;
    prikaz.snaga = 0.0f;

    mosquitto_publish(mosq, NULL, TOPIC_APP_GET_STATUS, 0, "", 0, false);
    for (int i = 0; i < 20 && !prikaz.primljeno; i++) {
        mosquitto_loop(mosq, 100, 1);
        usleep(100000); /* 100ms */
    }
    if (!prikaz.primljeno)
        printf("  [UPOZORENJE] Kontroler ne odgovara.\n");
}

/* Publish komande i log */
static void pub(const char *topic, const char *payload) {
    int rc = mosquitto_publish(mosq, NULL, topic,
                               (int)strlen(payload), payload, 1, false);
    if (rc == MOSQ_ERR_SUCCESS)
        printf("  [POSLANO] %s -> %s\n", topic, payload);
    else
        printf("  [GRESKA] Nije moguce poslati komandu.\n");
    usleep(200000);
}

static void ocisti(void) { printf("\033[H\033[J"); }
static void linija(void) { printf("  ----------------------------------------\n"); }

/* =========================================================
 * Prikaz stanja (ARCH-7.2.1)
 * ========================================================= */
static void prikazi_stanje(void) {
    osvezi();
    ocisti();

    printf("\n");
    printf("  ==========================================\n");
    printf("      PAMETNA KUHINJA — Stanje sistema\n");
    printf("  ==========================================\n");
    printf("  Broker: %s\n\n",
           prikaz.broker_ok ? "Povezan" : "*** NIJE POVEZAN ***");

    printf("  ASPIRATOR\n");
    printf("    Ukljucenost : %s\n",  prikaz.asp_ukl);
    printf("    Brzina      : %d / 3\n", prikaz.asp_brzina);
    linija();

    printf("  UTICNICA\n");
    printf("    Ukljucenost : %s\n",  prikaz.utic_ukl);
    printf("    Snaga       : %.1f W  |  Struja: %.2f A\n",
           prikaz.snaga, prikaz.struja);
    linija();

    printf("  SVETLO\n");
    printf("    Ukljucenost : %s\n",  prikaz.svetlo_ukl);
    linija();

    printf("  SENZORI\n");
    printf("    Vlaga : %.1f %%RH  ", prikaz.vlaga);
    printf("\n");
}

static void meni_aspirator(void) {
    int izbor;
    ocisti();
    printf("\n  === ASPIRATOR ===\n");
    printf("  Stanje: %s | Brzina: %d\n\n", prikaz.asp_ukl, prikaz.asp_brzina);
    printf("    1 - Ukljuci (ON)\n");
    printf("    2 - Iskljuci (OFF)\n");
    printf("    3 - Postavi brzinu (1/2/3)\n");
    printf("    4 - Vrati u AUTO mod\n");
    printf("    0 - Nazad\n");
    printf("  Izbor: ");
    if (scanf("%d", &izbor) != 1) { while (getchar() != '\n'); return; }
    switch (izbor) {
        case 1: pub(TOPIC_APP_CMD_ASPIRATOR, "ON");   break;
        case 2: pub(TOPIC_APP_CMD_ASPIRATOR, "OFF");  break;
        case 3: {
            int b;
            printf("  Brzina (1-3): ");
            if (scanf("%d", &b) == 1 && b >= 1 && b <= 3) {
                char buf[16];
                snprintf(buf, sizeof(buf), "BRZINA:%d", b);
                pub(TOPIC_APP_CMD_ASPIRATOR, buf);
            } else printf("  Neispravna vrednost.\n");
            break;
        }
        case 4: pub(TOPIC_APP_CMD_ASPIRATOR, "AUTO"); break;
        case 0: return;
        default: printf("  Nepoznata opcija.\n");
    }
    sleep(1);
}

static void meni_uticnica(void) {
    int izbor;
    ocisti();
    printf("\n  === UTICNICA ===\n");
    printf("  Stanje: %s | Snaga: %.1fW\n\n", prikaz.utic_ukl, prikaz.snaga);
    printf("    1 - Ukljuci (ON)\n");
    printf("    2 - Iskljuci (OFF)\n");
    printf("    0 - Nazad\n");
    printf("  Izbor: ");
    if (scanf("%d", &izbor) != 1) { while (getchar() != '\n'); return; }
    switch (izbor) {
        case 1: pub(TOPIC_APP_CMD_UTICNICA, "ON");  break;
        case 2: pub(TOPIC_APP_CMD_UTICNICA, "OFF"); break;
        case 0: return;
    }
    sleep(1);
}

static void meni_svetlo(void) {
    int izbor;
    ocisti();
    printf("\n  === SVETLO ===\n");
    printf("  Stanje: %s\n\n", prikaz.svetlo_ukl);
    printf("    1 - Upali (ON)\n");
    printf("    2 - Ugasi (OFF)\n");
    printf("    0 - Nazad\n");
    printf("  Izbor: ");
    if (scanf("%d", &izbor) != 1) { while (getchar() != '\n'); return; }
    switch (izbor) {
        case 1: pub(TOPIC_APP_CMD_SVETLO, "ON");  break;
        case 2: pub(TOPIC_APP_CMD_SVETLO, "OFF"); break;
        case 0: return;
    }
    sleep(1);
}

/* =========================================================
 * Glavni meni
 * ========================================================= */
static void glavni_meni(void) {
    int izbor;
    while (1) {
        prikazi_stanje();
        printf("  ==========================================\n");
        printf("              GLAVNI MENI\n");
        printf("  ==========================================\n");
        printf("    1 - Upravljanje aspiratorom\n");
        printf("    2 - Upravljanje uticnicom\n");
        printf("    3 - Upravljanje svetlom\n");
        printf("    4 - Osvezi stanje\n");
        printf("    0 - Izlaz\n");
        printf("\n  Izbor: ");

        if (scanf("%d", &izbor) != 1) {
            while (getchar() != '\n');
            continue;
        }
        switch (izbor) {
            case 1: meni_aspirator(); break;
            case 2: meni_uticnica();  break;
            case 3: meni_svetlo();    break;
            case 4: break; /* prikazi_stanje se poziva na pocetku while */
            case 0:
                printf("\n  Dovidjenja!\n\n");
                return;
        }
    }
}

/* =========================================================
 * main
 * ========================================================= */
int main(void) {
    printf("\n  Pokrecemo aplikaciju...\n");

    mosquitto_lib_init();
    mosq = mosquitto_new("Aplikacija-001", true, NULL);
    if (!mosq) { fprintf(stderr, "[APP] Greska init\n"); return 1; }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);

    /* SWR-7.2: pokusaj konekcije, ispisati gresku ako ne uspe */
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr,
                "[APP] Ne mogu se povezati na broker %s:%d\n"
                "      Proverite: sudo systemctl start mosquitto\n\n",
                MQTT_BROKER, MQTT_PORT);
        return 1;
    }

    /* Startuj MQTT petlju u pozadini (niti) */
    mosquitto_loop_start(mosq);
    sleep(1); /* cekamo da se konekcija uspostavi */

    glavni_meni();

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
