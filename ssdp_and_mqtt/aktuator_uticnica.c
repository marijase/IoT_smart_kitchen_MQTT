/*
 * MQTT : subscribe TOPIC_UTICNICA_UKLJUCENOST — ON / OFF (SWR-3.4, ARCH-3.4.1)
 *        publish  TOPIC_UTICNICA_STATUS       — OK/OVERCURRENT/OVERPOWER
 *                 (inicijalno OK, retain=true, SWR-4.3)
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
#define DEVICE_USN  "uuid:uticnica-001::urn:schemas-upnp-org:device:actuator:uticnica"
#define DEVICE_LOC  "http://192.168.1.11:5000/uticnica.xml"
#define MQTT_ID     "Aktuator-Uticnica-001"

//stanje uticnice
static struct {
    char ukljucenost[4];   // "ON" ili "OFF"                    
    char status[16];       // "OK", "OVERCURRENT", "OVERPOWER"  
} utc = { "OFF", "OK" };

static int               g_sock = -1;
static struct sockaddr_in g_ssdp_dst;
static int               boot_id = 0;
static struct mosquitto *mosq    = NULL;

static void ssdp_notify(const char *nts) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "NOTIFY * HTTP/1.1\r\nHOST: %s:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\nNT: %s\r\nNTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 Uticnica/1.0\r\nUSN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        SSDP_ADDR, DEVICE_LOC, DEVICE_NT, nts,
        DEVICE_USN, boot_id, utc.ukljucenost, AUTH_TOKEN);
    sendto(g_sock, buf, strlen(buf), 0,
           (struct sockaddr *)&g_ssdp_dst, sizeof(g_ssdp_dst));
}

static void ssdp_respond(struct sockaddr_in *dest) {
    char date[64], resp[1024];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nDATE: %s\r\nEXT:\r\n"
        "LOCATION: %s\r\nSERVER: Linux/5.0 UPnP/2.0 Uticnica/1.0\r\n"
        "ST: %s\r\nUSN: %s\r\nBOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: %s\r\nAUTH: %s\r\n\r\n",
        date, DEVICE_LOC, DEVICE_NT, DEVICE_USN, boot_id,
        utc.ukljucenost, AUTH_TOKEN);
    sendto(g_sock, resp, strlen(resp), 0,
           (struct sockaddr *)dest, sizeof(*dest));
    printf("[Uticnica][SSDP] Odgovoreno na M-SEARCH\n");
}

static void handle_exit(int sig) {
    (void)sig;
    ssdp_notify("ssdp:byebye");
    printf("\n[Uticnica] Odjavljena. Izlazim.\n");
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(g_sock);
    exit(0);
}


static void publish_status(void) {
    mosquitto_publish(mosq, NULL, TOPIC_UTICNICA_STATUS,
                      (int)strlen(utc.status), utc.status, 1, true);
}

/* =========================================================
 * MQTT on_message — prima ON/OFF komande 
 * ========================================================= */
static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg) {
    (void)m; (void)obj;
    const char *payload = (const char *)msg->payload;

    printf("[Uticnica][MQTT] %s -> %s\n", msg->topic, payload);

    if (strcmp(msg->topic, TOPIC_UTICNICA_UKLJUCENOST) == 0) {
        if (strcmp(payload, "ON") == 0) {
            strncpy(utc.ukljucenost, "ON", sizeof(utc.ukljucenost));
            strncpy(utc.status,      "OK", sizeof(utc.status));
            printf(">> [HARDVER] Uticnica UKLJUCENA\n");
        } else if (strcmp(payload, "OFF") == 0) {
            strncpy(utc.ukljucenost, "OFF", sizeof(utc.ukljucenost));
            printf(">> [HARDVER] Uticnica ISKLJUCENA\n");
        }
        publish_status();
    }
}

static void on_connect(struct mosquitto *m, void *obj, int rc) {
    (void)obj;
    if (rc != 0) {
        fprintf(stderr, "[Uticnica][MQTT] Greska konekcija: %d\n", rc);
        return;
    }
    printf("[Uticnica][MQTT] Povezana na broker. Subscribujem...\n");
    // Subscribe na komandnu temu 
    mosquitto_subscribe(m, NULL, TOPIC_UTICNICA_UKLJUCENOST, 1);
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
    if (!mosq) { fprintf(stderr, "[Uticnica][MQTT] Greska init\n"); exit(1); }
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[Uticnica][MQTT] Ne mogu se povezati!\n");
}

int main(void) {
    boot_id = (int)time(NULL);
    signal(SIGINT, handle_exit);

    init_ssdp();
    ssdp_notify("ssdp:alive");
    printf("[Uticnica] SSDP aktivan (BOOTID: %d)\n", boot_id);

    init_mqtt();
    printf("[Uticnica] MQTT aktivan. Cekam komande...\n");


    publish_status();

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
