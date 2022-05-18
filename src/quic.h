#include <picoquic.h>
#include <picosocks.h>
#include <picoquic_utils.h>
#include <assert.h>
#include "net.h"

#include <sys/ioctl.h>


static const char* ALPN_HQ = "hq-interop";
static const char* ALPN_H3 = "h3";
static const char* ALPN_SAMPLE_SERVER = "picoquic_sample";

#ifndef DEBUG
#define DEBUG 0
#if DEBUG
#define debug(s) (printf("[%f - %p]" s "\n", (float)time_us() / 1000000, c))
#define debug(s, ...) (printf("[%f - %p]" s "\n", (float)time_us() / 1000000, c, ##__VA_ARGS__))
#else
#define debug(...)
#endif
#endif

status quic_connect(connection *c, char *host, int fd);
void quic_readable(aeEventLoop *loop, int fd, void *data, int mask);
void quic_writeable(aeEventLoop *loop, int fd, void *data, int mask);

struct wr {
    picoquic_cnx_t* cnx;
    connection* c;

    long long int ae;

};
extern struct wr fd_to_cnx[];


picoquic_quic_t *quic_init();

int quic_connect_socket(thread *thread, connection *c);

int quic_reconnect_socket(thread *thread, connection *c);

int quic_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
        picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx);

status quic_connect(connection *c, char *host, int fd);

status quic_read(connection *c,  size_t *n, int fd);
status quic_write(connection *c, char *buf, size_t len, size_t *n);

void quic_readable(aeEventLoop *loop, int fd, void *data, int mask);

void quic_writeable(aeEventLoop *loop, int fd, void *data, int mask);

extern struct proto quic_proto;