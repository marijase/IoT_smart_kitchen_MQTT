/*
 * MQTT : subscribe TOPIC_SVETLO_UKLJUCENOST — ON / OFF (SWR-5.5)
 *        publish  TOPIC_SVETLO_STATUS       — JSON stanje (SWR-5.7, retain=true)
 *
 * SWR-5.6: Simulacija nestanka struje — pri pokretanju svetlo se vraca u OFF stanje (bezbedan default).
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
#define DEVICE_USN  "uuid:sijalica-001::urn:schemas-upnp-org:device:actuator:sijalica"
#define DEVICE_LOC  "http://192.168.1.11:5000/sijalica.xml"
#define MQTT_ID     "Aktuator-Sijalica-001"

// stanje sijalice 
static struct {
    char ukljucenost[4];  // "ON" ili "OFF" 
} sij = { "OFF"};

static int               g_sock = -1;
static struct sockaddr_in g_ssdp_dst;
static int               boot_id = 0;
static struct mosquitto *mosq    = NULL;


static void ssdp_notify(const char *nts) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "NOTIFY * HTTP/1.1\r\nHOST: %s:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\nNT: %s\r\nNTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 Sijalica/1.0\r\nUSN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        SSDP_ADDR, DEVICE_LOC, DEVICE_NT, nts,
        DEVICE_USN, boot_id, sij.ukljucenost, AUTH_TOKEN);
    sendto(g_sock, buf, strlen(buf), 0,
           (struct sockaddr *)&g_ssdp_dst, sizeof(g_ssdp_dst));
}

static void ssdp_respond(struct sockaddr_in *dest) {
    char date[64], resp[1024];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nDATE: %s\r\nEXT:\r\n"
        "LOCATION: %s\r\nSERVER: Linux/5.0 UPnP/2.0 Sijalica/1.0\r\n"
        "ST: %s\r\nUSN: %s\r\nBOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        date, DEVICE_LOC, DEVICE_NT, DEVICE_USN, boot_id,
        sij.ukljucenost, AUTH_TOKEN);
    sendto(g_sock, resp, strlen(resp), 0,
           (struct sockaddr *)dest, sizeof(*dest));
    printf("[Sijalica][SSDP] Odgovoreno na M-SEARCH\n");
}

static void handle_exit(int sig) {
    (void)sig;
    ssdp_notify("ssdp:byebye");
    printf("\n[Sijalica] Odjavljena. Izlazim.\n");
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(g_sock);
    exit(0);
}

/* =========================================================
 * Publish statusa sijalice 
 *
 * JSON format: {"ukljucenost":"ON"}
 * ========================================================= */
static void publish_status(void) {
    char json[64];
    snprintf(json, sizeof(json),
             "{\"ukljucenost\":\"%s\"}",
             sij.ukljucenost);
    // retain=true: nova konekcija odmah dobija trenutno stanje 
    mosquitto_publish(mosq, NULL, TOPIC_SVETLO_STATUS,
                      (int)strlen(json), json, 1, true);
}

/* =========================================================
 * MQTT on_message — prima ON/OFF komande
 * ========================================================= */
static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg) {
    (void)m; (void)obj;
    const char *payload = (const char *)msg->payload;

    printf("[Sijalica][MQTT] %s -> %s\n", msg->topic, payload);

    if (strcmp(msg->topic, TOPIC_SVETLO_UKLJUCENOST) == 0) {
        if (strcmp(payload, "ON") == 0) {
            strncpy(sij.ukljucenost, "ON", sizeof(sij.ukljucenost));

            printf(">> [HARDVER] Sijalica UPALJENA ");
        } else if (strcmp(payload, "OFF") == 0) {
            strncpy(sij.ukljucenost, "OFF", sizeof(sij.ukljucenost));
            printf(">> [HARDVER] Sijalica UGASENA\n");
        }

        publish_status();
    }
}

static void on_connect(struct mosquitto *m, void *obj, int rc) {
    (void)obj;
    if (rc != 0) {
        fprintf(stderr, "[Sijalica][MQTT] Greska konekcija: %d\n", rc);
        return;
    }
    printf("[Sijalica][MQTT] Povezana na broker. Subscribujem...\n");
    mosquitto_subscribe(m, NULL, TOPIC_SVETLO_UKLJUCENOST, 1);
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
    mosquitto_lib_init();
    mosq = mosquitto_new(MQTT_ID, true, NULL);
    if (!mosq) { fprintf(stderr, "[Sijalica][MQTT] Greska init\n"); exit(1); }
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[Sijalica][MQTT] Ne mogu se povezati!\n");
}

int main(void) {
    boot_id = (int)time(NULL);
    signal(SIGINT, handle_exit);

    init_ssdp();
    ssdp_notify("ssdp:alive");
    printf("[Sijalica] SSDP aktivan (BOOTID: %d)\n", boot_id);

    init_mqtt();

    /*
     * SWR-5.6: Simulacija nestanka struje.
     * Pri svakom pokretanju svetlo se vraca u OFF stanje (bezbedan default).
     */
    publish_status();
    printf("[Sijalica] MQTT aktivan. Inicijalno stanje: OFF (SWR-5.6)\n");

    char   rxbuf[2048];
    time_t last_notify = 0;

    while (1) {
        time_t now = time(NULL);

        if (now - last_notify >= NOTIFY_INTERVAL) {
            ssdp_notify("ssdp:alive");
            last_notify = now;
        }

        mosquitto_loop(mosq, 100, 1);

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
