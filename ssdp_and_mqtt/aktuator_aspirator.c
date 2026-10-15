/*
 * MQTT : subscribe na komandne teme (SWR-1.5, SWR-1.6, SWR-1.8):
 *          TOPIC_ASPIRATOR_UKLJUCENOST  — ON / OFF
 *          TOPIC_ASPIRATOR_BRZINA       — 0 / 1 / 2 / 3
 *          TOPIC_ASPIRATOR_MOD          — AUTO / MANUAL
 *        publish status (retain=true, SWR-1.9):
 *          TOPIC_ASPIRATOR_STATUS       
 *
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


#define DEVICE_NT   "urn:schemas-upnp-org:device:actuator:1"
#define DEVICE_USN  "uuid:aspirator-001::urn:schemas-upnp-org:device:actuator:aspirator"
#define DEVICE_LOC  "http://192.168.1.11:5000/aspirator.xml"
#define MQTT_ID     "Aktuator-Aspirator-001"

// stanje aspiratora 
static struct {
    char ukljucenost[4];   
    int  brzina;           
    char mod[8];           // auto ili manual
} asp = { "OFF", 0, "MANUAL" };

static int               g_sock = -1;
static struct sockaddr_in g_ssdp_dst;
static int               boot_id = 0;
static struct mosquitto *mosq    = NULL;


static void ssdp_notify(const char *nts) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "NOTIFY * HTTP/1.1\r\nHOST: %s:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\nNT: %s\r\nNTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 Aspirator/1.0\r\nUSN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        SSDP_ADDR, DEVICE_LOC, DEVICE_NT, nts,
        DEVICE_USN, boot_id, asp.ukljucenost, AUTH_TOKEN);
        
    sendto(g_sock, buf, strlen(buf), 0,
           (struct sockaddr *)&g_ssdp_dst, sizeof(g_ssdp_dst));
}

static void ssdp_respond(struct sockaddr_in *dest) {
    char date[64], resp[1024];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nDATE: %s\r\nEXT:\r\n"
        "LOCATION: %s\r\nSERVER: Linux/5.0 UPnP/2.0 Aspirator/1.0\r\n"
        "ST: %s\r\nUSN: %s\r\nBOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        date, DEVICE_LOC, DEVICE_NT, DEVICE_USN, boot_id,
        asp.ukljucenost, AUTH_TOKEN);
        
    sendto(g_sock, resp, strlen(resp), 0,
           (struct sockaddr *)dest, sizeof(*dest));
    printf("[Aspirator][SSDP] Odgovoreno na M-SEARCH\n");
}

static void handle_exit(int sig) {
    (void)sig;
    ssdp_notify("ssdp:byebye");
    printf("\n[Aspirator] Odjavljen. Izlazim.\n");
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(g_sock);
    exit(0);
}

/* =========================================================
 * Publish statusa aspiratora 
 *  - na svaku promenu stanja
 *  - periodicno svakih 30s
 * ========================================================= */
static void publish_status(void) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"ukljucenost\":\"%s\",\"brzina\":%d,\"mod\":\"%s\"}",
             asp.ukljucenost, asp.brzina, asp.mod);
    mosquitto_publish(mosq, NULL, TOPIC_ASPIRATOR_STATUS,
                      (int)strlen(json), json, 1, false);
}

/* =========================================================
 * MQTT on_message — prima komande od kontrolera
 *
 * Aspirator prima 3 teme:
 *   TOPIC_ASPIRATOR_UKLJUCENOST : "ON" ili "OFF"
 *   TOPIC_ASPIRATOR_BRZINA      : "0", "1", "2", "3"
 *   TOPIC_ASPIRATOR_MOD         : "AUTO" ili "MANUAL"
 * ========================================================= */
static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg) {
    (void)m; (void)obj;
    
    if (msg->retain) {
        return;
    }
    const char *topic   = msg->topic;
    const char *payload = (const char *)msg->payload;

    printf("[Aspirator][MQTT] %s -> %s\n", topic, payload);

    if (strcmp(topic, TOPIC_ASPIRATOR_UKLJUCENOST) == 0) {
        if (strcmp(payload, "ON") == 0 || strcmp(payload, "OFF") == 0) {
            strncpy(asp.ukljucenost, payload, sizeof(asp.ukljucenost) - 1);
            printf(">> [HARDVER] Aspirator: %s\n", asp.ukljucenost);
            publish_status();
        }

    } else if (strcmp(topic, TOPIC_ASPIRATOR_BRZINA) == 0) {
        int nova = atoi(payload);
        if (nova >= 0 && nova <= 3) {
            asp.brzina = nova;
            // Ako je brzina 0, ugasi motor 
            if (nova == 0) strncpy(asp.ukljucenost, "OFF",
                                   sizeof(asp.ukljucenost));
            else           strncpy(asp.ukljucenost, "ON",
                                   sizeof(asp.ukljucenost));
            printf(">> [HARDVER] Brzina aspiratora: %d\n", asp.brzina);
            publish_status();
        }

    } else if (strcmp(topic, TOPIC_ASPIRATOR_MOD) == 0) {
        if (strcmp(payload, "AUTO") == 0 || strcmp(payload, "MANUAL") == 0) {
            strncpy(asp.mod, payload, sizeof(asp.mod) - 1);
            printf(">> [HARDVER] Aspirator mod: %s\n", asp.mod);
            publish_status();
        }
    }
}

/* =========================================================
 * MQTT on_connect — subscribe na komandne teme
 * ========================================================= */
static void on_connect(struct mosquitto *m, void *obj, int rc) {
    (void)obj;
    if (rc != 0) {
        fprintf(stderr, "[Aspirator][MQTT] Greska konekcija: %d\n", rc);
        return;
    }
    printf("[Aspirator][MQTT] Povezan na broker. Subscribujem...\n");
    /* Subscribe na sve tri komandne teme (SWR-1.5, SWR-1.6, SWR-1.8) */
    mosquitto_subscribe(m, NULL, TOPIC_ASPIRATOR_UKLJUCENOST, 1);
    mosquitto_subscribe(m, NULL, TOPIC_ASPIRATOR_BRZINA,      1);
    mosquitto_subscribe(m, NULL, TOPIC_ASPIRATOR_MOD,         1);
}

static void init_ssdp(void) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(SSDP_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(g_sock, (struct sockaddr *)&local, sizeof(local));

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    int ttl = 4;
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    g_ssdp_dst.sin_family = AF_INET;
    g_ssdp_dst.sin_port = htons(SSDP_PORT);
    g_ssdp_dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);
}

static void init_mqtt(void) {
    mosquitto_publish(mosq, NULL,TOPIC_ASPIRATOR_STATUS,0, NULL, 1, false);
    mosquitto_lib_init();
    mosq = mosquitto_new(MQTT_ID, true, NULL);
    if (!mosq) { fprintf(stderr, "[Aspirator][MQTT] Greska init\n"); exit(1); }
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[Aspirator][MQTT] Ne mogu se povezati!\n");
}


int main(void) {
    boot_id = (int)time(NULL);
    signal(SIGINT, handle_exit);

    init_ssdp();
    ssdp_notify("ssdp:alive");
    printf("[Aspirator] SSDP aktivan (BOOTID: %d)\n", boot_id);

    init_mqtt();
    printf("[Aspirator] MQTT aktivan. Cekam komande...\n");

    publish_status();

    char   rxbuf[2048];
    time_t last_notify = 0, last_status = 0;

    while (1) {
        time_t now = time(NULL);

        // Periodicni SSDP alive (SWR-1.7) 
        if (now - last_notify >= NOTIFY_INTERVAL) {
            ssdp_notify("ssdp:alive");
            last_notify = now;
        }

        // Periodicni publish statusa svakih 30s (TOPIC_ASPIRATOR_STATUS) 
        if (now - last_status >= 30) {
            publish_status();
            last_status = now;
        }

        // MQTT petlja — prima komande 
        mosquitto_loop(mosq, 100, 1);

        // SSDP prijem M-SEARCH
        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        int n = recvfrom(g_sock, rxbuf, sizeof(rxbuf) - 1, 0,
                         (struct sockaddr *)&sender, &slen);
        if (n > 0) {
            rxbuf[n] = '\0';
            if (strstr(rxbuf, "M-SEARCH") && strstr(rxbuf, AUTH_TOKEN))
                ssdp_respond(&sender);
        }
    }
    return 0;
}
