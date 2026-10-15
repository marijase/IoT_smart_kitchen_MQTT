/*
 * MQTT : publish struje na TOPIC_UTICNICA_STRUJA svakih 10s
 *        publish snage  na TOPIC_UTICNICA_SNAGA  svakih 10s
 *
 * Senzor je iskljucivo PUBLISHER.
 * Logiku zastite (OVERCURRENT/OVERPOWER) donosi KONTROLER.
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
#define DEVICE_USN  "uuid:senzor-uticnice-001::urn:schemas-upnp-org:device:sensor:uticnice"
#define DEVICE_LOC  "http://192.168.1.10:5000/senzor_uticnice.xml"
#define MQTT_ID     "SenzorUticnice-001"

#define PUBLISH_INTERVAL_S 4
#define STRUJA_NORMALNA  8.0f    
#define SNAGA_NORMALNA   1800.0f 

static int               g_sock = -1;
static struct sockaddr_in g_ssdp_dst;
static int               boot_id = 0;
static struct mosquitto *mosq    = NULL;

// SSDP funkcije (iste kao kod senzor_vlage, razlikuje se NT/USN)
static void ssdp_notify(const char *nts) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "NOTIFY * HTTP/1.1\r\nHOST: %s:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\nNT: %s\r\nNTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 SenzorUticnice/1.0\r\nUSN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: ON\r\nAUTH: %s\r\n\r\n",
        SSDP_ADDR, DEVICE_LOC, DEVICE_NT, nts,
        DEVICE_USN, boot_id, AUTH_TOKEN);
    sendto(g_sock, buf, strlen(buf), 0,
           (struct sockaddr *)&g_ssdp_dst, sizeof(g_ssdp_dst));
}

static void ssdp_respond(struct sockaddr_in *dest) {
    char date[64], resp[1024];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nDATE: %s\r\nEXT:\r\n"
        "LOCATION: %s\r\nSERVER: Linux/5.0 UPnP/2.0 SenzorUticnice/1.0\r\n"
        "ST: %s\r\nUSN: %s\r\nBOOTID.UPNP.ORG: %d\r\nCONFIGID.UPNP.ORG: 1\r\n"
        "X-DEVICE-STATUS: ON\r\nAUTH: %s\r\n\r\n",
        date, DEVICE_LOC, DEVICE_NT, DEVICE_USN, boot_id, AUTH_TOKEN);
    sendto(g_sock, resp, strlen(resp), 0,
           (struct sockaddr *)dest, sizeof(*dest));
    printf("[SenzorUticnice][SSDP] Odgovoreno na M-SEARCH\n");
}

static void handle_exit(int sig) {
    (void)sig;
    ssdp_notify("ssdp:byebye");
    printf("\n[SenzorUticnice] Odjavljen. Izlazim.\n");
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    close(g_sock);
    exit(0);
}

/* =========================================================
 * Simulacija senzora struje i snage
 *
 * Normalan rad: struja ~8A, snaga ~1800W.
 * Povremeno (1 u 10 merenja) simulira prekoracenje da bi se mogla testirati zastitna logika kontrolera.
 * ========================================================= */
static float simuliraj_struju(void) {
    static int brojac = 0;
    brojac++;
    /* Svako 10. merenje simulira overcurrent (>16A) za test */
    if (brojac % 10 == 0) {
        printf("[SenzorUticnice] [TEST] Simuliram OVERCURRENT!\n");
        return MAX_STRUJA + 2.0f;
    }

    float sum = ((float)(rand() % 21) - 10) / 10.0f;
    return STRUJA_NORMALNA + sum;
}

static float simuliraj_snagu(float struja) {
    return struja * 230.0f * 0.95f; // P = U * I * cos(phi), pretpostavljamo 230V i cos(phi)=0.95 
    
}

// MQTT — publish struje i snage 
static void mqtt_publish_senzori(void) {
    float struja = simuliraj_struju();
    float snaga  = simuliraj_snagu(struja);

    // negativne vrednosti su greska 
    if (struja < 0.0f || snaga < 0.0f) {
        fprintf(stderr, "[SenzorUticnice][MQTT] Greska: negativna vrednost\n");
        return;
    }

    char buf_struja[16], buf_snaga[16];
    snprintf(buf_struja, sizeof(buf_struja), "%.2f", struja);
    snprintf(buf_snaga,  sizeof(buf_snaga),  "%.2f", snaga);

  
    mosquitto_publish(mosq, NULL, TOPIC_UTICNICA_STRUJA,(int)strlen(buf_struja), buf_struja, 1, false);
    mosquitto_publish(mosq, NULL, TOPIC_UTICNICA_SNAGA, (int)strlen(buf_snaga),  buf_snaga,  1, false);

    printf("[SenzorUticnice][MQTT] Struja: %sA | Snaga: %sW\n",  buf_struja, buf_snaga);
}

// SSDP i MQTT inicijalizacija
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
    if (!mosq) { fprintf(stderr, "[SenzorUticnice][MQTT] Greska init\n"); exit(1); }
    if (mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[SenzorUticnice][MQTT] Ne mogu se povezati!\n");
}

int main(void) {
    srand((unsigned)time(NULL));
    boot_id = (int)time(NULL);
    signal(SIGINT, handle_exit);

    init_ssdp();
    ssdp_notify("ssdp:alive");
    printf("[SenzorUticnice] SSDP aktivan (BOOTID: %d)\n", boot_id);

    init_mqtt();
    printf("[SenzorUticnice] MQTT aktivan. Saljem struju/snagu svakih %ds.\n", PUBLISH_INTERVAL_S);

    char   rxbuf[2048];
    time_t last_notify = 0, last_mqtt = 0;

    while (1) {
        time_t now = time(NULL);

        if (now - last_notify >= NOTIFY_INTERVAL) {
            ssdp_notify("ssdp:alive");
            last_notify = now;
        }

        if (now - last_mqtt >= PUBLISH_INTERVAL_S) {
            mqtt_publish_senzori();
            last_mqtt = now;
        }

        mosquitto_loop(mosq, 0, 1);

        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        int n = recvfrom(g_sock, rxbuf, sizeof(rxbuf) - 1, 0, (struct sockaddr *)&sender, &slen);
        if (n > 0) {
            rxbuf[n] = '\0';
            if (strstr(rxbuf, "M-SEARCH") && strstr(rxbuf, AUTH_TOKEN))
                ssdp_respond(&sender);
        }
    }
    return 0;
}
