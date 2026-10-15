/*
 * MQTT : publish vrednosti vlage na TOPIC_VLAGA svakih 2s
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


#define DEVICE_NT   "urn:schemas-upnp-org:device:sensor:1"
#define DEVICE_USN  "uuid:senzor-vlage-001::urn:schemas-upnp-org:device:sensor:vlage"
#define DEVICE_LOC  "http://192.168.1.10:5000/senzor_vlage.xml"
#define MQTT_ID     "SenzorVlage-001"


#define PUBLISH_INTERVAL_S 4


static int               g_sock = -1;
static struct sockaddr_in g_ssdp_dst;  
static int               boot_id = 0;
static struct mosquitto *mosq    = NULL;


static void ssdp_notify(const char *nts) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: %s:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\n"
        "NT: %s\r\n"
        "NTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 SenzorVlage/1.0\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: ON\r\n"
        "AUTH: %s\r\n\r\n",
        SSDP_ADDR, DEVICE_LOC, DEVICE_NT, nts,
        DEVICE_USN, boot_id, AUTH_TOKEN);

    if (sendto(g_sock, buf, strlen(buf), 0,
               (struct sockaddr *)&g_ssdp_dst, sizeof(g_ssdp_dst)) < 0)
        perror("[SenzorVlage][SSDP] sendto greska");
}

static void ssdp_respond(struct sockaddr_in *dest) {
    char date[64], resp[1024];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));

    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "DATE: %s\r\n"
        "EXT:\r\n"
        "LOCATION: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 SenzorVlage/1.0\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: ON\r\n"
        "AUTH: %s\r\n\r\n",
        date, DEVICE_LOC, DEVICE_NT, DEVICE_USN, boot_id, AUTH_TOKEN);

    sendto(g_sock, resp, strlen(resp), 0,
           (struct sockaddr *)dest, sizeof(*dest));
    printf("[SenzorVlage][SSDP] Odgovoreno na M-SEARCH\n");
}

static void handle_exit(int sig) {
    (void)sig;
    ssdp_notify("ssdp:byebye");
    printf("\n[SenzorVlage] Odjavljen sa mreze. Izlazim.\n");
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(g_sock);
    exit(0);
}

// Simulacija senzora vlage
static float simuliraj_vlagu(void)
{
    static float v = 10.0f;
    static int smer = 1;   // 1 = raste, -1 = opada

    v += smer * 5.0f;

    if (v >= 95.0f) {
        v = 95.0f;
        smer = -1;
    }
    else if (v <= 10.0f) {
        v = 10.0f;
        smer = 1;
    }

    return v;
}

// MQTT — publish vrednosti vlage 
static void mqtt_publish_vlaga(float vlaga) {
    //validacija opsega [0.0, 100.0] 
    if (vlaga < 0.0f || vlaga > 100.0f) {
        fprintf(stderr, "[SenzorVlage][MQTT] GRESKA: vrednost van opsega: %.2f\n", vlaga);
        mosquitto_publish(mosq, NULL, TOPIC_VLAGA, 5, "ERROR", 1, true);
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", vlaga);


    int rc = mosquitto_publish(mosq, NULL, TOPIC_VLAGA, (int)strlen(buf), buf, 1, true);
    if (rc == MOSQ_ERR_SUCCESS)
        printf("[SenzorVlage][MQTT] Vlaga: %s %%RH\n", buf);
    else
        fprintf(stderr, "[SenzorVlage][MQTT] Greska publish: %d\n", rc);
}

// SSDP socket inicijalizacija
static void init_ssdp(void) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { perror("socket"); exit(1); }

    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif


    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind na SSDP port 
    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(SSDP_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&local, sizeof(local)) < 0)
        perror("[SenzorVlage][SSDP] bind");


    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Multicast TTL i loopback (za testiranje na istoj masini) 
    int ttl = 4;
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Odredisna adresa za NOTIFY poruke 
    g_ssdp_dst.sin_family      = AF_INET;
    g_ssdp_dst.sin_port        = htons(SSDP_PORT);
    g_ssdp_dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);
}

// MQTT inicijalizacija — senzor je samo publisher
static void init_mqtt(void) {
    mosquitto_lib_init();
    mosq = mosquitto_new(MQTT_ID, true, NULL);
    if (!mosq) { 
    	fprintf(stderr, "[SenzorVlage][MQTT] Greska init\n"); exit(1); 
    	}
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[SenzorVlage][MQTT] Ne mogu se povezati na broker!\n");
}

int main(void) {
    srand((unsigned)time(NULL));
    boot_id = (int)time(NULL);
    signal(SIGINT, handle_exit);

    //SSDP inicijalizacija i prijava na mrezu 
    init_ssdp();
    ssdp_notify("ssdp:alive");
    sleep(1);
    printf("[SenzorVlage] SSDP aktivan (BOOTID: %d)\n", boot_id);


    init_mqtt();
    printf("[SenzorVlage] MQTT aktivan. Saljem vlagu svakih %ds.\n", PUBLISH_INTERVAL_S);

    char   rxbuf[2048];
    time_t last_notify = 0;
    time_t last_mqtt   = 0;

    while (1) {
        time_t now = time(NULL);

        // SSDP: periodicni alive 
        if (now - last_notify >= NOTIFY_INTERVAL) {
            ssdp_notify("ssdp:alive");
            last_notify = now;
        }

        // MQTT: publish vlaga svakih PUBLISH_INTERVAL_S
        if (now - last_mqtt >= PUBLISH_INTERVAL_S) {
            mqtt_publish_vlaga(simuliraj_vlagu());
            last_mqtt = now;
        }
        mosquitto_loop(mosq, 0, 1);


        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        int n = recvfrom(g_sock, rxbuf, sizeof(rxbuf) - 1, 0, (struct sockaddr *)&sender, &slen);
        if (n > 0) {
            rxbuf[n] = '\0';
            // Prihvati samo poruke sa ispravnim AUTH tokenom 
            if (strstr(rxbuf, "M-SEARCH") && strstr(rxbuf, AUTH_TOKEN))
                ssdp_respond(&sender);
        }
    }
    return 0;
}
