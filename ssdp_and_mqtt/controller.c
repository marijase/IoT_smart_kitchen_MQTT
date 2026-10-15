/*
 * MQTT : subscribe na kuhinja/# — prima sve senzorske i statusne poruke
 *        publish komande aktuatorima NA OSNOVU PRAVILA iz pravila.c
 *
 * Prioritet komandi (SWR-6.5):
 *   1. Zastita uticnice (OVERCURRENT / OVERPOWER) — najvisi prioritet
 *   2. Rucne komande iz aplikacije
 *   3. Automatske komande aspiratora na osnovu vlage
 *
 * Kljucno: kontroler salje komande SAMO ako je uredjaj aktivan u SSDP registru. ("ne slati komande neaktivnom uredjaju")

 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include "pravila.h"
#include <fcntl.h>
#include <errno.h>


#define CTRL_UUID       "uuid:kontroler-001"
#define CTRL_FRIENDLY   "Kontroler-PametnaKuhinja"
#define CTRL_AGENT      "Linux/5.0 UPnP/2.0 Kontroler/1.0"



// registar aktivnih uredjaja
static Device   registry[MAX_DEVICES];
static int      sock    = -1;
static struct   sockaddr_in ssdp_dst;
static struct mosquitto *mosq = NULL;  

//stanje sistema 
static struct {
    float vlaga;
    float struja;
    float snaga;
    int   asp_brzina;          // 0-3
    int   asp_override;       
    char  asp_ukl[4];          // "ON"  "OFF" 
    char  svetlo_ukl[4];
    char  utic_ukl[4];
} sys = {
    .vlaga       = 0.0f,
    .struja      = 0.0f,
    .snaga       = 0.0f,
    .asp_brzina  = 0,
    .asp_override = 0,
    .asp_ukl     = "OFF",
    .svetlo_ukl  = "OFF",
    .utic_ukl    = "OFF"
};
static int nova_struja = 0;
static int nova_snaga  = 0;


// da li je uredjaj datog tipa aktivan?
// type — podstring koji se trazi u USN uredjaja npr. "aspirator", "sijalica", "senzor-vlage"
static int is_active(const char *type) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (registry[i].active && strstr(registry[i].usn, type))
            return 1;
    return 0;
}

/* =========================================================
 * SetPropertyValue()
 * Salje komandu SAMO ako je uredjaj aktivan.
 * ========================================================= */
static void cmd(const char *device_type, const char *topic,
                const char *payload, int qos, bool retain) {
    if (!is_active(device_type)) {
        printf("  %s nije aktivan, komanda ignorisana.\n", device_type);
        return;
    }
    mosquitto_publish(mosq, NULL, topic,
                      (int)strlen(payload), payload, qos, retain);
    //printf("[KONTROLER][MQTT] -> %s : %s\n", topic, payload);
}

/* =========================================================
 * Logika automatizacije aspiratora
 * Poziva se sinhrono u on_message kada stigne nova vlaga
 * ========================================================= */
static void automatika_aspirator(float vlaga) {
    if (!is_active("aspirator")) {
    printf(" Aspirator nije aktivan.\n");
    return;
    }
    int nova_brzina = pravilo_brzina(vlaga, sys.asp_brzina);

    if (nova_brzina == sys.asp_brzina)
        return; 

    printf(" Vlaga %.1f%%RH =>  brzina: %d\n",
           vlaga, nova_brzina);

    char brzina_str[4];
    snprintf(brzina_str, sizeof(brzina_str), "%d", nova_brzina);

    // AUTO mod  
    cmd("aspirator", TOPIC_ASPIRATOR_MOD, "AUTO", 1, false);
    cmd("aspirator", TOPIC_ASPIRATOR_BRZINA, brzina_str, 1, true);

    if (nova_brzina > 0) {
        cmd("aspirator", TOPIC_ASPIRATOR_UKLJUCENOST, "ON", 1, true);
        strncpy(sys.asp_ukl, "ON", sizeof(sys.asp_ukl));
    } else {
        cmd("aspirator", TOPIC_ASPIRATOR_UKLJUCENOST, "OFF", 1, true);
        strncpy(sys.asp_ukl, "OFF", sizeof(sys.asp_ukl));
    }

    sys.asp_brzina = nova_brzina;
}

/* =========================================================
 * Slanje statusa svih uredjaja aplikaciji
 * Poziva se na zahtev aplikacije (TOPIC_APP_GET_STATUS).
 * ========================================================= */
static void send_status_to_app(void) {
    // Pripremamo privremene stringove za JSON
    char asp_status[4];
    char utic_status[4];
    char svetlo_status[4];

    // Provera za Aspirator
    if (is_active("aspirator")) {
        strncpy(asp_status, sys.asp_ukl, sizeof(asp_status));
    } else {
        strcpy(asp_status, "N/A");
    }

    // Provera za Utičnicu
    if (is_active("uticnica")) {
        strncpy(utic_status, sys.utic_ukl, sizeof(utic_status));
    } else {
        strcpy(utic_status, "N/A");
    }

    // Provera za Svetlo (Sijalicu)
    if (is_active("sijalica")) {
        strncpy(svetlo_status, sys.svetlo_ukl, sizeof(svetlo_status));
    } else {
        strcpy(svetlo_status, "N/A");
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"aspirator\":{\"ukljucenost\":\"%s\",\"brzina\":%d},"
        "\"uticnica\":{\"ukljucenost\":\"%s\"},"
        "\"svetlo\":{\"ukljucenost\":\"%s\"},"
        "\"vlaga\":%.1f,"
        "\"struja\":%.2f,"
        "\"snaga\":%.2f"
        "}",
        asp_status, is_active("aspirator") ? (sys.asp_brzina >= 0 ? sys.asp_brzina : 0) : 0,
        utic_status,
        svetlo_status,
        is_active("senzor-vlage") ? sys.vlaga : 0.0f, 
        is_active("senzor-uticnice") ? sys.struja : 0.0f, 
        is_active("senzor-uticnice") ? sys.snaga : 0.0f);

    mosquitto_publish(mosq, NULL, TOPIC_APP_STATUS,
                      (int)strlen(json), json, 1, false);
    printf("[KONTROLER] Stanje poslato aplikaciji.\n");
}

/* =========================================================
 * MQTT on_message 
 * Obradjuje sve dolazne MQTT poruke i primenjuje pravila.
 * ========================================================= */
static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg) {
    (void)m; (void)obj;
    
    if (msg->retain) {    
        return;
    }

    
    const char *topic   = msg->topic;
    const char *payload = (const char *)msg->payload;
    time_t      now     = time(NULL);
    struct tm  *t       = localtime(&now);
    
    
    
    printf("[%02d:%02d:%02d][MQTT] %s -> %s\n",
           t->tm_hour, t->tm_min, t->tm_sec, topic, payload);

    /* -------------------------------------------------------
     * 1. SENZOR VLAGE 
     *    Kontroler prima vrednost, primenjuje pravilo_brzina.
     *    Ne reaguje ako senzor nije aktivan u SSDP registru.
     * ----------------------------------------------------- */
    if (strcmp(topic, TOPIC_VLAGA) == 0) {
        if (!is_active("senzor-vlage")) {
            printf(" Vlaga ignorisana — senzor nije u registru.\n");
            return;
        }
        if (strcmp(payload, "ERROR") == 0) {
            printf("[KONTROLER] Senzor vlage javio GRESKU.\n");
            return;
        }
        sys.vlaga = atof(payload);


        if (!sys.asp_override)
            automatika_aspirator(sys.vlaga);
        return;
    }

    /* -------------------------------------------------------
     * 2. SENZOR UTICNICE — struja i snaga 
     * PRIORITET 1: zastita uticnice ima prednost nad svim.
     * ----------------------------------------------------- */

    if (strcmp(topic, TOPIC_UTICNICA_STRUJA) == 0) {
    if (!is_active("senzor-uticnice")) return;

    sys.struja = atof(payload);
    nova_struja = 1;

    if (!is_active("uticnica")) {
        printf("[KONTROLER] CURRENT = (%.2f A), ali uticnica nije aktivna.\n", sys.struja);
        return;
    }

    if (nova_struja && nova_snaga) {
        pravilo_zastita_uticnice(mosq, sys.struja, sys.snaga);
        nova_struja = 0;
        nova_snaga  = 0;
    }

    return;
}

    if (strcmp(topic, TOPIC_UTICNICA_SNAGA) == 0) {
    if (!is_active("senzor-uticnice")) return;

    sys.snaga = atof(payload);
    nova_snaga = 1;

    if (!is_active("uticnica")) {
        printf("[KONTROLER] POWER = (%.2f W), ali uticnica nije aktivna.\n", sys.snaga);
        return;
    }

    if (nova_struja && nova_snaga) {
        pravilo_zastita_uticnice(mosq, sys.struja, sys.snaga);
        nova_struja = 0;
        nova_snaga  = 0;
    }

    return;
}

    
    
    
    if (strcmp(topic, TOPIC_ASPIRATOR_STATUS) == 0) {
        /* JSON: {"ukljucenost":"ON","brzina":2,"mod":"AUTO"} */
        char ukl[4]; int brz;
        if (sscanf(payload,"{\"ukljucenost\":\"%3[^\"]\",\"brzina\":%d", ukl, &brz) == 2) {
            strncpy(sys.asp_ukl, ukl, sizeof(sys.asp_ukl));
            sys.asp_brzina = brz;
        }
        return;
    }

    if (strcmp(topic, TOPIC_UTICNICA_STATUS) == 0) {
        printf("Status uticnice: %s\n", payload);
        return;
    }

    if (strcmp(topic, TOPIC_SVETLO_STATUS) == 0) {
        char ukl[4];
        if (sscanf(payload, "{\"ukljucenost\":\"%3[^\"]\"", ukl) == 1)
            strncpy(sys.svetlo_ukl, ukl, sizeof(sys.svetlo_ukl));
        return;
    }

    // 4. KOMANDE IZ APLIKACIJE — PRIORITET 2 

    if (strcmp(topic, TOPIC_APP_GET_STATUS) == 0) {     // Aplikacija trazi kompletan status 
        send_status_to_app();
        return;
    }

    /* Rucna kontrola aspiratora */
    if (strcmp(topic, TOPIC_APP_CMD_ASPIRATOR) == 0) {
        
         // Rucna komanda aktivira override — automatika se pauzira. 
        if (strcmp(payload, "ON") == 0 || strcmp(payload, "OFF") == 0) {
            sys.asp_override = 1;
            cmd("aspirator", TOPIC_ASPIRATOR_UKLJUCENOST, payload, 1, true);
            cmd("aspirator", TOPIC_ASPIRATOR_MOD, "MANUAL", 1, false);
            printf("[KONTROLER] Rucni override aspiratora: %s\n", payload);
        } else if (strncmp(payload, "BRZINA:", 7) == 0) {
            sys.asp_override = 1;
            const char *brz = payload + 7;
            cmd("aspirator", TOPIC_ASPIRATOR_BRZINA, brz, 1, true);
            cmd("aspirator", TOPIC_ASPIRATOR_MOD, "MANUAL", 1, false);
        } else if (strcmp(payload, "AUTO") == 0) {
            // Korisnik vraca u automatski rezim 
            sys.asp_override = 0;
            cmd("aspirator", TOPIC_ASPIRATOR_MOD, "AUTO", 1, false);
            printf("[KONTROLER] Aspirator prebacen u AUTO mod.\n");
        }
        return;
    }

    // Rucna kontrola uticnice 
	if (strcmp(topic, TOPIC_APP_CMD_UTICNICA) == 0) {
	    if (strcmp(payload, "ON") == 0 || strcmp(payload, "OFF") == 0) {
		// Prvo proveravamo da li je utičnica aktivna u SSDP-u
		if (is_active("uticnica")) {
		    cmd("uticnica", TOPIC_UTICNICA_UKLJUCENOST, payload, 1, true);
		    strncpy(sys.utic_ukl, payload, sizeof(sys.utic_ukl));
		} else {
		    printf("Odbijena rucna komanda: Uticnica nije aktivna na mrezi!\n");
		    strncpy(sys.utic_ukl, "OFF", sizeof(sys.utic_ukl));
		}
	    }
	    return;
	}

    // Rucna kontrola svetla 
	if (strcmp(topic, TOPIC_APP_CMD_SVETLO) == 0) {
	    if (strcmp(payload, "ON") == 0 || strcmp(payload, "OFF") == 0) {
		// Prvo proveravamo da li je sijalica uopšte živa u SSDP-u
		if (is_active("sijalica")) {
		    cmd("sijalica", TOPIC_SVETLO_UKLJUCENOST, payload, 1, true);
		    strncpy(sys.svetlo_ukl, payload, sizeof(sys.svetlo_ukl));
		} else {
		    printf("[KONTROLER] Odbijena rucna komanda: Sijalica nije aktivna na mrezi!\n");
		    // Opciono: Možeš eksplicitno staviti na "OFF" ili "N/A" da aplikacija zna da nema uređaja
		    strncpy(sys.svetlo_ukl, "OFF", sizeof(sys.svetlo_ukl)); 
		}
	    }
	    return;
	}
}

static void on_connect(struct mosquitto *m, void *obj, int rc) {
    (void)obj;
    if (rc != 0) {
        fprintf(stderr, "[KONTROLER][MQTT] Greska konekcija: %d\n", rc);
        return;
    }
    printf("[KONTROLER][MQTT] Povezan na broker. Subscribujem na kuhinja/#\n");
    mosquitto_subscribe(m, NULL, "kuhinja/#", 0);    // Subscribe na sve teme kuhinje 
}

// SSDP — azuriranje registra uredjaja
static void registry_update(const char *usn, const char *nt, const char *loc, const char *status) {

	//printf("[DEBUG] registry_update: %s\n", usn);
    /* Da li uredjaj vec postoji? */
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (registry[i].active && strcmp(registry[i].usn, usn) == 0) {
            registry[i].last_seen = time(NULL);
            strncpy(registry[i].status, status, sizeof(registry[i].status)-1);
            return; /* azuriran */
        }
    }
    /* Novi uredjaj — dodaj u slobodan slot */
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!registry[i].active) {
            strncpy(registry[i].usn,      usn,    sizeof(registry[i].usn)-1);
            strncpy(registry[i].nt,       nt,     sizeof(registry[i].nt)-1);
            strncpy(registry[i].location, loc,    sizeof(registry[i].location)-1);
            strncpy(registry[i].status,   status, sizeof(registry[i].status)-1);
            registry[i].last_seen = time(NULL);
            registry[i].active    = 1;
            printf("[KONTROLER][SSDP] Novi uredjaj: %s\n", usn);
            return;
        }
    }
    fprintf(stderr, "[KONTROLER][SSDP] Registar pun!\n");
}
static void registry_remove(const char *usn)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (registry[i].active && strcmp(registry[i].usn, usn) == 0)
        {
            registry[i].active = 0;
            strcpy(registry[i].status, "OFF");

            // --- IZMENA OVDE ---
            if (strstr(usn, "senzor-vlage")) {
                sys.vlaga = 0.0f;
                sys.asp_brzina = 0;
                strncpy(sys.asp_ukl, "OFF", sizeof(sys.asp_ukl));

                printf("[KONTROLER] Senzor vlage nestao! Gasim aspirator iz bezbednosti...\n");
                
                // Šaljemo stvarne MQTT komande aktuatoru na mreži
                cmd("aspirator", TOPIC_ASPIRATOR_MOD, "AUTO", 1, false);
                cmd("aspirator", TOPIC_ASPIRATOR_BRZINA, "0", 1, true);
                cmd("aspirator", TOPIC_ASPIRATOR_UKLJUCENOST, "OFF", 1, true);
            }

            if (strstr(usn, "aspirator")) {
                strcpy(sys.asp_ukl, "OFF");
                sys.asp_brzina = 0;
            }

            printf("[KONTROLER][SSDP] Uredjaj odjavljen (byebye): %s\n", usn);
            return;
        }
    }
}

static void registry_cleanup(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (registry[i].active &&
            now - registry[i].last_seen > DEVICE_TIMEOUT)
        {
            registry_remove(registry[i].usn);
        }
    }
}

// Stampa stanje registra — SWR-6.3
static void registry_print(void) {
    printf("\n===== REGISTAR UREDJAJA =====\n");
    int found = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (strlen(registry[i].usn) > 0) {
            printf("  [%s] %s — %s\n",
                   registry[i].active ? "ONLINE     " : "UNAVAILABLE",
                   registry[i].usn,
                   registry[i].status);
            found = 1;
        }
    }
    if (!found)
    printf("  (nema pronadjenih uredjaja)\n");

	if (is_active("senzor-vlage"))
	    printf("  Vlaga: %.1f%%RH | ", sys.vlaga);
	else
	    printf("  Vlaga: N/A | ");


	if (is_active("senzor-uticnice"))
	    printf("Struja: %.2fA | Snaga: %.2fW\n", sys.struja, sys.snaga);
	else
	    printf("Struja: N/A | Snaga: N/A\n");


	if (is_active("aspirator"))
	    printf("  Aspirator: %s (brz:%d) | ", sys.asp_ukl, sys.asp_brzina >= 0 ? sys.asp_brzina : 0);
	else
	    printf("  Aspirator: N/A | ");


	if (is_active("uticnica"))
	    printf("Uticnica: %s | ", sys.utic_ukl);
	else
	    printf("Uticnica: N/A | ");


	if (is_active("sijalica"))
	    printf("Svetlo: %s\n", sys.svetlo_ukl);
	else
	    printf("Svetlo: N/A\n");

	printf("=============================\n\n");
}

static void send_msearch(void) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: ssdp:all\r\n"
        "USER-AGENT: %s\r\n"
        "CPFN.UPNP.ORG: %s\r\n"
        "CPUUID.UPNP.ORG: %s\r\n"
        "AUTH: %s\r\n\r\n",
        SSDP_ADDR, SSDP_PORT, CTRL_AGENT,
        CTRL_FRIENDLY, CTRL_UUID, AUTH_TOKEN);
    sendto(sock, buf, strlen(buf), 0,
           (struct sockaddr *)&ssdp_dst, sizeof(ssdp_dst));
    printf("[KONTROLER][SSDP] M-SEARCH poslat.\n");
}

static void process_ssdp(const char *buf, struct sockaddr_in *sender) {
    (void)sender;

    /* Odbaci poruke bez AUTH tokena */
    if (!strstr(buf, AUTH_TOKEN)) return;

    /* Obradi samo NOTIFY i 200 OK odgovore */
    if (!strstr(buf, "NOTIFY") && !strstr(buf, "200 OK")) return;

    /* Byebye — ukloni uredjaj iz registra */
    if (strstr(buf, "ssdp:byebye")) {
        char usn[128] = "";
        const char *p = strstr(buf, "USN: ");
        if (p) sscanf(p, "USN: %127[^\r\n]", usn);
        if (strlen(usn)) registry_remove(usn);
        return;
    }

    /* Izvuci polja */
    char usn[128] = "", nt[64] = "", loc[128] = "", status[16] = "ON";
    const char *p;

    if ((p = strstr(buf, "USN: ")))      sscanf(p, "USN: %127[^\r\n]", usn);
    if ((p = strstr(buf, "NT: ")))       sscanf(p, "NT: %63[^\r\n]",   nt);
    if (!strlen(nt) &&
        (p = strstr(buf, "ST: ")))       sscanf(p, "ST: %63[^\r\n]",   nt);
    if ((p = strstr(buf, "LOCATION: "))) sscanf(p, "LOCATION: %127[^\r\n]", loc);
    if ((p = strstr(buf, "X-DEVICE-STATUS: ")))
                                         sscanf(p, "X-DEVICE-STATUS: %15[^\r\n]", status);

    if (strlen(usn))
        registry_update(usn, nt, loc, status);
}
static void init_ssdp(void) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    // Postavljanje socketa u ne-blokirajući režim (non-blocking)
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(SSDP_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    ssdp_dst.sin_family = AF_INET;
    ssdp_dst.sin_port = htons(SSDP_PORT);
    ssdp_dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);
}

static void init_mqtt(void) {
    mosquitto_lib_init();
    mosq = mosquitto_new("Kontroler-001", true, NULL);
    if (!mosq) { fprintf(stderr, "[KONTROLER][MQTT] Greska init\n"); exit(1); }
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[KONTROLER][MQTT] Ne mogu se povezati na broker!\n");
}

static void load_config(void) {
    FILE *fp = fopen("config.json", "r");
    if (!fp) {
        printf("[KONTROLER] config.json nije nadjen — koristim defaults.\n");
        return;
    }
    /* Jednostavan linijski parser za flat JSON (bez ugnjezdenih objekata) */
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        float fval; int ival;
        /* Vrednosti se ucitavaju samo radi logovanja ovde;
           pragovi su definisani u pravila.h i koriste se direktno */
        if (sscanf(line, " \"PRAG_SNAGE\": %f", &fval) == 1)
            printf("[KONTROLER] Config: PRAG_SNAGE = %.1f W\n", fval);
        if (sscanf(line, " \"PRAG_SVETLA\": %d", &ival) == 1)
            printf("[KONTROLER] Config: PRAG_SVETLA = %d\n", ival);
        if (sscanf(line, " \"BRZINA_ASP\": %d", &ival) == 1)
            printf("[KONTROLER] Config: BRZINA_ASP = %d\n", ival);
    }
    fclose(fp);
    printf("[KONTROLER] Konfiguracija ucitana.\n");
}

static volatile int running = 1;
static void handle_exit(int sig) { (void)sig; running = 0; }

int main(void) {
    signal(SIGINT, handle_exit);
    memset(registry, 0, sizeof(registry));

    load_config();

    /* 1. SSDP inicijalizacija */
    init_ssdp();
    printf("[KONTROLER] SSDP aktivan.\n");

    /* 2. MQTT inicijalizacija */
    init_mqtt();
    printf("[KONTROLER] MQTT aktivan.\n");

    /* 3. Pocetni M-SEARCH da otkrijemo uredjaje na mrezi */
    send_msearch();

    char   rxbuf[2048];
    time_t last_msearch = time(NULL);
    time_t last_cleanup = time(NULL);
    time_t last_print   = time(NULL);

    while (running) {
        time_t now = time(NULL);

        /* Periodicni M-SEARCH (MSEARCH_INTERVAL = 60s) */
        if (now - last_msearch >= MSEARCH_INTERVAL) {
            send_msearch();
            last_msearch = now;
        }

        /* Cistimo uredjaje koji nisu javili alive (DEVICE_TIMEOUT = 25s) */
        if (now - last_cleanup >= 5) {
            registry_cleanup();
            last_cleanup = now;
        }

        /* Stampamo stanje registra svakih 15s (SWR-6.3) */
        if (now - last_print >= 15) {
            registry_print();
            last_print = now;
        }

        /* MQTT petlja — obradjuje dolazne poruke */
        int rc = mosquitto_loop(mosq, 100, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[KONTROLER][MQTT] Greska petlja, reconnect...\n");
            mosquitto_reconnect(mosq);
        }

        /* SSDP prijem NOTIFY i 200 OK (Sada je ne-blokirajući) */
        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        int n = recvfrom(sock, rxbuf, sizeof(rxbuf) - 1, 0, (struct sockaddr *)&sender, &slen);
        if (n > 0) {
            rxbuf[n] = '\0';
            process_ssdp(rxbuf, &sender);
        } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Ispisujemo grešku samo ako je stvarni problem na socketu,
            // a ne zato što je buffer trenutno prazan
            fprintf(stderr, "[KONTROLER][SSDP] Greška pri prijemu: %d\n", errno);
        }
    }

    printf("[KONTROLER] Gasenje...\n");
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(sock);
    return 0;
}

