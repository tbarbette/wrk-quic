// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#define _GNU_SOURCE
#include "wrk.h"
#include "script.h"
#include "main.h"
#include "hdr_histogram.h"
#include "stats.h"
#include <arpa/inet.h>
#include "assert.h"

// Max recordable latency of 1 day
#define MAX_LATENCY 24L * 60 * 60 * 1000000

struct http_parser_settings parser_settings;

#include "tcp.h"
#ifdef HAVE_QUIC
#include "quic.h"
#endif


struct config cfg;

static struct {
    stats *requests;
    pthread_mutex_t mutex;
} statistics;

struct http_parser_settings parser_settings = {
    .on_message_complete = response_complete
};


void stats_request_completed();

static volatile sig_atomic_t stop = 0;

static void handler(int sig) {
    stop = 1;
};

int delay_request(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    uint64_t time_usec_to_wait = usec_to_next_send(c);
    if (time_usec_to_wait) {
        return round((time_usec_to_wait / 1000.0L) + 0.5); /* don't send, wait */
    }
    aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static void sock_writeable(connection* c, aeEventLoop *loop) {
    thread *thread = c->thread;

    //If nothing is written in the connection, we'll wait
    if (!c->written) {
        uint64_t time_usec_to_wait = usec_to_next_send(c);
        if (time_usec_to_wait) {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            // Not yet time to send. Delay:
            aeDeleteFileEvent(loop, c->fd, AE_WRITABLE);
            aeCreateTimeEvent(
                    thread->loop, msec_to_wait, delay_request, c, NULL);
            return;
        }
        c->latest_write = time_us();
    }

    if (!c->written && cfg.dynamic) {
        script_request(thread->L, &c->request, &c->length, &c->user, &c->user_len);
    }

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    if (!c->written) {
        c->start = time_us();
        if (!c->has_pending) {
            c->actual_latency_start = c->start;
            c->complete_at_last_batch_start = c->complete;
            c->has_pending = true;
        }
        c->pending = cfg.pipeline;
    }

eagain:
    switch (sock.write(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
        case AGAIN: goto eagain;
    }

    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, c->fd, AE_WRITABLE);
    }

    return;

  error:
    thread->errors.write++;
    cfg.proto->reconnect(thread, c);
}

static void usage() {
    printf("Usage: wrk <options> <url>                            \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "    -a, --affinity         Affinitize threads to CPUs \n"
           "                                                      \n"
#if HAVE_QUIC
           "    -q, --quic             Use QUIC transport instead of TCP\n"
#endif
           "    -p, --http             HTTP Version (09,10 or 11) \n"
#if HAVE_QUIC
           "                           -1 use the sample server   \n"
           "                           picoquic mechanism.        \n"
#endif
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "    -L  --latency          Print latency statistics   \n"
           "    -U  --u_latency        Print uncorrected latency statistics\n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -B, --batch_latency    Measure latency of whole   \n"
           "                           batches of pipelined ops   \n"
           "                           (as opposed to each op)    \n"
           "    -v, --version          Print version details      \n"
           "        --raw              No human-readable unit     \n"
           "    -V, --verbose          Be verbose about requests  \n"
           "    -b, --bind       <IP>  Establish connection from a\n"
           "                             given address            \n"
           "    -R, --rate        <T>  work rate (throughput)     \n"
           "                           in requests/sec (total)    \n"
           "                           [Required Parameter]       \n"
           "                                                      \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};
    cpu_set_t cpuset;

    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    sock.writeable = sock_writeable;
#if HAVE_QUIC
    if (cfg.proto == &quic_proto) {
        //Quic handles everything on its own
        sock.connect  = 0;
        sock.close    = 0;
        sock.read     = 0;
        sock.write    = 0;
        sock.readable = 0;
        sock.writeable = 0;
    } else
#endif
    if (!strncmp("https", schema, 5)) {
        if ((cfg.ctx = ssl_init()) == NULL) {
            fprintf(stderr, "unable to initialize SSL\n");
            ERR_print_errors_fp(stderr);
            exit(1);
        }
        sock.connect  = ssl_connect;
        sock.close    = ssl_close;
        sock.read     = ssl_read;
        sock.write    = ssl_write;
        sock.readable = ssl_readable;
    }
	
    cfg.host = host;
	
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  SIG_IGN);

    pthread_mutex_init(&statistics.mutex, NULL);
    statistics.requests = stats_alloc(10);
    thread *threads = zcalloc(cfg.threads * sizeof(thread));

    hdr_init(1, MAX_LATENCY, 3, &(statistics.requests->histogram));

    bool is_tcp = cfg.proto == &tcp_proto;

    lua_State *L = script_create(cfg.script, url, headers, is_tcp, cfg.http_version);

    if (!script_resolve(L, host, service)) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
        //exit(1);
    }

    uint64_t connections = cfg.connections / cfg.threads;
    double throughput    = (double)cfg.rate / cfg.threads;
    uint64_t stop_at     = time_us() + (cfg.duration * 1000000);

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        t->loop        = aeCreateEventLoop(10 + cfg.connections * 3);
        t->connections = connections;
        t->throughput = throughput;
        t->stop_at     = stop_at;

        t->L = script_create(cfg.script, url, headers, cfg.proto == &tcp_proto, cfg.http_version);
        script_init(L, t, argc - optind, &argv[optind]);

        if (i == 0) {
            if (cfg.http_version >= 0)
                cfg.pipeline = script_verify_request(t->L);
            else
                cfg.pipeline = 1;
            cfg.dynamic = !script_is_static(t->L);
            if (script_want_response(t->L)) {
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;
            }
        }

        if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }

        if (cfg.affinity) {
            CPU_ZERO(&cpuset);
            CPU_SET(i, &cpuset);
            pthread_setaffinity_np(t, sizeof(cpuset), &cpuset);
        }
    }

    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    char *time = format_time_s(cfg.duration, cfg.raw);
    printf("Running %s test @ %s\n", time, url);
    printf("  %"PRIu64" threads and %"PRIu64" connections\n",
            cfg.threads, cfg.connections);

    uint64_t start    = time_us();
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };

    struct hdr_histogram* latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
    struct hdr_histogram* u_latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &u_latency_histogram);

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        pthread_join(t->thread, NULL);
    }

    uint64_t runtime_us = time_us() - start;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        complete += t->complete;
        bytes    += t->bytes;

        errors.connect += t->errors.connect;
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;

        hdr_add(latency_histogram, t->latency_histogram);
        hdr_add(u_latency_histogram, t->u_latency_histogram);
    }

    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    stats *latency_stats = stats_alloc(10);
    if ((int64_t)cfg.rate <= 0) {
        printf("Rate is disabled, latency is not corrected");
        latency_stats->min = hdr_min(u_latency_histogram);
        latency_stats->max = hdr_max(u_latency_histogram);
        latency_stats->histogram = u_latency_histogram;

    } else {
        latency_stats->min = hdr_min(latency_histogram);
        latency_stats->max = hdr_max(latency_histogram);
        latency_stats->histogram = latency_histogram;
    }

    print_stats_header(cfg.raw);
    print_stats("Latency", latency_stats, format_time_us);
    print_stats("Req/Sec", statistics.requests, format_metric);

    if (cfg.latency) {
        print_hdr_latency(latency_histogram,
                "Recorded Latency", cfg.raw);
        printf("----------------------------------------------------------\n");
    }

    if (cfg.u_latency) {
        printf("\n");
        print_hdr_latency(u_latency_histogram,
                "Uncorrected Latency (measured without taking delayed starts into account)", cfg.raw);
        printf("----------------------------------------------------------\n");
    }

    char *runtime_msg = format_time_us(runtime_us, cfg.raw);

    printf("  %"PRIu64" requests in %s, %sB read\n",
            complete, runtime_msg, format_binary(bytes, cfg.raw));
    if (errors.connect || errors.read || errors.write || errors.timeout) {
        printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
               errors.connect, errors.read, errors.write, errors.timeout);
    }

    if (errors.status) {
        printf("  Non-2xx or 3xx responses: %d\n", errors.status);
    }

    if (cfg.raw) {
        printf("Requests/sec: %Lf\n", req_per_s);
        printf("Transfer/sec: %sB\n", format_binary(bytes_per_s,cfg.raw));
    } else {
        printf("Requests/sec: %9.2Lf\n", req_per_s);
        printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s,cfg.raw));
    }

    if (script_has_done(L)) {
        script_summary(L, runtime_us, complete, bytes);
        script_errors(L, &errors);
        script_done(L, latency_stats, statistics.requests);
    }

    return 0;
}

void *thread_main(void *arg) {
    thread *thread = arg;
    aeEventLoop *loop = thread->loop;

    thread->cs = zcalloc(thread->connections * sizeof(connection));
    tinymt64_init(&thread->rand, time_us());
    hdr_init(1, MAX_LATENCY, 3, &thread->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &thread->u_latency_histogram);

    char *request = NULL;
    char *user = NULL;
    size_t length,user_len = 0;

    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length, &user, &user_len);
    }

    double throughput = (thread->throughput / 1000000.0) / thread->connections;
#if HAVE_QUIC
    if (cfg.proto == &quic_proto) {
        if (cfg.http_version < 0)
            thread->alpn = ALPN_SAMPLE_SERVER;
        else if (cfg.http_version == 30)
            thread->alpn = ALPN_H3;
        else
            thread->alpn = ALPN_HQ;

        thread->quic       =  quic_init();
        if (!thread->quic) {
            printf("Could not initialize QUIC!\n");
            exit(1);
        }
    }
#endif
    connection *c = thread->cs;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread     = thread;
        c->bind.s_addr    = cfg.bind;
        c->ssl        = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request    = request;
        c->length     = length;
        c->throughput = throughput;
        c->catch_up_throughput = throughput * 2;
        c->complete   = 0;
        c->caught_up  = true;
        // Stagger connects 5 msec apart within thread:
        aeCreateTimeEvent(loop, i * 5, delayed_initial_connect, c, NULL);
    }

    uint64_t calibrate_delay = CALIBRATE_DELAY_MS + (thread->connections * 5);
    uint64_t timeout_delay = TIMEOUT_INTERVAL_MS + (thread->connections * 5);

    aeCreateTimeEvent(loop, calibrate_delay, calibrate, thread, NULL);
    aeCreateTimeEvent(loop, timeout_delay, check_timeouts, thread, NULL);

    thread->start = time_us();
    aeMain(loop);

    aeDeleteEventLoop(loop);
    zfree(thread->cs);

    return NULL;
}

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    c->thread_start = time_us();
    cfg.proto->connect(c->thread, c);
    return AE_NOMORE;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    long double mean = hdr_mean(thread->latency_histogram);
    long double latency = hdr_value_at_percentile(
            thread->latency_histogram, 90.0) / 1000.0L;
    long double interval = MAX(latency * 2, 10);

    if (mean == 0) return CALIBRATE_DELAY_MS;

    thread->mean     = (uint64_t) mean;
    hdr_reset(thread->latency_histogram);
    hdr_reset(thread->u_latency_histogram);

    thread->start    = time_us();
    thread->interval = interval;
    thread->requests = 0;

    printf("  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
            (thread->mean)/1000.0,
            thread->interval);

    aeCreateTimeEvent(loop, thread->interval, sample_rate, thread, NULL);

    return AE_NOMORE;
}

static int check_timeouts(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    connection *c  = thread->cs;
    uint64_t now   = time_us();

    uint64_t maxAge = now - (cfg.timeout * 1000);

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        if (maxAge > c->start) {
            thread->errors.timeout++;
        }
    }

    if (stop || now >= thread->stop_at) {
        aeStop(loop);
    }

    return TIMEOUT_INTERVAL_MS;
}

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
    uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;

    pthread_mutex_lock(&statistics.mutex);
    stats_record(statistics.requests, requests);
    pthread_mutex_unlock(&statistics.mutex);

    thread->requests = 0;
    thread->start    = time_us();

    return thread->interval;
}

static int header_field(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

void stats_request_completed(connection* c) {
    thread *thread = c->thread;
    uint64_t now = time_us();
    if (now >= thread->stop_at) {
        aeStop(thread->loop);
        goto done;
    }

    thread->complete++;
    thread->requests++;


    // Count all responses (including pipelined ones:)
    c->complete++;

    // Note that expected start time is computed based on the completed
    // response count seen at the beginning of the last request batch sent.
    // A single request batch send may contain multiple requests, and
    // result in multiple responses. If we incorrectly calculated expect
    // start time based on the completion count of these individual pipelined
    // requests we can easily end up "gifting" them time and seeing
    // negative latencies.
    uint64_t expected_latency_start = c->thread_start +
            (c->complete_at_last_batch_start / c->throughput);

    int64_t expected_latency_timing = now - expected_latency_start;

    if (expected_latency_timing < 0) {
        uint64_t actual_latency_timing = now - c->actual_latency_start;
        printf("\n\n ---------- \n\n");
        printf("We are about to crash and die (recoridng a negative # : expected %ld, actual %ld)", expected_latency_timing, actual_latency_timing);
        printf("This wil never ever ever happen...");
        printf("But when it does. The following information will help in debugging");
        printf("response_complete:\n");
        printf("  expected_latency_timing = %" PRId64 "\n", expected_latency_timing);
        printf("  now = %" PRId64 "\n", now);
        printf("  expected_latency_start = %" PRId64 "\n", expected_latency_start);
        printf("  c->thread_start = %" PRId64 "\n", c->thread_start);
        printf("  c->complete = %" PRId64 "\n", c->complete);
        printf("  throughput = %g\n", c->throughput);
        printf("  latest_should_send_time = %" PRId64 "\n", c->latest_should_send_time);
        printf("  latest_expected_start = %" PRId64 "\n", c->latest_expected_start);
        printf("  latest_connect = %" PRId64 "\n", c->latest_connect);
        printf("  latest_write = %" PRId64 "\n", c->latest_write);

        expected_latency_start = c->thread_start +
                ((c->complete ) / c->throughput);
        printf("  next expected_latency_start = %" PRId64 "\n", expected_latency_start);
    }

    c->latest_should_send_time = 0;
    c->latest_expected_start = 0;

    // Record if needed, either last in batch or all, depending in cfg:
    if (cfg.record_all_responses || !c->has_pending) {
        hdr_record_value(thread->latency_histogram, expected_latency_timing);

        uint64_t actual_latency_timing = now - c->actual_latency_start;
//        printf("[%p] LAT %d\n", c, actual_latency_timing);
        hdr_record_value(thread->u_latency_histogram, actual_latency_timing);
    }

done:
    return;
}

static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    int status = parser->status_code;

    if (cfg.verbose)
        printf("Response complete\n");

    if (status > 399) {
        thread->errors.status++;
    }

    if (c->headers.buffer) {
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->headers, &c->body, c->user, c->user_len);
        c->state = FIELD;
    }


    if (--c->pending == 0) {
        c->has_pending = false;
        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    stats_request_completed(c);

    if (!http_should_keep_alive(parser)) {
        if (cfg.verbose)
            fprintf(stderr, "Request complete, reconnecting\n");
        cfg.proto->reconnect(thread, c);
        goto done;
    }

    http_parser_init(parser, HTTP_RESPONSE);

  done:
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    if (cfg.verbose)
        printf("Socket connected!\n");
    connection *c = data;
    eagain:
    switch (sock.connect(c, cfg.host)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
        case AGAIN: goto eagain;
    }

    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->written = 0;

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);

    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

    return;

  error:

    fprintf(stderr, "Error connecting socket, reconnecting\n");
    c->thread->errors.connect++;
    cfg.proto->reconnect(c->thread, c);

}




static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    assert(fd == c->fd);
    sock.writeable(c, loop);
}


static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (sock.read(c, &n)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
            case AGAIN: continue;
        }

        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) {
            fprintf(stderr, "Could not parse query\n");
            goto error;
        }
        c->thread->bytes += n;
    } while (n == RECVBUF && sock.readable(c) > 0);

    return;

  error:

    fprintf(stderr, "Error reading, reconnecting\n");
    c->thread->errors.read++;
    cfg.proto->reconnect(c->thread, c);
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections",    required_argument, NULL, 'c' },
    { "duration",       required_argument, NULL, 'd' },
    { "threads",        required_argument, NULL, 't' },
    { "affinity",       no_argument,       NULL, 'a' },
    { "script",         required_argument, NULL, 's' },
    { "header",         required_argument, NULL, 'H' },
    { "latency",        no_argument,       NULL, 'L' },
    { "u_latency",      no_argument,       NULL, 'U' },
    { "batch_latency",  no_argument,       NULL, 'B' },
    { "bind",        required_argument, NULL, 'b' },
    { "timeout",        required_argument, NULL, 'T' },
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'v' },
    { "rate",           required_argument, NULL, 'R' },
    { "raw",         no_argument,       NULL, 'r' },
#if HAVE_QUIC
    { "quic",           no_argument,        NULL, 'q' },
#endif
    { "http",           required_argument,        NULL, 'p' },
    { NULL,             0,                 NULL,  0  }
};

static int parse_args(struct config *cfg, char **url, struct http_parser_url *parts, char **headers, int argc, char **argv) {
    char c, **header = headers;

    memset(cfg, 0, sizeof(struct config));
    cfg->threads     = 2;
    cfg->affinity    = 0;
    cfg->connections = 10;
    cfg->duration    = 10;
    cfg->timeout     = SOCKET_TIMEOUT_MS;
    cfg->raw         = 0;
    cfg->bind        = 0;
    cfg->rate        = 0;
    cfg->record_all_responses = true;
    cfg->proto = &tcp_proto;
    cfg->http_version = 11;
    cfg->verbose     = 0;

    while ((c = getopt_long(argc, argv, "t:c:d:s:H:b:T:R:p:LUBrqaVv?", longopts, NULL)) != -1) {
        switch (c) {
            case 't':
                if (scan_metric(optarg, &cfg->threads)) return -1;
                break;
            case 'c':
                if (scan_metric(optarg, &cfg->connections)) return -1;
                break;
#if HAVE_QUIC
            case 'q':
                cfg->proto = &quic_proto;
                break;
#endif
            case 'd':
                if (scan_time(optarg, &cfg->duration)) return -1;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'p':
                cfg->http_version = atoi(optarg);
            case 'a':
                cfg->affinity = true;
                break;
            case 'b':
                cfg->bind = inet_addr(optarg);
                break;
            case 'B':
                cfg->record_all_responses = false;
                break;
            case 'U':
                cfg->u_latency = true;
                break;
            case 'T':
                if (scan_time(optarg, &cfg->timeout)) return -1;
                cfg->timeout *= 1000;
                break;
            case 'r':
                cfg->raw = 1;
                break;
            case 'R':
                if (scan_metric(optarg, &cfg->rate)) return -1;
                break;
            case 'V':
                cfg->verbose = 1;
                break;
            case 'v':
                printf("wrk %s [%s] ", VERSION, aeGetApiName());
                printf("Copyright (C) 2012 Will Glozer, 2020-2024 Tom Barbette\n");
                break;
            case 'h':
            case '?':
            case ':':
            default:
                return -1;
        }
    }

    if (optind == argc || !cfg->threads || !cfg->duration) return -1;

    if (!script_parse_url(argv[optind], parts)) {
        fprintf(stderr, "invalid URL: %s\n", argv[optind]);
        return -1;
    }

    if (!cfg->connections || cfg->connections < cfg->threads) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    if (cfg->rate == 0) {
        cfg->rate = -1;
    }

    *url    = argv[optind];
    *header = NULL;

    return 0;
}

static void print_stats_header(bool raw) {
    if (raw) {
        printf("  Thread Stats;%s;%s;%s;%s\n", "Avg", "Stdev", "Max", "+/- Stdev");
    } else {
        printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
    }
}

static void print_units(long double n, char *(*fmt)(long double,int), int width) {
    char *msg = fmt(n,cfg.raw);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    if (cfg.raw)
        printf(" %s ", msg);
    else
        printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double,int)) {
    uint64_t max = stats->max;
    long double mean  = stats_summarize(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    if (cfg.raw) {
        print_units(mean,  fmt, -1);
        print_units(stdev, fmt, -1);
        print_units(max,   fmt, -1);
        printf("%Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
    } else {
        print_units(mean,  fmt, 8);
        print_units(stdev, fmt, 10);
        print_units(max,   fmt, 9);
        printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
    }
}

static void print_hdr_latency(struct hdr_histogram* histogram, const char* description, bool raw) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0};
    printf("  Latency Distribution (HdrHistogram - %s)\n", description);
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        int64_t n = hdr_value_at_percentile(histogram, p);
        if (raw)
            printf("%Lf%%", p);
        else
            printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
    printf("\n%s\n", "  Detailed Percentile spectrum:");
    hdr_percentiles_print(histogram, stdout, 5, 1000.0, CLASSIC);
}

/*static void print_stats_latency(stats *stats, bool raw) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("  Latency Distribution\n");
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        uint64_t n = stats_percentile(stats, p);
        if (raw)
            printf("%Lf%%", p);
        else
            printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
}*/
