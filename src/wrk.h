#ifndef WRK_H
#define WRK_H

#include "config.h"
#include "glue.h"
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <math.h>

#if HAVE_QUIC
#include <picoquic.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <lua.h>
#include <sys/time.h>

#include "stats.h"
#include "ae.h"
#include "http_parser.h"
#include "hdr_histogram.h"

#define VERSION  "4.0.0"
#define RECVBUF  8192
#define SAMPLES  100000000

#define SOCKET_TIMEOUT_MS   2000
#define CALIBRATE_DELAY_MS  10000
#define TIMEOUT_INTERVAL_MS 2000

typedef struct {
    pthread_t thread;
    aeEventLoop *loop;
    struct addrinfo *addr;
    uint64_t connections;
    int interval;
    uint64_t stop_at;
    uint64_t complete;
    uint64_t requests;
    uint64_t bytes;
    uint64_t start;
    double throughput;
    uint64_t mean;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *u_latency_histogram;
    tinymt64_t rand;
    lua_State *L;
    errors errors;
    struct connection *cs;
#if HAVE_QUIC
    picoquic_quic_t *quic;
#endif
    char* alpn;
} thread;

typedef struct {
    char  *buffer;
    size_t length;
    char  *cursor;
} buffer;

typedef struct connection {
    thread *thread;
    http_parser parser;
    enum {
        FIELD, VALUE
    } state;
    int fd;
    struct in_addr bind;
    union {
            SSL *ssl;
#if HAVE_QUIC
            picoquic_cnx_t* cnx;
#endif
    };
    double throughput;
    double catch_up_throughput;
    uint64_t complete;
    uint64_t complete_at_last_batch_start;
    uint64_t catch_up_start_time;
    uint64_t complete_at_catch_up_start;
    uint64_t thread_start;
    uint64_t start;
    char *request;
    size_t length;
    char *user;
    size_t user_len;
    size_t written;
    uint64_t pending;
    buffer headers;
    buffer body;
    char buf[RECVBUF];
    uint64_t actual_latency_start;
    bool has_pending;
    bool caught_up;
    // Internal tracking numbers (used purely for debugging):
    uint64_t latest_should_send_time;
    uint64_t latest_expected_start;
    uint64_t latest_connect;
    uint64_t latest_write;

} connection;


typedef int (*connect_t)(thread *thread, connection *c);
typedef int (*reconnect_t)(thread *, connection *);

typedef struct proto {
   connect_t connect;
   reconnect_t reconnect;
} proto;

struct config {
    uint64_t threads;
    uint64_t connections;
    uint64_t duration;
    uint64_t timeout;
    uint64_t pipeline;
    uint64_t rate;
    uint64_t delay_ms;
    in_addr_t bind;
    int      affinity;
    bool     latency;
    bool     u_latency;
    bool     dynamic;
    bool     record_all_responses;
    bool     raw;
    bool    verbose;
    char    *host;
    char    *script;
    int     http_version;
    proto   *proto;
    SSL_CTX *ctx;
};

extern struct config cfg;


extern struct http_parser_settings parser_settings;

static uint64_t time_us() {
    struct timespec t;
    clock_gettime( CLOCK_MONOTONIC, &t) ;
    return (t.tv_sec * 1000000) + (t.tv_nsec / 1000);
}

int delay_request(aeEventLoop *loop, long long id, void *data);
void stats_request_completed(connection* c);

static uint64_t usec_to_next_send(connection *c) {
    uint64_t now = time_us();

    uint64_t next_start_time = c->thread_start + (c->complete / c->throughput);

    bool send_now = true;

    if (next_start_time > now) {
        // We are on pace. Indicate caught_up and don't send now.
        c->caught_up = true;
        send_now = false;
    } else {
        // We are behind
        if (c->caught_up) {
            // This is the first fall-behind since we were last caught up
            c->caught_up = false;
            c->catch_up_start_time = now;
            c->complete_at_catch_up_start = c->complete;
        }

        // Figure out if it's time to send, per catch up throughput:
        uint64_t complete_since_catch_up_start =
                c->complete - c->complete_at_catch_up_start;

        next_start_time = c->catch_up_start_time +
                (complete_since_catch_up_start / c->catch_up_throughput);

        if (next_start_time > now) {
            // Not yet time to send, even at catch-up throughout:
            send_now = false;
        }
    }

    if (send_now) {
        c->latest_should_send_time = now;
        c->latest_expected_start = next_start_time;
    }

    return send_now ? 0 : (next_start_time - now);
}

#endif /* WRK_H */
