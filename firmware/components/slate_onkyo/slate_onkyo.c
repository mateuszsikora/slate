/*
 * Slate — the Onkyo/Integra eISCP provider. See include/slate_onkyo.h.
 *
 * One task owns every socket and the device tables, exactly as §5.9's adapter
 * does and for the same reason: §5.1 calls `subscribe()` on the UI task, so
 * anything this component holds across a network wait would freeze the screen.
 * The handover is the same shape — `subscribe()` builds the replacement tables
 * on its own stack and swaps them in under a lock held for a few pointer moves.
 *
 * What is genuinely different is the direction. `shelly` asks and waits; a
 * receiver talks. Turning the volume knob on the front panel produces an `MVL`
 * frame nobody requested, so the task sits in `select()` and publishes what
 * arrives. Questions are asked once per connection and then only as a slow
 * safety net, because the interesting events are pushed.
 */

#include "slate_onkyo.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "slate_action.h"
#include "slate_state.h"
#include "slate_wifi.h"

static const char *TAG = "slate_onkyo";

#define TASK_STACK    5120
#define TASK_PRIORITY 4
#define EISCP_PORT    "60128"

/*
 * How long the task blocks in select() before looking at the command queue,
 * which is a FreeRTOS queue and therefore not something select() can wait on.
 * It is the whole of the latency a tap inherits from this adapter, and §5.3
 * allows three seconds, so 200 ms is generous in the direction that costs
 * nothing: the task is blocked, not spinning.
 */
#define SELECT_TIMEOUT_MS 200

/* A connect() is the one blocking call here. It is only ever attempted when no
 * command is waiting, so a receiver that has been unplugged cannot delay a tap
 * meant for one that has not. */
#define CONNECT_TIMEOUT_MS 2000
#define BACKOFF_MIN_MS     2000
#define BACKOFF_MAX_MS     30000

/*
 * Everything the receiver pushes is a few dozen bytes. 512 is room for the
 * longest of them several times over, and it is a ceiling rather than a growing
 * buffer: `NRI` answers with kilobytes of XML that this adapter never asks for,
 * and a device sending something that size is one it does not understand.
 */
#define RX_MAX   512
#define HOST_MAX SLATE_RESOURCE_ID_MAX

/* The receiver is asked outright once per connection, and then this often as a
 * safety net against a pushed frame that was never sent or never arrived. */
#define REFRESH_INTERVAL_US (60 * 1000 * 1000LL)

#define COMMAND_QUEUE_LEN 8

/* --- The binding set, parsed ---------------------------------------------- */

typedef enum {
    ROLE_MAIN = 0,     /**< the receiver, published as a `light` */
    ROLE_INPUT,        /**< the selected input, by name, as a `sensor` */
    ROLE_INPUT_SELECT, /**< a `scene` that selects one input */
    ROLE_MUTE,         /**< a `scene` that toggles mute */
} role_t;

typedef struct {
    char host[HOST_MAX + 1];
    int fd; /**< -1 while not connected */
    uint8_t rx[RX_MAX];
    size_t rx_len;
    int64_t retry_at_us;
    uint32_t backoff_ms;
    int64_t refresh_at_us;

    /* What the receiver last said. `known` is the difference between "off" and
     * "we have not been told", and §5.2 has no third value for a light — so
     * nothing is published until the receiver has answered once. */
    bool known;
    bool power;
    uint8_t volume;
    bool muted;
    char input[3]; /**< the eISCP selector code, lowercase; empty until known */
} device_t;

typedef struct {
    char id[SLATE_RESOURCE_ID_MAX + 1];
    uint16_t device;
    role_t role;
    char code[3]; /**< ROLE_INPUT_SELECT only */
} entry_t;

typedef enum {
    CMD_SWEEP = 0, /**< a rebuild asking to be noticed now */
    CMD_ACTION,
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char resource[SLATE_RESOURCE_ID_MAX + 1];
    uint32_t action_id;
    slate_action_t action;
    int32_t number;
    bool boolean;
} command_t;

/* Owned by the connection task alone. */
static device_t *s_devices;
static entry_t *s_entries;
static size_t s_device_count;
static size_t s_entry_count;

/* The handover, held for pointer moves and never across a network wait. */
static SemaphoreHandle_t s_bind_lock;
static StaticSemaphore_t s_bind_lock_storage;
static device_t *s_pending_devices;
static entry_t *s_pending_entries;
static size_t s_pending_device_count;
static size_t s_pending_entry_count;
static bool s_pending_valid;

static QueueHandle_t s_commands;
static TaskHandle_t s_task;
static volatile bool s_network_up;
static volatile bool s_has_bindings;
static bool s_initialized;

/* --- eISCP ---------------------------------------------------------------- */

/*
 * "ISCP", a four-byte header size, a four-byte data size, a version byte and
 * three reserved bytes; then `!1`, a three-letter command, its parameter and a
 * terminator. The header size is read rather than assumed to be 16, because it
 * is a field and a device is entitled to make it larger.
 */
#define EISCP_HEADER_MIN 16

static const char *input_name(const char *code)
{
    static const struct {
        const char *code;
        const char *name;
    } NAMES[] = {
        {"00", "VCR/DVR"},  {"01", "CBL/SAT"}, {"02", "Game"},   {"03", "Aux"},
        {"05", "PC"},       {"10", "BD/DVD"},  {"12", "TV"},     {"20", "TV/Tape"},
        {"22", "Phono"},    {"23", "CD"},      {"24", "FM"},     {"25", "AM"},
        {"26", "Tuner"},    {"29", "USB"},     {"2b", "Net"},    {"2e", "Bluetooth"},
        {"33", "DAB"},
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strcasecmp(NAMES[i].code, code) == 0) {
            return NAMES[i].name;
        }
    }
    return NULL;
}

static bool send_command(device_t *device, const char *command)
{
    if (device->fd < 0) {
        return false;
    }
    char message[32];
    int body = snprintf(message, sizeof(message), "!1%s\r", command);
    if (body <= 0 || (size_t)body >= sizeof(message)) {
        return false;
    }
    uint8_t frame[16 + sizeof(message)];
    memcpy(frame, "ISCP", 4);
    frame[4] = 0; frame[5] = 0; frame[6] = 0; frame[7] = EISCP_HEADER_MIN;
    frame[8] = (uint8_t)(body >> 24); frame[9] = (uint8_t)(body >> 16);
    frame[10] = (uint8_t)(body >> 8); frame[11] = (uint8_t)body;
    frame[12] = 1; frame[13] = 0; frame[14] = 0; frame[15] = 0;
    memcpy(frame + 16, message, (size_t)body);

    size_t total = 16 + (size_t)body;
    ssize_t written = send(device->fd, frame, total, 0);
    return written == (ssize_t)total;
}

/**
 * @brief Apply one `!1CMDvalue` payload to the cached receiver state.
 * @return true when something a tile shows actually changed.
 */
static bool apply_message(device_t *device, const char *message)
{
    if (strncmp(message, "!1", 2) != 0 || strlen(message) < 5) {
        return false;
    }
    const char *command = message + 2;
    const char *value = message + 5;
    bool changed = false;

    if (strncmp(command, "PWR", 3) == 0) {
        bool power = value[0] == '0' && value[1] == '1';
        changed = !device->known || device->power != power;
        device->power = power;
    } else if (strncmp(command, "MVL", 3) == 0) {
        char digits[3] = {value[0], value[1], '\0'};
        char *end = NULL;
        long level = strtol(digits, &end, 16);
        if (end == digits || level < 0 || level > 0xFF) {
            return false;
        }
        changed = !device->known || device->volume != (uint8_t)level;
        device->volume = (uint8_t)level;
    } else if (strncmp(command, "AMT", 3) == 0) {
        bool muted = value[0] == '0' && value[1] == '1';
        changed = !device->known || device->muted != muted;
        device->muted = muted;
    } else if (strncmp(command, "SLI", 3) == 0) {
        char code[3] = {(char)tolower((unsigned char)value[0]),
                        (char)tolower((unsigned char)value[1]), '\0'};
        changed = !device->known || strcmp(device->input, code) != 0;
        memcpy(device->input, code, sizeof(code));
    } else {
        /* A receiver volunteers plenty this adapter has no tile for — now
         * playing, tuner presets, listening modes. Ignoring them is not a gap:
         * §5.2's four kinds are what a component can render. */
        return false;
    }
    device->known = true;
    return changed;
}

/**
 * @brief Pull whole frames out of the receive buffer.
 * @return true when any of them changed something worth publishing.
 *
 * A stream protocol hands you halves of frames, so this consumes only what has
 * fully arrived and leaves the remainder for the next read. It also resynchs on
 * `ISCP` rather than trusting the buffer to start on a boundary: one malformed
 * length would otherwise desynchronise the connection permanently.
 */
static bool consume(device_t *device)
{
    bool changed = false;
    size_t offset = 0;

    while (device->rx_len - offset >= EISCP_HEADER_MIN) {
        const uint8_t *base = device->rx + offset;
        size_t available = device->rx_len - offset;

        if (memcmp(base, "ISCP", 4) != 0) {
            offset++; /* resynchronise a byte at a time */
            continue;
        }
        uint32_t header = ((uint32_t)base[4] << 24) | ((uint32_t)base[5] << 16) |
                          ((uint32_t)base[6] << 8) | base[7];
        uint32_t data = ((uint32_t)base[8] << 24) | ((uint32_t)base[9] << 16) |
                        ((uint32_t)base[10] << 8) | base[11];
        if (header < EISCP_HEADER_MIN || header > RX_MAX || data > RX_MAX) {
            /* Not a frame this adapter can hold — an `NRI` answer is kilobytes
             * of XML. Skip the magic and resynchronise rather than waiting
             * forever for bytes that will not fit. */
            offset += 4;
            continue;
        }
        if (available < header + data) {
            break; /* the rest of it has not arrived yet */
        }

        char message[64];
        size_t length = data < sizeof(message) - 1 ? data : sizeof(message) - 1;
        memcpy(message, base + header, length);
        message[length] = '\0';
        for (char *c = message; *c != '\0'; c++) {
            if (*c == '\r' || *c == '\n' || *c == 0x1A) {
                *c = '\0';
                break;
            }
        }
        changed |= apply_message(device, message);
        offset += header + data;
    }

    if (offset > 0) {
        memmove(device->rx, device->rx + offset, device->rx_len - offset);
        device->rx_len -= offset;
    }
    if (device->rx_len == RX_MAX) {
        device->rx_len = 0; /* nothing parseable in a full buffer: start over */
    }
    return changed;
}

/* --- Resource ids --------------------------------------------------------- */

static bool parse_resource(const char *id, char *host, role_t *role, char *code)
{
    const char *slash = strchr(id, '/');
    if (slash == NULL || slash == id) {
        return false;
    }
    size_t host_len = (size_t)(slash - id);
    if (host_len > HOST_MAX) {
        return false;
    }
    memcpy(host, id, host_len);
    host[host_len] = '\0';
    code[0] = '\0';

    const char *tail = slash + 1;
    if (strcmp(tail, "main") == 0) {
        *role = ROLE_MAIN;
        return true;
    }
    if (strcmp(tail, "input") == 0) {
        *role = ROLE_INPUT;
        return true;
    }
    if (strcmp(tail, "mute") == 0) {
        *role = ROLE_MUTE;
        return true;
    }
    if (strncmp(tail, "input:", 6) == 0) {
        const char *digits = tail + 6;
        if (strlen(digits) != 2 || !isxdigit((unsigned char)digits[0]) ||
            !isxdigit((unsigned char)digits[1])) {
            return false;
        }
        code[0] = (char)tolower((unsigned char)digits[0]);
        code[1] = (char)tolower((unsigned char)digits[1]);
        code[2] = '\0';
        *role = ROLE_INPUT_SELECT;
        return true;
    }
    return false;
}

/* --- Publication ---------------------------------------------------------- */

static void publish_device(size_t device_index)
{
    const device_t *device = &s_devices[device_index];
    bool available = device->fd >= 0 && device->known;

    for (size_t i = 0; i < s_entry_count; i++) {
        const entry_t *entry = &s_entries[i];
        if (entry->device != device_index) {
            continue;
        }
        /* Nothing is published before the receiver has answered once. §5.2 has
         * no spelling for "not told yet", and a light defaulting to off would
         * be a claim rather than an absence — §5.9 settled the same question
         * the same way, and the tile keeps §3.3's placeholder until then. */
        if (!device->known) {
            continue;
        }

        slate_snapshot_t snapshot = {.resource = entry->id, .available = available};
        switch (entry->role) {
        case ROLE_MAIN:
            snapshot.kind = SLATE_KIND_LIGHT;
            snapshot.state.light.on = device->power;
            snapshot.state.light.brightness =
                device->volume > 100 ? 100 : (int16_t)device->volume;
            snapshot.state.light.color_temperature = SLATE_STATE_ABSENT;
            snapshot.capabilities.actions = (1u << SLATE_ACTION_TOGGLE) |
                                            (1u << SLATE_ACTION_SET_POWER) |
                                            (1u << SLATE_ACTION_SET_BRIGHTNESS);
            break;
        case ROLE_INPUT: {
            const char *name = input_name(device->input);
            snapshot.kind = SLATE_KIND_SENSOR;
            snapshot.state.sensor.numeric = false;
            if (name != NULL) {
                snprintf(snapshot.state.sensor.text, sizeof(snapshot.state.sensor.text),
                         "%s", name);
            } else {
                /* An input this table does not name still has a code, and the
                 * code is what the remote control shows too. */
                snprintf(snapshot.state.sensor.text, sizeof(snapshot.state.sensor.text),
                         "%s", device->input[0] != '\0' ? device->input : "—");
            }
            break;
        }
        case ROLE_INPUT_SELECT:
        case ROLE_MUTE:
            snapshot.kind = SLATE_KIND_SCENE;
            snapshot.capabilities.actions = 1u << SLATE_ACTION_ACTIVATE;
            break;
        }

        esp_err_t err = slate_state_publish(SLATE_ONKYO_PROVIDER_ID, &snapshot);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "publish %s: %s", entry->id, esp_err_to_name(err));
        }
    }
}

/* --- Connection ----------------------------------------------------------- */

static void disconnect(device_t *device, const char *why)
{
    if (device->fd >= 0) {
        ESP_LOGW(TAG, "%s: %s", device->host, why);
        close(device->fd);
        device->fd = -1;
    }
    device->rx_len = 0;
    device->backoff_ms = device->backoff_ms == 0
                             ? BACKOFF_MIN_MS
                             : (device->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS
                                                                       : device->backoff_ms * 2);
    device->retry_at_us = esp_timer_get_time() + (int64_t)device->backoff_ms * 1000;
}

static void connect_device(device_t *device)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *found = NULL;
    if (getaddrinfo(device->host, EISCP_PORT, &hints, &found) != 0 || found == NULL) {
        disconnect(device, "cannot be resolved");
        return;
    }

    int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(found);
        disconnect(device, "no socket");
        return;
    }
    struct timeval timeout = {.tv_sec = CONNECT_TIMEOUT_MS / 1000,
                              .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int result = connect(fd, found->ai_addr, found->ai_addrlen);
    freeaddrinfo(found);
    if (result != 0) {
        close(fd);
        disconnect(device, "did not accept a connection");
        return;
    }

    device->fd = fd;
    device->rx_len = 0;
    device->backoff_ms = 0;
    device->refresh_at_us = esp_timer_get_time() + REFRESH_INTERVAL_US;
    ESP_LOGI(TAG, "%s: connected", device->host);

    /* Everything at once: a receiver in standby answers these, which is how a
     * panel shows the right state before anybody presses anything. */
    send_command(device, "PWRQSTN");
    send_command(device, "MVLQSTN");
    send_command(device, "AMTQSTN");
    send_command(device, "SLIQSTN");
}

/* --- Commands ------------------------------------------------------------- */

static void run_command(const command_t *command)
{
    char host[HOST_MAX + 1];
    role_t role;
    char code[3];
    if (!parse_resource(command->resource, host, &role, code)) {
        slate_action_result(SLATE_ONKYO_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }
    device_t *device = NULL;
    size_t index = 0;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].host, host) == 0) {
            device = &s_devices[i];
            index = i;
            break;
        }
    }
    if (device == NULL) {
        slate_action_result(SLATE_ONKYO_PROVIDER_ID, command->action_id, false, "not_bound");
        return;
    }
    if (device->fd < 0) {
        slate_action_result(SLATE_ONKYO_PROVIDER_ID, command->action_id, false, "offline");
        return;
    }

    char message[16];
    bool sent = false;
    switch (role) {
    case ROLE_MAIN:
        if (command->action == SLATE_ACTION_TOGGLE) {
            sent = send_command(device, device->power ? "PWR00" : "PWR01");
        } else if (command->action == SLATE_ACTION_SET_POWER) {
            sent = send_command(device, command->boolean ? "PWR01" : "PWR00");
        } else if (command->action == SLATE_ACTION_SET_BRIGHTNESS) {
            int32_t level = command->number < 0 ? 0 : (command->number > 100 ? 100
                                                                            : command->number);
            /* Setting a volume on a receiver in standby is a request nobody
             * means: turn it on first, the way reaching for the knob does. */
            if (!device->power && level > 0 && !send_command(device, "PWR01")) {
                break;
            }
            snprintf(message, sizeof(message), "MVL%02X", (unsigned)level);
            sent = send_command(device, message);
        }
        break;
    case ROLE_INPUT_SELECT:
        if (command->action == SLATE_ACTION_ACTIVATE) {
            snprintf(message, sizeof(message), "SLI%s", code);
            sent = (device->power || send_command(device, "PWR01")) &&
                   send_command(device, message);
        }
        break;
    case ROLE_MUTE:
        if (command->action == SLATE_ACTION_ACTIVATE) {
            sent = send_command(device, "AMTTG");
        }
        break;
    case ROLE_INPUT:
        break; /* a reading, and §5.2 gives it no actions */
    }

    if (!sent) {
        slate_action_result(SLATE_ONKYO_PROVIDER_ID, command->action_id, false,
                            "unsupported_action");
        return;
    }
    /* §5.3: this acknowledges delivery. The receiver answers with its own
     * `PWR`/`MVL`/`SLI` frame within a few tens of milliseconds and that is
     * what confirms the tile — there is nothing to poll for. */
    slate_action_result(SLATE_ONKYO_PROVIDER_ID, command->action_id, true, NULL);
    (void)index;
}

/* --- The one task --------------------------------------------------------- */

static void update_status(void)
{
    slate_provider_status_t status;
    if (s_entry_count == 0) {
        status = SLATE_PROVIDER_UNCONFIGURED;
    } else if (!s_network_up) {
        status = SLATE_PROVIDER_OFFLINE;
    } else {
        size_t connected = 0;
        size_t answered = 0;
        for (size_t i = 0; i < s_device_count; i++) {
            connected += s_devices[i].fd >= 0 ? 1 : 0;
            answered += s_devices[i].known ? 1 : 0;
        }
        if (connected == s_device_count && answered == s_device_count) {
            status = SLATE_PROVIDER_ONLINE;
        } else if (connected > 0) {
            status = SLATE_PROVIDER_DEGRADED;
        } else if (answered > 0) {
            status = SLATE_PROVIDER_OFFLINE;
        } else {
            status = SLATE_PROVIDER_CONNECTING;
        }
    }
    slate_state_provider_set_status(SLATE_ONKYO_PROVIDER_ID, status);
}

static bool adopt_pending(void)
{
    xSemaphoreTake(s_bind_lock, portMAX_DELAY);
    bool pending = s_pending_valid;
    device_t *devices = s_pending_devices;
    entry_t *entries = s_pending_entries;
    size_t device_count = s_pending_device_count;
    size_t entry_count = s_pending_entry_count;
    s_pending_valid = false;
    s_pending_devices = NULL;
    s_pending_entries = NULL;
    s_pending_device_count = 0;
    s_pending_entry_count = 0;
    xSemaphoreGive(s_bind_lock);

    if (!pending) {
        return false;
    }

    /*
     * Move the live connection and everything learned over it to the host's
     * entry in the new table. Dropping them would reconnect a receiver whose
     * binding never changed, and — §5.9 learned this the expensive way — would
     * also clear `known`, so a receiver that then went away would never publish
     * `available: false` over the value `slate_state_bind()` carried across.
     */
    for (size_t d = 0; d < device_count; d++) {
        devices[d].fd = -1;
        for (size_t o = 0; o < s_device_count; o++) {
            if (strcmp(s_devices[o].host, devices[d].host) != 0) {
                continue;
            }
            devices[d] = s_devices[o]; /* the socket included */
            s_devices[o].fd = -1;      /* so the sweep below does not close it */
            break;
        }
    }
    for (size_t o = 0; o < s_device_count; o++) {
        if (s_devices[o].fd >= 0) {
            ESP_LOGI(TAG, "%s: no longer bound", s_devices[o].host);
            close(s_devices[o].fd);
        }
    }

    free(s_devices);
    free(s_entries);
    s_devices = devices;
    s_entries = entries;
    s_device_count = device_count;
    s_entry_count = entry_count;
    s_has_bindings = entry_count > 0;
    ESP_LOGI(TAG, "%u resources on %u receivers", (unsigned)s_entry_count,
             (unsigned)s_device_count);
    for (size_t i = 0; i < s_device_count; i++) {
        publish_device(i);
    }
    update_status();
    return true;
}

static void service_sockets(void)
{
    fd_set readable;
    FD_ZERO(&readable);
    int highest = -1;
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].fd >= 0) {
            FD_SET(s_devices[i].fd, &readable);
            if (s_devices[i].fd > highest) {
                highest = s_devices[i].fd;
            }
        }
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = SELECT_TIMEOUT_MS * 1000};
    if (highest < 0) {
        /* Nothing connected: the sleep is what keeps this loop from spinning
         * while every receiver is unplugged. */
        vTaskDelay(pdMS_TO_TICKS(SELECT_TIMEOUT_MS));
        return;
    }
    if (select(highest + 1, &readable, NULL, NULL, &timeout) <= 0) {
        return;
    }

    for (size_t i = 0; i < s_device_count; i++) {
        device_t *device = &s_devices[i];
        if (device->fd < 0 || !FD_ISSET(device->fd, &readable)) {
            continue;
        }
        ssize_t got = recv(device->fd, device->rx + device->rx_len, RX_MAX - device->rx_len, 0);
        if (got == 0) {
            disconnect(device, "closed the connection");
            publish_device(i);
            continue;
        }
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            disconnect(device, strerror(errno));
            publish_device(i);
            continue;
        }
        device->rx_len += (size_t)got;
        if (consume(device)) {
            publish_device(i);
        }
    }
}

static void connection_task(void *arg)
{
    (void)arg;
    for (;;) {
        adopt_pending();

        command_t command;
        if (xQueueReceive(s_commands, &command, 0) == pdTRUE) {
            if (command.kind == CMD_ACTION) {
                if (s_network_up) {
                    run_command(&command);
                } else {
                    slate_action_result(SLATE_ONKYO_PROVIDER_ID, command.action_id, false,
                                        "offline");
                }
            }
            continue; /* drain before spending time on sockets */
        }

        int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < s_device_count && s_network_up; i++) {
            device_t *device = &s_devices[i];
            if (device->fd < 0) {
                /* Only when nothing is waiting: connect() blocks, and a tap
                 * meant for a receiver that is up must not queue behind one
                 * that is not. */
                if (now >= device->retry_at_us && uxQueueMessagesWaiting(s_commands) == 0) {
                    connect_device(device);
                    if (device->fd >= 0) {
                        update_status();
                    }
                }
            } else if (now >= device->refresh_at_us) {
                device->refresh_at_us = now + REFRESH_INTERVAL_US;
                if (!send_command(device, "PWRQSTN") || !send_command(device, "MVLQSTN") ||
                    !send_command(device, "SLIQSTN")) {
                    disconnect(device, "stopped accepting commands");
                    publish_device(i);
                }
            }
        }

        service_sockets();
        update_status();
    }
}

/* --- Provider callbacks --------------------------------------------------- */

static esp_err_t subscribe(void *ctx, const char *const *resources, size_t count)
{
    (void)ctx;
    device_t *devices = NULL;
    entry_t *entries = NULL;
    size_t device_count = 0;
    size_t entry_count = 0;

    if (count > 0) {
        entries = heap_caps_calloc(count, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        devices = heap_caps_calloc(count, sizeof(device_t), MALLOC_CAP_SPIRAM);
        if (entries == NULL || devices == NULL) {
            free(entries);
            free(devices);
            return ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; i < count; i++) {
        char host[HOST_MAX + 1];
        role_t role;
        char code[3];
        if (!parse_resource(resources[i], host, &role, code)) {
            ESP_LOGW(TAG, "ignoring unparseable resource id %s", resources[i]);
            continue;
        }
        size_t device = SIZE_MAX;
        for (size_t d = 0; d < device_count; d++) {
            if (strcmp(devices[d].host, host) == 0) {
                device = d;
                break;
            }
        }
        if (device == SIZE_MAX) {
            device = device_count++;
            snprintf(devices[device].host, sizeof(devices[device].host), "%s", host);
            devices[device].fd = -1;
        }
        entry_t *entry = &entries[entry_count++];
        snprintf(entry->id, sizeof(entry->id), "%s", resources[i]);
        entry->device = (uint16_t)device;
        entry->role = role;
        memcpy(entry->code, code, sizeof(entry->code));
    }

    xSemaphoreTake(s_bind_lock, portMAX_DELAY);
    free(s_pending_devices);
    free(s_pending_entries);
    s_pending_devices = devices;
    s_pending_entries = entries;
    s_pending_device_count = device_count;
    s_pending_entry_count = entry_count;
    s_pending_valid = true;
    xSemaphoreGive(s_bind_lock);

    if (s_commands != NULL) {
        const command_t poke = {.kind = CMD_SWEEP};
        xQueueSend(s_commands, &poke, 0);
    }
    return ESP_OK;
}

static esp_err_t dispatch(void *ctx, uint32_t id, const slate_action_request_t *request)
{
    (void)ctx;
    if (s_commands == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (request->action) {
    case SLATE_ACTION_TOGGLE:
    case SLATE_ACTION_ACTIVATE:
        break;
    case SLATE_ACTION_SET_POWER:
        if (request->value_type != SLATE_ACTION_VALUE_BOOL) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SLATE_ACTION_SET_BRIGHTNESS:
        if (request->value_type != SLATE_ACTION_VALUE_NUMBER) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    command_t command = {
        .kind = CMD_ACTION,
        .action_id = id,
        .action = request->action,
        .number = request->value_type == SLATE_ACTION_VALUE_NUMBER ? request->value.number : 0,
        .boolean = request->value_type == SLATE_ACTION_VALUE_BOOL && request->value.boolean,
    };
    snprintf(command.resource, sizeof(command.resource), "%s", request->resource);

    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == SLATE_WIFI_EVENT_CONNECTED) {
        s_network_up = true;
    } else if (id == SLATE_WIFI_EVENT_DISCONNECTED) {
        s_network_up = false;
        slate_action_provider_unavailable(SLATE_ONKYO_PROVIDER_ID, "offline");
    } else {
        return;
    }
    if (!s_network_up && s_has_bindings) {
        slate_state_provider_set_status(SLATE_ONKYO_PROVIDER_ID, SLATE_PROVIDER_OFFLINE);
    }
}

/* --- Lifecycle ------------------------------------------------------------ */

esp_err_t slate_onkyo_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_bind_lock = xSemaphoreCreateMutexStatic(&s_bind_lock_storage);
    if (s_bind_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const slate_state_provider_t provider = {
        .id = SLATE_ONKYO_PROVIDER_ID,
        .subscribe = subscribe,
    };
    esp_err_t err = slate_state_provider_register(&provider);
    if (err != ESP_OK) {
        return err;
    }
    s_initialized = true;
    slate_state_provider_set_status(SLATE_ONKYO_PROVIDER_ID, SLATE_PROVIDER_UNCONFIGURED);

    const slate_action_provider_t action_provider = {
        .id = SLATE_ONKYO_PROVIDER_ID,
        .dispatch = dispatch,
    };
    esp_err_t action_err = slate_action_provider_register(&action_provider);
    if (action_err != ESP_OK) {
        ESP_LOGE(TAG, "semantic action dispatch unavailable: %s", esp_err_to_name(action_err));
    }
    return action_err;
}

esp_err_t slate_onkyo_start(void)
{
    if (!s_initialized || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    if (s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_event_handler_instance_register(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        return err;
    }

    slate_wifi_status_t wifi;
    slate_wifi_status(&wifi);
    s_network_up = wifi.connected;

    if (xTaskCreate(connection_task, "slate_onkyo", TASK_STACK, NULL, TASK_PRIORITY, &s_task) !=
        pdPASS) {
        esp_event_handler_unregister(SLATE_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        vQueueDelete(s_commands);
        s_commands = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#ifdef SLATE_ONKYO_SELFTEST

/*
 * The parser, the framing and the event mapping — everything between a byte off
 * a socket and §5.2's vocabulary. A receiver is not needed for any of it, and
 * the case that matters most cannot be produced by one on demand: a frame split
 * across two reads. TCP is a stream, the receiver sends small frames, and on a
 * quiet LAN they arrive whole every time — so the bug where a reader assumes
 * that would sit undisturbed until the day it did not.
 */
esp_err_t slate_onkyo_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name)                                                \
    do {                                                                      \
        bool passed_ = (condition);                                           \
        failures += !passed_;                                                 \
        ESP_LOGI(TAG, "selftest: %-52s %s", name, passed_ ? "PASS" : "FAIL"); \
    } while (0)

    char host[HOST_MAX + 1];
    role_t role;
    char code[3];

    CHECK(parse_resource("192.0.2.60/main", host, &role, code) &&
              strcmp(host, "192.0.2.60") == 0 && role == ROLE_MAIN && code[0] == '\0',
          "an address and the receiver itself");
    CHECK(parse_resource("192.0.2.60/input", host, &role, code) && role == ROLE_INPUT,
          "the selected input, as a reading");
    CHECK(parse_resource("192.0.2.60/mute", host, &role, code) && role == ROLE_MUTE,
          "mute");
    CHECK(parse_resource("192.0.2.60/input:2b", host, &role, code) &&
              role == ROLE_INPUT_SELECT && strcmp(code, "2b") == 0,
          "an input selector, by its eISCP code");
    CHECK(parse_resource("onkyo.local/input:2B", host, &role, code) &&
              strcmp(code, "2b") == 0 && strcmp(host, "onkyo.local") == 0,
          "an mDNS name, and a code normalised to lowercase");
    CHECK(!parse_resource("192.0.2.60/input:2", host, &role, code), "a one-digit code");
    CHECK(!parse_resource("192.0.2.60/input:2g", host, &role, code), "a non-hex code");
    CHECK(!parse_resource("192.0.2.60/volume", host, &role, code), "a role this adapter lacks");
    CHECK(!parse_resource("/main", host, &role, code), "an empty host");
    CHECK(!parse_resource("192.0.2.60", host, &role, code), "no role at all");

    CHECK(strcmp(input_name("2b"), "Net") == 0 && strcmp(input_name("2B"), "Net") == 0,
          "an input name, whichever case the receiver used");
    CHECK(input_name("7f") == NULL, "an input this table does not name");

    /* One frame, whole, exactly as the receiver sends it. */
    static const uint8_t POWER_ON[] = {'I',  'S',  'C', 'P', 0, 0, 0, 0x10, 0, 0, 0, 8,
                                       1,    0,    0,   0,   '!', '1', 'P', 'W', 'R', '0',
                                       '1',  0x1A};
    device_t device = {.fd = -1};
    memcpy(device.rx, POWER_ON, sizeof(POWER_ON));
    device.rx_len = sizeof(POWER_ON);
    CHECK(consume(&device) && device.known && device.power && device.rx_len == 0,
          "one whole frame turns the receiver on and is consumed");

    /* The same frame delivered in two reads, which is what a stream does. */
    memset(&device, 0, sizeof(device));
    device.fd = -1;
    memcpy(device.rx, POWER_ON, 13);
    device.rx_len = 13;
    CHECK(!consume(&device) && device.rx_len == 13,
          "half a frame changes nothing and is kept for the rest");
    memcpy(device.rx + device.rx_len, POWER_ON + 13, sizeof(POWER_ON) - 13);
    device.rx_len += sizeof(POWER_ON) - 13;
    CHECK(consume(&device) && device.power && device.rx_len == 0,
          "and the second read completes it");

    /* Rubbish ahead of a good frame must not desynchronise the connection. */
    memset(&device, 0, sizeof(device));
    device.fd = -1;
    device.rx[0] = 0xFF;
    device.rx[1] = 0x00;
    device.rx[2] = 'I';
    memcpy(device.rx + 3, POWER_ON, sizeof(POWER_ON));
    device.rx_len = 3 + sizeof(POWER_ON);
    CHECK(consume(&device) && device.power && device.rx_len == 0,
          "leading rubbish is skipped and the frame behind it still reads");

    /* An `NRI` answer is kilobytes of XML: too large to hold, and not a reason
     * to wait forever for the rest of it. */
    memset(&device, 0, sizeof(device));
    device.fd = -1;
    memcpy(device.rx, POWER_ON, 16);
    device.rx[8] = 0x00; device.rx[9] = 0x01; device.rx[10] = 0x00; device.rx[11] = 0x00;
    device.rx_len = 16;
    consume(&device);
    CHECK(device.rx_len < 16, "a frame too large to hold is skipped, not waited on");

    memset(&device, 0, sizeof(device));
    device.fd = -1;
    CHECK(apply_message(&device, "!1MVL56") && device.volume == 0x56,
          "MVL is hexadecimal: 56 is 86, not fifty-six");
    CHECK(!apply_message(&device, "!1MVL56"), "the same value again changes nothing");
    CHECK(apply_message(&device, "!1AMT01") && device.muted, "AMT01 mutes");
    CHECK(apply_message(&device, "!1SLI2B") && strcmp(device.input, "2b") == 0,
          "SLI carries the selector, normalised");
    CHECK(!apply_message(&device, "!1NLSC-P"), "a frame with no tile behind it is ignored");
    CHECK(!apply_message(&device, "!1"), "a truncated message is not a message");

#undef CHECK
    ESP_LOGI(TAG, "selftest: %d failure(s)", failures);
    return failures == 0 ? ESP_OK : ESP_FAIL;
}

#endif /* SLATE_ONKYO_SELFTEST */
