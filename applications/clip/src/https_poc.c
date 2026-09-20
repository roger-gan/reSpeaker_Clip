/* Single-job HTTPS experiment. No local deletion or persistent credentials. */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_time.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include "https_poc.h"
#include "at_server.h"
#include "transport.h"
#include "storage.h"
#include "wifi.h"
#include "https_ca.h"

#define CA_TAG 71
#define MAX_FILE (8U * 1024U * 1024U)
static atomic_t active, cancelled, stage, last_error, bytes_sent;
static atomic_t stalled;
static K_MUTEX_DEFINE(socket_lock);
static int running_socket = -1;
static K_SEM_DEFINE(start_job, 0, 1);
static char ssid[33], password[64], host[96], port[6], device_id[33], token[97];
static char session[32];
static uint32_t segment;
static bool time_ready;
static int64_t deadline;
static struct fs_file_t file;
/* Allocate only during a job, from the same heap as TLS. This saves static
 * reservation, not peak upload memory; allocation failure is a normal error. */
static struct upload_buffers {
    uint8_t file_io[2048], receive[2048];
    char body[1025];
} *buffers;
#define io (buffers->file_io)
#define recv_buffer (buffers->receive)
#define response_body (buffers->body)
static char expected_hash[65];
static size_t response_length, file_length, header_bytes;
static bool response_final;
static int response_status;

/* Wall time uses libc time()/CLOCK_REALTIME; TLS timers use monotonic uptime. */
mbedtls_ms_time_t mbedtls_ms_time(void) { return k_uptime_get(); }

static char *split_field(char **cursor)
{
    char *value = *cursor;
    if (!value) { return NULL; }
    char *end = strchr(value, ',');
    if (end) { *end = 0; *cursor = end + 1; }
    else { *cursor = NULL; }
    return value;
}

static void interrupt_socket(void)
{
    k_mutex_lock(&socket_lock, K_FOREVER);
    if (running_socket >= 0) { zsock_shutdown(running_socket, ZSOCK_SHUT_RDWR); }
    k_mutex_unlock(&socket_lock);
}

static void stall_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_set(&stalled, 1);
    interrupt_socket();
}
static K_WORK_DELAYABLE_DEFINE(stall_work, stall_handler);

static void progress(void) { k_work_reschedule(&stall_work, K_SECONDS(15)); }

static void close_socket(int sock)
{
    k_mutex_lock(&socket_lock, K_FOREVER);
    running_socket = -1;
    zsock_close(sock);
    k_mutex_unlock(&socket_lock);
}

enum { IDLE, PREPARING, CONNECTING, TLS, SENDING, VERIFYING, SUCCEEDED, FAILED, CANCELLED };
static const char *const names[] = {"idle", "preparing", "connecting", "tls", "sending",
    "verifying", "succeeded", "failed", "cancelled"};

bool https_poc_active(void) { return atomic_get(&active); }

bool https_poc_command_allowed(const char *name)
{
    /* A deliberately small control surface also excludes deferred erase/reboot
     * commands which could outlive the AT handler and race a subsequent job. */
    if (https_poc_active()) {
        return !strcmp(name, "UPLOAD") || !strcmp(name, "UPLOADCANCEL") ||
               !strcmp(name, "GSTAT") || !strcmp(name, "BATT") || !strcmp(name, "VERSION");
    }
    return !strcmp(name, "NETCFG") || !strcmp(name, "UPLOADCFG") ||
           !strcmp(name, "UPTIME") || !strcmp(name, "UPLOAD") ||
           !strcmp(name, "UPLOADCANCEL") || !strcmp(name, "USB") ||
           !strcmp(name, "GSTAT") || !strcmp(name, "BATT") ||
           !strcmp(name, "VERSION") || !strcmp(name, "LIST") ||
           !strcmp(name, "START") || !strcmp(name, "STOP") ||
           !strcmp(name, "TIME") || !strcmp(name, "STORAGE");
}

bool https_poc_event(enum clip_event event, struct clip_event_result_info *result,
                     bool device_idle)
{
    enum clip_event_result ret = CLIP_EVENT_OK;
    if (event == CLIP_EVENT_UPLOAD_BEGIN) {
        if (!device_idle || !atomic_cas(&active, 0, 1)) { ret = CLIP_EVENT_BUSY; }
    } else if (event == CLIP_EVENT_UPLOAD_END) {
        atomic_clear(&active);
    } else if (https_poc_active() && event != CLIP_EVENT_STATUS_SHOW &&
               event != CLIP_EVENT_USB_CONNECTED) {
        if (event == CLIP_EVENT_POWER_OFF_EXEC) {
            atomic_set(&cancelled, 1); interrupt_socket();
        }
        ret = CLIP_EVENT_BUSY;
    } else { return false; }
    if (result) { result->result = ret; }
    return true;
}

static int check_cancel(void)
{
    if (atomic_get(&cancelled)) { return -ECANCELED; }
    return k_uptime_get() >= deadline ? -ETIMEDOUT : 0;
}

static bool valid_id(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (!n || n > max) { return false; }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') { return false; }
    }
    return true;
}

static int hash_file(void)
{
    mbedtls_sha256_context hash;
    uint8_t digest[32];
    mbedtls_sha256_init(&hash);
    int ret = mbedtls_sha256_starts(&hash, 0);
    size_t total = 0;
    while (!ret && total < file_length) {
        ret = check_cancel();
        if (ret) { break; }
        ssize_t n = fs_read(&file, io, MIN(sizeof(io), file_length - total));
        if (n <= 0) { ret = n < 0 ? (int)n : -EIO; break; }
        total += n;
        ret = mbedtls_sha256_update(&hash, io, n);
    }
    if (!ret) { ret = mbedtls_sha256_finish(&hash, digest); }
    mbedtls_sha256_free(&hash);
    if (ret) { return ret; }
    for (int i = 0; i < 32; i++) { snprintf(expected_hash + i * 2, 3, "%02x", digest[i]); }
    return fs_seek(&file, 0, FS_SEEK_SET);
}

static int send_payload(int sock, struct http_request *req, void *user)
{
    ARG_UNUSED(req); ARG_UNUSED(user);
    size_t total = 0;
    atomic_set(&stage, SENDING);
    while (total < file_length) {
        int ret = check_cancel();
        if (ret) { return ret; }
        ssize_t n = fs_read(&file, io, MIN(sizeof(io), file_length - total));
        if (n <= 0) { return n < 0 ? (int)n : -EIO; }
        size_t sent = 0;
        while (sent < n) {
            ret = check_cancel();
            if (ret) { return ret; }
            ssize_t count = zsock_send(sock, io + sent, n - sent, 0);
            if (count <= 0) { return count < 0 ? -errno : -EIO; }
            sent += count;
            progress();
            atomic_set(&bytes_sent, total + sent);
        }
        total += n;
    }
    return (int)total;
}

static int header_fragment(struct http_parser *parser, const char *at, size_t length)
{
    ARG_UNUSED(parser); ARG_UNUSED(at);
    progress();
    header_bytes += length;
    return header_bytes > 4096 ? -EMSGSIZE : 0;
}

static int receive_response(struct http_response *rsp, enum http_final_call final, void *user)
{
    ARG_UNUSED(user);
    progress();
    if (check_cancel()) { return check_cancel(); }
    if (response_length + rsp->body_frag_len > 1024) { return -EMSGSIZE; }
    if (rsp->body_frag_len) {
        memcpy(response_body + response_length, rsp->body_frag_start, rsp->body_frag_len);
        response_length += rsp->body_frag_len;
    }
    response_status = rsp->http_status_code;
    response_final = final == HTTP_DATA_FINAL;
    return 0;
}

static int upload(int sock)
{
    char url[180], auth[128], hash_header[90];
    snprintf(url, sizeof(url), "/v1/devices/%s/recordings/%s/%u", device_id, session, segment);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);
    snprintf(hash_header, sizeof(hash_header), "X-Content-SHA256: %s\r\n", expected_hash);
    const char *headers[] = {auth, hash_header, "Connection: close\r\n", NULL};
    struct http_parser_settings callbacks = {
        .on_header_field = header_fragment, .on_header_value = header_fragment,
    };
    struct http_request req = {
        .method = HTTP_PUT, .url = url, .protocol = "HTTP/1.1", .host = host, .port = port,
        .header_fields = headers, .content_type_value = "application/octet-stream",
        .payload_cb = send_payload, .payload_len = file_length,
        .response = receive_response, .recv_buf = recv_buffer, .recv_buf_len = sizeof(recv_buffer),
        .http_cb = &callbacks,
    };
    response_length = header_bytes = 0;
    response_final = false;
    response_status = 0;
    memset(response_body, 0, sizeof(response_body));
    int remaining = (int)(deadline - k_uptime_get());
    if (remaining <= 0) { return -ETIMEDOUT; }
    progress();
    int ret = http_client_req(sock, &req, remaining, NULL);
    memset(auth, 0, sizeof(auth));
    if (ret < 0) { return ret; }
    atomic_set(&stage, VERIFYING);
    if (!response_final || (response_status != 200 && response_status != 201)) { return -EBADMSG; }
    struct receipt { char *status; int size; char *sha256; } receipt;
    static const struct json_obj_descr fields[] = {
        JSON_OBJ_DESCR_PRIM(struct receipt, status, JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM(struct receipt, size, JSON_TOK_NUMBER),
        JSON_OBJ_DESCR_PRIM(struct receipt, sha256, JSON_TOK_STRING),
    };
    ret = json_obj_parse(response_body, response_length, fields, ARRAY_SIZE(fields), &receipt);
    if (ret != 7 || strcmp(receipt.status, "stored") || receipt.size != file_length ||
        strcmp(receipt.sha256, expected_hash)) { return -EBADMSG; }
    return 0;
}

static int tls_connect(void)
{
    struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct zsock_addrinfo *address = NULL;
    int ret = zsock_getaddrinfo(host, port, &hints, &address);
    if (ret) { return -EHOSTUNREACH; }
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) { zsock_freeaddrinfo(address); return -errno; }
    k_mutex_lock(&socket_lock, K_FOREVER);
    running_socket = sock;
    k_mutex_unlock(&socket_lock);
    if ((ret = check_cancel()) != 0) { goto out; }
    sec_tag_t tags[] = {CA_TAG};
    int verify = TLS_PEER_VERIFY_REQUIRED;
    struct timeval timeout = {.tv_sec = 15};
#define OPTION(level, name, value, size) do { \
    if (zsock_setsockopt(sock, level, name, value, size) < 0) { ret = -errno; goto out; } \
} while (0)
    OPTION(SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags));
    OPTION(SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
    OPTION(SOL_TLS, TLS_HOSTNAME, host, strlen(host) + 1);
    OPTION(SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    OPTION(SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#undef OPTION
    /* Nonblocking connect bounds the TLS handshake and supports cancellation. */
    if (zsock_fcntl(sock, F_SETFL, O_NONBLOCK) < 0) { ret = -errno; goto out; }
    atomic_set(&stage, TLS);
    ret = zsock_connect(sock, address->ai_addr, address->ai_addrlen);
    if (ret < 0 && errno != EINPROGRESS && errno != EAGAIN) { ret = -errno; goto out; }
    int64_t end = k_uptime_get() + 30000;
    while (ret < 0) {
        if ((ret = check_cancel()) != 0) { goto out; }
        if (k_uptime_get() >= end) { ret = -ETIMEDOUT; goto out; }
        struct zsock_pollfd fd = {.fd = sock, .events = ZSOCK_POLLOUT};
        int count = zsock_poll(&fd, 1, 100);
        if (count < 0) { ret = -errno; goto out; }
        if (!count) { ret = -EINPROGRESS; continue; }
        int error = 0; socklen_t len = sizeof(error);
        if (zsock_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0) { ret = -errno; goto out; }
        if (error || (fd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
            ret = error ? -error : -ECONNRESET; goto out;
        }
        ret = 0;
    }
    if (zsock_fcntl(sock, F_SETFL, 0) < 0) { ret = -errno; goto out; }
out:
    zsock_freeaddrinfo(address);
    if (ret < 0) { close_socket(sock); return ret; }
    return sock;
}

static void worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        k_sem_take(&start_job, K_FOREVER);
        deadline = k_uptime_get() + 300000;
        atomic_set(&stage, PREPARING);
        char path[STORAGE_PATH_MAX];
        buffers = k_malloc(sizeof(*buffers));
        int ret = buffers ? storage_ensure_mounted() : -ENOMEM;
        bool opened = false;
        int sock = -1;
        struct fs_dirent entry;
        if (!ret) { ret = storage_build_chunk_path(session, segment, path, sizeof(path)); }
        if (!ret) { ret = fs_stat(path, &entry); }
        if (!ret && (entry.type != FS_DIR_ENTRY_FILE || !entry.size || entry.size > MAX_FILE)) { ret = -EINVAL; }
        fs_file_t_init(&file);
        if (!ret) { ret = fs_open(&file, path, FS_O_READ); opened = ret == 0; }
        if (!ret) { file_length = entry.size; ret = hash_file(); }
        if (!ret) {
            atomic_set(&stage, CONNECTING);
            ret = wifi_station_connect(ssid, password, &cancelled);
        }
        if (!ret) { sock = tls_connect(); if (sock < 0) { ret = sock; } }
        if (!ret) { ret = upload(sock); }
        struct k_work_sync sync;
        k_work_cancel_delayable_sync(&stall_work, &sync);
        if (sock >= 0) { close_socket(sock); }
        wifi_off();
        if (opened) { fs_close(&file); }
        k_free(buffers);
        buffers = NULL;
        if (atomic_get(&cancelled)) { ret = -ECANCELED; }
        else if (atomic_get(&stalled)) { ret = -ETIMEDOUT; }
        atomic_set(&last_error, ret);
        atomic_set(&stage, ret == -ECANCELED ? CANCELLED : ret ? FAILED : SUCCEEDED);
        struct clip_event_result_info result;
        /* A temporarily full event queue must not strand the storage claim. */
        while (clip_post_event_sync(CLIP_EVENT_UPLOAD_END, &result)) {
            k_sleep(K_MSEC(10));
        }
    }
}
K_THREAD_DEFINE(https_worker, 6144, worker, NULL, NULL, NULL, 7, 0, 0);

static int command(struct at_cmd_ctx *ctx, char *response, size_t length)
{
    int ret = 0;
    char args[256] = {0};
    if (ctx->transport_type != TRANSPORT_TYPE_USB) { ret = -EACCES; goto reply; }
    if (!strcmp(ctx->name, "UPLOAD") && ctx->type != AT_CMD_TYPE_SET) { goto reply; }
    if (!strcmp(ctx->name, "UPLOADCANCEL")) {
        atomic_set(&cancelled, 1); interrupt_socket(); goto reply;
    }
    if (https_poc_active()) { ret = -EBUSY; goto reply; }
    if (!ctx->args) { ret = -EINVAL; goto reply; }
    if (strlen(ctx->args) >= sizeof(args)) { ret = -EINVAL; goto reply; }
    strcpy(args, ctx->args);
    if (!strcmp(ctx->name, "NETCFG")) {
        char *split = strchr(args, ',');
        if (!split) { ret = -EINVAL; goto reply; }
        *split++ = 0;
        if (!strlen(args) || strlen(args) > 32 || strlen(split) < 8 || strlen(split) > 63) {
            ret = -EINVAL; goto reply;
        }
        strcpy(ssid, args); strcpy(password, split);
    } else if (!strcmp(ctx->name, "UPLOADCFG")) {
        char *fields[4], *cursor = args;
        for (int i = 0; i < 4; i++) { fields[i] = split_field(&cursor); }
        if (cursor || !fields[3] || !valid_id(fields[2], 32) ||
            !valid_id(fields[3], 96) || strlen(fields[3]) < 32 ||
            !fields[0][0] || strlen(fields[0]) >= sizeof(host)) { ret = -EINVAL; goto reply; }
        for (char *p = fields[0]; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') { ret = -EINVAL; goto reply; }
        }
        char *end; unsigned long n = strtoul(fields[1], &end, 10);
        if (!fields[1][0] || *end || n < 1 || n > 65535 || strlen(fields[1]) > 5) { ret = -EINVAL; goto reply; }
        strcpy(host, fields[0]); strcpy(port, fields[1]); strcpy(device_id, fields[2]); strcpy(token, fields[3]);
    } else if (!strcmp(ctx->name, "UPTIME")) {
        char *end; long long timestamp = strtoll(args, &end, 10);
        if (*end || timestamp < 1700000000LL || timestamp > 4102444800LL) { ret = -EINVAL; goto reply; }
        struct timespec ts = {.tv_sec = timestamp, .tv_nsec = 0};
        ret = clock_settime(CLOCK_REALTIME, &ts) ? -errno : 0;
        time_ready = ret == 0;
    } else if (!strcmp(ctx->name, "UPLOAD")) {
        char *split = strchr(args, ':');
        if (!split) { ret = -EINVAL; goto reply; }
        *split++ = 0;
        char *end; unsigned long n = strtoul(split, &end, 10);
        if (!valid_id(args, 31) || !*split || *end || !n || n > 999999999 ||
            !ssid[0] || !host[0] || !time_ready) { ret = -EINVAL; goto reply; }
        struct clip_event_result_info result;
        ret = clip_post_event_sync(CLIP_EVENT_UPLOAD_BEGIN, &result);
        if (ret || result.result != CLIP_EVENT_OK) { ret = -EBUSY; goto reply; }
        strcpy(session, args); segment = n;
        atomic_clear(&cancelled); atomic_clear(&stalled);
        atomic_clear(&last_error); atomic_clear(&bytes_sent);
        atomic_set(&stage, PREPARING);
        k_sem_give(&start_job);
    } else { ret = -ENOTSUP; }
    memset(args, 0, sizeof(args));
reply:
    memset(args, 0, sizeof(args));
    return snprintf(response, length,
        "{\"ok\":%s,\"error\":%d,\"stage\":\"%s\",\"active\":%s,\"last_error\":%d,\"sent\":%d}\n",
        ret ? "false" : "true", ret, names[atomic_get(&stage)],
        https_poc_active() ? "true" : "false", (int)atomic_get(&last_error), (int)atomic_get(&bytes_sent));
}

int https_poc_register(void)
{
    int ret = tls_credential_add(CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, https_ca, sizeof(https_ca));
    if (ret) { return ret; }
    static const struct at_command commands[] = {
        {"NETCFG", AT_CMD_SET, command}, {"UPLOADCFG", AT_CMD_SET, command},
        {"UPTIME", AT_CMD_SET, command}, {"UPLOAD", AT_CMD_SET | AT_CMD_QUERY, command},
        {"UPLOADCANCEL", AT_CMD_EXEC, command},
    };
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        ret = at_server_register_cmd(&commands[i]);
        if (ret) { return ret; }
    }
    return 0;
}
