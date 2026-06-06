#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>    //  za boot_id 

#ifndef _IP_MREQ_DEFINED
struct my_ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};
#define ip_mreq my_ip_mreq
#endif

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define NOTIFY_NT "urn:schemas-upnp-org:device:actuator:1"
#define NOTIFY_USN "uuid:light-bulb-001::urn:schemas-upnp-org:device:actuator:1"
#define AUTH_TOKEN "smart_kitchen_pass"
#define LOCATION_URL "http://192.168.1.11:5000/desc.xml"


int g_sock = -1;
struct sockaddr_in g_addr;
int boot_id = 0; // Ovde je definisan boot_id

// Funkcija za slanje NOTIFY poruka
void send_notify(const char* nts_type) {
    char message[1024];
    snprintf(message, sizeof(message),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: %s:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\n"
        "NT: %s\r\n"
        "NTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 Aktuator/1.0\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "SEARCHPORT.UPNP.ORG: 1900\r\n"
        "X-DEVICE-STATUS: ON\r\n"  // NOVO: ON/OFF stanje
        "AUTH: %s\r\n"             // Tvoj filter (Kontrola pristupa) DODANO
        "\r\n", 
        SSDP_ADDR, LOCATION_URL, NOTIFY_NT, nts_type, NOTIFY_USN, boot_id, AUTH_TOKEN);
       

    int ret = sendto(g_sock, message, strlen(message), 0, (struct sockaddr*)&g_addr, sizeof(g_addr));
     if (ret < 0) {
        perror("[Aktuator] Greška pri slanju NOTIFY");
    }
}
void handle_exit(int sig) {
    // Provera koji je signal doveo do prekida
    if (sig == SIGINT) {
        printf("\n[Aktuator] Primljen SIGINT (Ctrl+C). Gasim...\n");
    } else if (sig == SIGHUP) {
        printf("\n[Aktuator] Terminal zatvoren (SIGHUP). Gasim...\n");
    }

    send_notify("ssdp:byebye");      // Ovo je ključno da bi aktuator nestao sa liste u aplikaciji
    
    
    // printf("\n[Aktuator] %s se uspesno odjavio sa mreže.\n", g_usn);
    // ovo dodati kada preko argumenata odaberemo koji nam je aktuator, i ispisace se koji aktuator se odjavljuje (npr. sijalica-001)
    

    close(g_sock);
    exit(0);
}

void get_http_date(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    // Format: Dan, Datum Mesec Godina Sat:Minut:Sekund GMT
    strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}
int main() {
    // Inicijalizacija boot_id-a preko trenutnog vremena
    boot_id = (int)time(NULL); 

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    int reuse = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv;
    tv.tv_sec = 5; 
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SSDP_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(g_sock, (struct sockaddr*)&local_addr, sizeof(local_addr));

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    g_addr.sin_family = AF_INET;
    g_addr.sin_port = htons(SSDP_PORT);
    g_addr.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    signal(SIGINT, handle_exit);
    signal(SIGHUP, handle_exit);

    printf("Aktuator online (BOOTID: %d). Čekam upite...\n", boot_id);

    char buffer[2048];
    time_t last_notify_time = 0; // DODANO

    while (1) {
        time_t now = time(NULL); // DODAJ OVO

        // DODANO Šalji alive svakih 7-10 sekundi
        if (now - last_notify_time >= 8) { 
            send_notify("ssdp:alive");
            printf("[AKTUATOR] Poslat periodični NOTIFY:alive (BOOTID: %d)\n", boot_id);
            last_notify_time = now;
        }
        //DODANO
        struct sockaddr_in client;
        socklen_t addr_len = sizeof(client);
        int len = recvfrom(g_sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client, &addr_len);
        
        if (len > 0) {
            buffer[len] = '\0';
            // Kontrola pristupa preko tokena  FTN_Sifra_2024
            if (strstr(buffer, "M-SEARCH") && strstr(buffer, AUTH_TOKEN)) {
               char date_buf[64];
                get_http_date(date_buf, sizeof(date_buf));
                
                char resp[1024]; 
                snprintf(resp, sizeof(resp), 
                    "HTTP/1.1 200 OK\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "DATE: %s\r\n"
                    "EXT:\r\n"                                 
                    "LOCATION: %s\r\n"
                    "SERVER: Linux/5.0 UPnP/2.0 Aktuator/1.0\r\n"
                    "ST: %s\r\n"
                    "USN: %s\r\n"
                    "BOOTID.UPNP.ORG: %d\r\n"
                    "CONFIGID.UPNP.ORG: 1\r\n"        // Konfiguracioni broj
                    "SEARCHPORT.UPNP.ORG: 1900\r\n"   // Port za Unicast M-SEARCH
                    "X-DEVICE-STATUS: ON\r\n"  // NOVO: ON/OFF stanje
                    "AUTH: %s\r\n"             // Tvoj filter (Kontrola pristupa) DODANO
                    "\r\n", 
                    date_buf, LOCATION_URL, NOTIFY_NT, NOTIFY_USN, boot_id, AUTH_TOKEN);


                sendto(g_sock, resp, strlen(resp), 0, (struct sockaddr*)&client, addr_len);
                printf("Odgovoreno autorizovanom kontroleru.\n");
            }
        }
    }
    return 0;
}
