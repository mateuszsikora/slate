/*
 * Slate — the captive DNS responder of §9.2.
 *
 * "A DNS responder answering every query with the device address, so phones
 * open the page unprompted." That is the whole specification, and the reason it
 * works is that answering every query is how a captive portal is detected in
 * the first place: the phone asks for a name it knows the answer to, gets ours,
 * fetches the probe URL and finds something that is not what it expected.
 *
 * §9.2 also says what this is not: "The captive portal is best-effort and never
 * the only way in." Both iOS and Android will label the network as having no
 * internet and offer to leave it, which is why the address is printed on the
 * screen. Nothing in this file is allowed to be load-bearing.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "slate_api.h"
#include "slate_setup_private.h"

static const char *TAG = "setup.dns";

#define DNS_PORT 53

/* 512 is the classic DNS-over-UDP limit and every query this will ever see is a
 * fraction of it. A larger one is refused rather than parsed: this answers
 * `captive.apple.com`, not the internet. The tail is room for the answer, which
 * is written into the same buffer after the question. */
#define QUERY_MAX  512
#define ANSWER_MAX 16

/*
 * Short, because the answer is a lie that must not outlive the network it was
 * told on. A phone that keeps `captive.apple.com` pointed at 192.168.4.1 after
 * it has left the panel's access point has a broken portal check on the next
 * network it joins, and the person will not connect that to a panel they
 * configured yesterday. Ten seconds is longer than a portal handshake and
 * shorter than walking away.
 */
#define ANSWER_TTL_S 10

#define TASK_STACK      3072
#define TASK_PRIORITY   4
#define RECV_TIMEOUT_MS 500
#define STOP_TIMEOUT_MS 3000

static int s_socket = -1;
static volatile bool s_stop;
static SemaphoreHandle_t s_stopped;

/* --- The wire format ---------------------------------------------------- */

enum {
    HEADER_BYTES = 12,
    FLAGS_HI = 2,
    FLAGS_LO = 3,
    QDCOUNT_HI = 4,
    ANCOUNT_HI = 6,
    NSCOUNT_HI = 8,
    ARCOUNT_HI = 10,

    QR_RESPONSE = 0x80,
    AA_AUTHORITATIVE = 0x04,
    RD_MASK = 0x01,
    RA_AVAILABLE = 0x80,

    TYPE_A = 1,
    CLASS_IN = 1,

    LABEL_MAX = 63,
};

/**
 * Walk the single question and return the offset just past it, or 0.
 *
 * Deliberately strict. This runs on an open network in whatever room the panel
 * is in, and the one thing it must not do is read past the datagram it was
 * given: a compression pointer in a question (the high bits of a length byte)
 * is not legal there and is the shape a malformed query would use to make this
 * follow an offset of its choosing, so a length above 63 ends the parse rather
 * than being dereferenced.
 */
static size_t question_end(const uint8_t *query, size_t len)
{
    size_t at = HEADER_BYTES;

    while (at < len && query[at] != 0) {
        if (query[at] > LABEL_MAX) {
            return 0;
        }
        at += (size_t) query[at] + 1;
    }
    if (at >= len) {
        return 0; /* no root label — the name is truncated */
    }
    at++;

    /* QTYPE and QCLASS. */
    return at + 4 <= len ? at + 4 : 0;
}

/**
 * Turn a query into a response in place.
 *
 * @return the length of the response, or 0 if the query is not one to answer.
 */
static size_t build_response(uint8_t *packet, size_t len, uint32_t address)
{
    if (len < HEADER_BYTES + 1 || len > QUERY_MAX) {
        return 0;
    }

    /* A response arriving on port 53 is somebody else's business, and an opcode
     * other than QUERY is not something this understands well enough to refuse
     * politely. Both are dropped. */
    if ((packet[FLAGS_HI] & QR_RESPONSE) != 0 || (packet[FLAGS_HI] & 0x78) != 0) {
        return 0;
    }
    /* Exactly one question. Zero is nothing to answer and more than one cannot
     * be answered with a single record. */
    if (packet[QDCOUNT_HI] != 0 || packet[QDCOUNT_HI + 1] != 1) {
        return 0;
    }

    size_t end = question_end(packet, len);
    if (end == 0) {
        return 0;
    }

    uint16_t qtype = (uint16_t) (packet[end - 4] << 8 | packet[end - 3]);
    uint16_t qclass = (uint16_t) (packet[end - 2] << 8 | packet[end - 1]);
    bool answerable = qtype == TYPE_A && qclass == CLASS_IN;

    packet[FLAGS_HI] = QR_RESPONSE | AA_AUTHORITATIVE | (packet[FLAGS_HI] & RD_MASK);
    packet[FLAGS_LO] = RA_AVAILABLE;
    packet[ANCOUNT_HI] = 0;
    packet[ANCOUNT_HI + 1] = answerable ? 1 : 0;

    /* Anything the query carried past the question is dropped, so the counts
     * that describe it have to go with it. An EDNS OPT record in the additional
     * section is the common case, and a response that claimed one and did not
     * carry it is a malformed answer rather than a smaller one. */
    packet[NSCOUNT_HI] = 0;
    packet[NSCOUNT_HI + 1] = 0;
    packet[ARCOUNT_HI] = 0;
    packet[ARCOUNT_HI + 1] = 0;

    if (!answerable) {
        /*
         * NOERROR with no answer, which is the right refusal for the AAAA query
         * every phone sends alongside the A one. NXDOMAIN would be a claim that
         * the name does not exist and takes some resolvers off IPv4 for the same
         * name; a silent drop costs the client its full timeout before it tries
         * anything else.
         */
        return end;
    }

    uint8_t *answer = packet + end;
    /* A pointer to the question's name rather than a copy of it: the name is at
     * a fixed offset in a single-question message, and the alternative is
     * re-encoding a string this file never had a reason to decode. */
    answer[0] = 0xC0;
    answer[1] = HEADER_BYTES;
    answer[2] = 0;
    answer[3] = TYPE_A;
    answer[4] = 0;
    answer[5] = CLASS_IN;
    answer[6] = 0;
    answer[7] = 0;
    answer[8] = (uint8_t) (ANSWER_TTL_S >> 8);
    answer[9] = (uint8_t) ANSWER_TTL_S;
    answer[10] = 0;
    answer[11] = 4;
    /* The address is already in network order — it came out of esp_netif. */
    memcpy(answer + 12, &address, 4);

    return end + ANSWER_MAX;
}

/* --- The task ----------------------------------------------------------- */

static void dns_task(void *arg)
{
    const uint32_t address = (uint32_t) (uintptr_t) arg;
    uint8_t packet[QUERY_MAX + ANSWER_MAX];

    ESP_LOGI(TAG, "answering every query with %s", SLATE_SETUP_AP_ADDRESS);

    while (!s_stop) {
        struct sockaddr_storage from = {0};
        socklen_t from_len = sizeof(from);

        int received = recvfrom(s_socket, packet, QUERY_MAX, 0, (struct sockaddr *) &from,
                               &from_len);
        if (received <= 0) {
            continue; /* the receive timeout, which is how s_stop is noticed */
        }

        size_t len = build_response(packet, (size_t) received, address);
        if (len == 0) {
            continue;
        }
        sendto(s_socket, packet, len, 0, (struct sockaddr *) &from, from_len);
    }

    close(s_socket);
    s_socket = -1;
    ESP_LOGI(TAG, "stopped");
    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

esp_err_t slate_setup_dns_start(void)
{
    if (s_socket >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_stopped == NULL) {
        s_stopped = xSemaphoreCreateBinary();
        if (s_stopped == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    /*
     * Bound to the access point's address, not to every interface. A responder
     * on 0.0.0.0 would answer the household's DNS queries with 192.168.4.1 for
     * as long as the panel is on the LAN — and it would do it from the interface
     * §9.4 keeps trying to bring up, so the worse the network is the more of the
     * house it would break.
     */
    const struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = esp_ip4addr_aton(SLATE_SETUP_AP_ADDRESS),
    };

    /* Non-blocking is not what is wanted here — a bounded block is, so the task
     * spends its time in recvfrom() rather than spinning, and still notices a
     * teardown twice a second. */
    const struct timeval timeout = {.tv_usec = RECV_TIMEOUT_MS * 1000};

    if (bind(sock, (const struct sockaddr *) &local, sizeof(local)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "cannot listen on %s:%d — errno %d", SLATE_SETUP_AP_ADDRESS, DNS_PORT,
                 errno);
        close(sock);
        return ESP_FAIL;
    }

    s_stop = false;
    s_socket = sock;

    if (xTaskCreate(dns_task, "slate_dns", TASK_STACK, (void *) (uintptr_t) local.sin_addr.s_addr,
                    TASK_PRIORITY, NULL) != pdPASS) {
        close(sock);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void slate_setup_dns_stop(void)
{
    if (s_socket < 0) {
        return;
    }

    /*
     * The socket is closed by the task and not from here. Closing a descriptor
     * another task is blocked in recvfrom() on is the kind of race that works
     * every time it is tested and then hands the number to whatever opens the
     * next socket — which on this device is the HTTP server or the Home
     * Assistant client.
     */
    s_stop = true;
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "the responder did not stop; leaving it and its socket alone");
    }
}
