static struct sock sock = {
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};

static int tcp_connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (c->bind.s_addr != 0) {
        struct sockaddr_in localaddr;
        localaddr.sin_family = AF_INET;
        localaddr.sin_addr = c->bind;
        localaddr.sin_port = 0;  // Any local port will do
        int err = bind(fd, (struct sockaddr *)&localaddr, sizeof(localaddr));
        if (err != 0) {
            printf("Could not bind to given address : errno %d!\n",errno);
            goto error;
        }
    }

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    c->latest_connect = time_us();

    flags = AE_READABLE | AE_WRITABLE;
    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

  error:
    thread->errors.connect++;
    close(fd);
    return -1;
}

static int tcp_reconnect_socket(thread *thread, connection *c) {
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    close(c->fd);
    return tcp_connect_socket(thread, c);
}


struct proto tcp_proto = {
    .connect = tcp_connect_socket,
    .reconnect = tcp_reconnect_socket
};
