#include <picoquic.h>
#include <picosocks.h>
#include <picoquic_utils.h>

#include <sys/ioctl.h>

char* ALPN_HQ = "hq-interop";
char* ALPN_SAMPLE_SERVER = "picoquic_sample";

#define DEBUG 0

#if DEBUG
#define debug(s) (printf("[%f - %p]" s "\n", (float)time_us() / 1000000, c))
#define debug(s, ...) (printf("[%f - %p]" s "\n", (float)time_us() / 1000000, c, ##__VA_ARGS__))
#else
#define debug(...)
#endif

status quic_connect(connection *c, char *host, int fd);
static void quic_readable(aeEventLoop *loop, int fd, void *data, int mask);
static void quic_writeable(aeEventLoop *loop, int fd, void *data, int mask);

struct wr {
    picoquic_cnx_t* cnx;
    connection* c;

    long long int ae;

};
struct wr fd_to_cnx[65536];

/**
 * quic_connect_socket -> fd register
 * quic_socket_connected -> fd register
 *  --> quic_readable
 *  --> quic_writeable
 */

/**
 * Called when a socket is connected
 */
static void quic_socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;


        uint64_t now = time_us();
    uint64_t actual_latency_timing = now - c->actual_latency_start;
    (void)actual_latency_timing;
    debug("Quic Socket connected (fd %d, lat %d)\n!",fd, actual_latency_timing);
again:
    switch (quic_connect(c, cfg.host, fd)) {
        case OK:    break;
        case ERROR: goto error;
        case AGAIN: goto again;
        case RETRY: return;
    }

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, quic_readable, c);

    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, quic_writeable, c);

    return;

  error:

    fprintf(stderr, "Error connecting socket, reconnecting\n");
    c->thread->errors.connect++;
    cfg.proto->reconnect(c->thread, c);

}


int write_request(aeEventLoop *loop, long long id, void *data) {
    int fd = (uintptr_t)data;

    connection* c = fd_to_cnx[fd].c;
    debug("Reopen fd %d", fd);
    aeCreateFileEvent(loop, fd, AE_WRITABLE, quic_writeable, c);
    return AE_NOMORE;
}



static int quic_connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags, res;


    //printf("Socket %d %d %d...\n", addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd <= 0) {
        goto oerror;
    }

    fd_to_cnx[fd].c = c;
    int recv_set = 0;
    int send_set = 0;
    picoquic_socket_set_ecn_options(fd, addr->ai_family, &recv_set, &send_set);
    picoquic_socket_set_pkt_info(fd, addr->ai_family);

    debug("Binding...\n");
    struct sockaddr_in localaddr;
    localaddr.sin_family = AF_INET;
    if (c->bind.s_addr != 0) {
        localaddr.sin_addr = c->bind;
    } else {
        localaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    localaddr.sin_port = 0;  // Any local port will do
    int err = bind(fd, (struct sockaddr *)&localaddr, sizeof(localaddr));
    if (err != 0) {
        //printf("Could not bind to given address : errno %d!\n",errno);
        goto error;
    }


    /*//printf("Connect...\n");
    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }*/


//    //printf("Connected...\n");
    c->latest_connect = time_us();

    flags = AE_READABLE | AE_WRITABLE;
    if ((res = aeCreateFileEvent(loop, fd, flags, quic_socket_connected, c)) == AE_OK) {
        c->parser.data = c;
//        c->fd = fd;
        return fd;
    } else {
        //printf("Cannot create file event, error %d for fd %d\n", res, fd);
    }

  error:
    debug("Closing %d", fd);
    close(fd);
  oerror:

    printf("CONNECT ERROR!");
    thread->errors.connect++;
    return -1;
}

static int quic_reconnect_socket(thread *thread, connection *c) {
    debug("Quic reconnect conn");
//    assert(c->fd == 0);
    quic_connect_socket(c->thread, c);
    return 0;
}

int delay_recon(aeEventLoop *loop, long long id, void *data) {
    connection* c = (connection*) data;
    quic_reconnect_socket(c->thread, c);
    return AE_NOMORE;
}

int quic_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    connection* c = (connection*)callback_ctx;
    debug("CALLBACK %d, cx %p!\n",fin_or_event,cnx);
    if (c == NULL) {
        debug("NO C!?");
        /* This should never happen, because the callback context for the client is initialized
         * when creating the client connection. */
        return -1;
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:

        case picoquic_callback_stream_fin:
            //printf("Received stuffs parsing (rcv %d, bytes tot %d)\n", length, c->thread->bytes);
/*            if (http_parser_execute(&c->parser, &parser_settings, (char*)bytes, length) != length) {
                f//printf(stderr, "Could not parse query\n");
                bytes[length] = '\0';
                f//printf(stderr, "%s\n", bytes);

//                goto error;
            }*/
            c->thread->bytes += length;
             {

/*                if (ret == 0 && length > 0) {
                     write the received bytes to the file
                    if (fwrite(bytes, length, 1, stream_ctx->F) != 1) {
                        f//printf(stderr, "Could not write data to disk.\n");
                        ret = -1;
                    }
                    else {
                        stream_ctx->bytes_received += length;
                    }
                }
*/
                if (ret == 0 && fin_or_event == picoquic_callback_stream_fin) {
                    debug("END OF CONNECTION");

                    c->has_pending = false;
                    stats_request_completed(c);
                        ret = picoquic_close(cnx, 0);
//                picoquic_set_callback(cnx, NULL, NULL);
//                    quic_reconnect_socket(c->thread, c);


/*                  stream_ctx->F = picoquic_file_close(stream_ctx->F);
                    stream_ctx->is_stream_finished = 1;
                    client_ctx->nb_files_received++;

                    if ((client_ctx->nb_files_received + client_ctx->nb_files_failed) >= client_ctx->nb_files) {
                        everything is done, close the connection
                        ret = picoquic_close(cnx, 0);
                    }*/
                }
            }
            break;
        case picoquic_callback_stop_sending: /* Should not happen, treated as reset */
            /* Mark stream as abandoned, close the file, etc. */
            picoquic_reset_stream(cnx, stream_id, 0);
            /* Fall through */
        case picoquic_callback_stream_reset: /* Server reset stream #x */
            {
                //printf("RESET\n");
//                    ret = picoquic_close(cnx, 0);
            }


           goto restart;
        case picoquic_callback_stateless_reset:

                        fprintf(stdout, "Connection stateless reset.\n");
            goto restart;

        case picoquic_callback_close: /* Received connection close */
//            fprintf(stdout, "Connection closed.\n");


            goto restart;
        case picoquic_callback_application_close: /* Received application close */
            fprintf(stdout, "Connection app closed.\n");


            /* Mark the connection as completed */
//            client_ctx->is_disconnected = 1;
            /* Remove the application callback */
//            picoquic_set_callback(cnx, NULL, NULL);
            goto restart;
        case picoquic_callback_version_negotiation:
            /* The client did not get the right version.
             * TODO: some form of negotiation?
             */
            fprintf(stdout, "Received a version negotiation request:");
            for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
                uint32_t vn = 0;
                for (int i = 0; i < 4; i++) {
                    vn <<= 8;
                    vn += bytes[byte_index + i];
                }
                fprintf(stdout, "%s%08x", (byte_index == 0) ? " " : ", ", vn);
            }
            fprintf(stdout, "\n");
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        case picoquic_callback_prepare_to_send: {
            debug("Ready to send");
            c->latest_write = time_us();

        if (!c->written && cfg.dynamic) {
            script_request(c->thread->L, &c->request, &c->length, &c->user, &c->user_len);
        }

        char  *buf = c->request + c->written;
        size_t len = c->length  - c->written;
        uint64_t now = time_us();
        uint64_t actual_latency_timing = now - c->actual_latency_start;
        debug("RECON LAT %d", actual_latency_timing);

        c->start = time_us();
            if (!c->has_pending) {
                c->actual_latency_start = c->start;
                c->complete_at_last_batch_start = c->complete;
                c->has_pending = true;
            }
            c->pending = cfg.pipeline;
        //printf("Request size %lu/%lu written %lu\n", len, c->length, c->written);

        uint8_t* buffer;
        size_t available = c->length;
        int is_fin = 1;

                /* The length parameter marks the space available in the packet */
               if (available > length) {
                    available = length;
                    is_fin = 0;
                    //printf("Warning! Request too long\n");
                }
                /* Needs to retrieve a pointer to the actual buffer
                 * the "bytes" parameter points to the sending context
                 */
                buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
                if (buffer != NULL) {
                    memcpy(buffer, buf, available);
                }
                else {
                    ret = -1;
                }
            }
                break;
        case picoquic_callback_almost_ready:
             debug("Connection to the server completed, almost ready.\n");
        http_parser_init(&c->parser, HTTP_RESPONSE);
        c->written = 0;
            struct sockaddr* peer_addr;

            struct sockaddr* local_addr;
            picoquic_get_peer_addr(cnx, &peer_addr);

            picoquic_get_local_addr(cnx, &local_addr);

//            fprintf(stdout, "Peer addr %x (fam %d), Local addr %x (fam %d)\n",((struct sockaddr_in*)peer_addr)->sin_addr, peer_addr->sa_family, ((struct sockaddr_in*)local_addr)->sin_addr, local_addr->sa_family);

            break;
        case picoquic_callback_ready:
            /* TODO: Check that the transport parameters are what the sample expects */
            debug("Connection to the server confirmed.\n");
            break;
        default:
            debug("Unexpected callback!?\n");
            /* unexpected -- just ignore. */
            break;
        }
    }

    return ret;

restart:
    {
        uint64_t time_usec_to_wait = usec_to_next_send(c);
        if (time_usec_to_wait) {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            // Not yet time to send. Delay:
//            aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE);
            aeCreateTimeEvent(c->thread->loop, msec_to_wait, delay_recon, c, NULL);
        } else {
//            aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);

                quic_reconnect_socket(c->thread, c);
        }
    }
    return ret;
}



picoquic_quic_t *quic_init() {
        int ret = 0;
        uint64_t current_time = picoquic_current_time();
        char* ticket_store_filename = "ticket_store.bin";
        char* token_store_filename = "token_store.bin";
        //printf("Creating QUIC for thread\n");
	    picoquic_quic_t *quic = picoquic_create(1, NULL, NULL, NULL, "picoquic_sample", NULL, NULL,
            NULL, NULL, NULL, current_time, NULL,
            ticket_store_filename, NULL, 0);

        if (quic == NULL) {
           fprintf(stderr, "Could not create quic object\n");
            ret = -1;
        } else {
/*            if (picoquic_load_retry_tokens(quic, token_store_filename) != 0) {
                fprintf(stderr, "No token file present. Will create one as <%s>.\n", token_store_filename);
            }*/

            picoquic_set_default_congestion_algorithm(quic, picoquic_bbr_algorithm);

/*            picoquic_set_key_log_file_from_env(quic);
            picoquic_set_qlog(quic, "./");
            picoquic_set_log_level(quic, 1);*/
        }
        if (ret != 0)
            return 0;
        return quic;
}

status quic_connect(connection *c, char *host, int fd) {
    picoquic_quic_t* quic = c->thread->quic;
    uint64_t current_time = picoquic_current_time();
    struct addrinfo *addr = c->thread->addr;
    char const* sni = host;
    int ret;
    struct sockaddr_in* sin = (struct sockaddr_in*)addr->ai_addr;
    debug("Creating Context for addr %x (host %s), alpn %s\n", sin->sin_addr, host,c->thread->alpn);
    char* alpn = c->thread->alpn;
    assert(quic);
    c->written = 0;
		picoquic_cnx_t* cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)sin, current_time, 0, sni, alpn, 1);
                fd_to_cnx[fd].cnx = cnx;

        if (cnx == NULL) {
            fprintf(stderr, "Could not create connection context\n");
            ret = -1;
        } else {
            debug("Starting Client Context\n");
            c->cnx = cnx;
            /* Set the client callback context */
            picoquic_set_callback(cnx, quic_callback, c);
            /* Client connection parameters could be set here, before starting the connection. */
            ret = picoquic_start_client_cnx(cnx);
            if (ret < 0) {
                fprintf(stderr, "Could not activate connection\n");
            } else {
                /* Printing out the initial CID, which is used to identify log files */
                picoquic_connection_id_t icid = picoquic_get_initial_cnxid(cnx);
                //printf("Initial connection ID: ");
                for (uint8_t i = 0; i < icid.id_len; i++) {
                    //printf("%02x", icid.id[i]);
                }
                //printf("\n");
            }
            int stream_id = 0;
            //printf("Marking stream %d\n", stream_id);
            ret = picoquic_mark_active_stream(cnx, stream_id, 1, c);
            if (ret != 0) {
                fprintf(stdout, "Error %d, cannot initialize stream for file number %d\n", ret, stream_id);
                                    }
             else {
                debug("Opened stream %d\n", stream_id);
            }

        }


    return OK;
}

status quic_read(connection *c,  size_t *n, int fd) {

	picoquic_quic_t* quic = c->thread->quic;
    uint8_t recv_buffer[1536];
    size_t recv_length = 1536;
    uint64_t current_time = picoquic_current_time();
//    int64_t delta_t;
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage local_addr;
    int if_index = 0;
    int ret;
    unsigned char received_ecn;
//        picoquic_connection_id_t log_cid;
//        int sock_ret = 0;
//        int sock_err = 0;
//
//    ssize_t r = read(c->fd, recv_buffer, recv_length);
//    *n = (size_t) r;
    recv_length = picoquic_recvmsg(fd, &peer_addr,
                         &local_addr, &if_index, &received_ecn,
                         (unsigned char*)&recv_buffer, recv_length);
    if (recv_length < 0) {

        //printf("RCV error\n", recv_length);
        return -1;
    }
/*    struct sockaddr_storage local_address;
    picoquic_get_local_address(s_socket[0], &local_address);
     socket_port = ((struct sockaddr_in*) & local_address)->sin_port;*/
    debug("READ %lu from FD %d", recv_length,fd);
    ret = picoquic_incoming_packet(quic, recv_buffer,
                    (size_t)recv_length, (struct sockaddr*) & peer_addr,
                    (struct sockaddr*) & local_addr, if_index, received_ecn,
                    current_time);

    //printf("READ FINISHED WITH STATUS %d, scheduling write if need be\n", ret);
    if (fd_to_cnx[fd].ae) {
        debug("DELETE EVENT FOR FD %d", fd);
        aeDeleteTimeEvent(c->thread->loop, fd_to_cnx[fd].ae);
        fd_to_cnx[fd].ae = 0;
        aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, quic_writeable, c);

    }
    n = 0;

/*
else{
            // No incoming packet, so check whether there is something to send
*/

/*    int r;
    if ((r = SSL_read(c->ssl, c->buf, sizeof(c->buf))) <= 0) {
        switch (SSL_get_error(c->ssl, r)) {
            case SSL_ERROR_WANT_READ:  return RETRY;
            case SSL_ERROR_WANT_WRITE: return RETRY;
            default:                   return ERROR;
        }
    }
    *n = (size_t) r;*/
    return AGAIN;
}

status quic_write(connection *c, char *buf, size_t len, size_t *n) {

//    picoquic_provide_stream_data_buffer(buf, available, is_fin, !is_fin);

 /*   if ((r = SSL_write(c->ssl, buf, len)) <= 0) {
        switch (SSL_get_error(c->ssl, r)) {
            case SSL_ERROR_WANT_READ:  return RETRY;
            case SSL_ERROR_WANT_WRITE: return RETRY;
            default:                   return ERROR;
        }
    }
    *n = (size_t) r;*/

    return OK;
}

size_t fd_readable(int fd) {
    int n, rc;
    rc = ioctl(fd, FIONREAD, &n);
    return rc == -1 ? 0 : n;
}

static void quic_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;

    debug("FD %d READABLE", fd);
    size_t n;

    do {
        switch (quic_read(c, &n, fd)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
            case AGAIN: continue;
        }

        assert(false);
    } while (n == RECVBUF && fd_readable(fd) > 0);

    return;

  error:

    fprintf(stderr, "Error reading, reconnecting\n");
    c->thread->errors.read++;
//    cfg.proto->reconnect(c->thread, c);
}

static void quic_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    debug("FD %d WRITEABLE", fd);
	uint8_t send_buffer[1536];
    size_t send_length = 0;
    picoquic_quic_t* quic = c->thread->quic;
	int ret;
	int sock_ret;
    int sock_err;
	struct sockaddr_storage peer_addr;
    struct sockaddr_storage local_addr;
    int if_index = 0;
	//picoquic_connection_id_t log_cid;
    (void)quic;
    uint64_t current_time = picoquic_current_time();

    picoquic_cnx_t* cnx = fd_to_cnx[fd].cnx;
    ret = picoquic_prepare_packet(cnx, current_time,
                send_buffer, sizeof(send_buffer), &send_length,
                &peer_addr, &local_addr, &if_index);

    if (ret == 0 && send_length > 0) {

        struct sockaddr_in *speer = (struct sockaddr_in *)&peer_addr;
        struct sockaddr_in *slocal = (struct sockaddr_in *)&local_addr;

        // Send the packet that was just prepared
        sock_ret = picoquic_send_through_socket(fd,
            (struct sockaddr*) & peer_addr, (struct sockaddr*)&local_addr, if_index,
            (const char*)send_buffer, (int)send_length, &sock_err);
        if (sock_ret <= 0) {
            printf("Could not send message to AF_to=%d, AF_from=%d, fd=%d, ret=%d, err=%d\n",
                peer_addr.ss_family, local_addr.ss_family,fd,  sock_ret, sock_err);
        }
    } else {
        if (ret != 0) {
           debug("ERRRORR !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

                aeDeleteFileEvent(loop, fd, AE_WRITABLE | AE_READABLE);
                close(fd);
                picoquic_delete_cnx(cnx);
        } else {

            int delta_t = picoquic_get_wake_delay(cnx, current_time, 1000);
            //printf("Wakeup in %d msec. Return was %d. Length %d\n", delta_t, ret, send_length);
            if (delta_t > 0) {
                    debug("Delaying fd %d for %dms", fd, delta_t);
                aeDeleteFileEvent(loop, fd, AE_WRITABLE);
                fd_to_cnx[fd].ae = aeCreateTimeEvent(
                        thread->loop, delta_t, write_request, (void*)fd, NULL);
            }
             else
             {
                    debug("Scheduling now fd %d (delta %d)", fd, delta_t);
                 aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, quic_writeable, c);
             }
        }
    }
    debug("RETURN\n");
    return;
}

struct proto quic_proto = {
    .connect = quic_connect_socket,
    .reconnect = quic_reconnect_socket
};
