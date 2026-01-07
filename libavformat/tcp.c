/*
 * TCP protocol
 * Copyright (c) 2002 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/parseutils.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"
#if HAVE_POLL_H
#include <poll.h>
#endif
#include <arpa/inet.h>
#include <net/if.h>

typedef struct TCPContext {
    const AVClass *class;
    int fd;
    int listen;
    int open_timeout;
    int rw_timeout;
    int listen_timeout;
    int recv_buffer_size;
    int send_buffer_size;
    int tcp_nodelay;
    char *localaddr;
    char *localif;
    unsigned int local_ifindex;
    int has_local_if;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;
    int has_local_addr;
#if !HAVE_WINSOCK2_H
    int tcp_mss;
#endif /* !HAVE_WINSOCK2_H */
} TCPContext;

#define OFFSET(x) offsetof(TCPContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "listen",          "Listen for incoming connections",  OFFSET(listen),         AV_OPT_TYPE_INT, { .i64 = 0 },     0,       2,       .flags = D|E },
    { "timeout",     "set timeout (in microseconds) of socket I/O operations", OFFSET(rw_timeout),     AV_OPT_TYPE_INT, { .i64 = -1 },         -1, INT_MAX, .flags = D|E },
    { "listen_timeout",  "Connection awaiting timeout (in milliseconds)",      OFFSET(listen_timeout), AV_OPT_TYPE_INT, { .i64 = -1 },         -1, INT_MAX, .flags = D|E },
    { "send_buffer_size", "Socket send buffer size (in bytes)",                OFFSET(send_buffer_size), AV_OPT_TYPE_INT, { .i64 = -1 },         -1, INT_MAX, .flags = D|E },
    { "recv_buffer_size", "Socket receive buffer size (in bytes)",             OFFSET(recv_buffer_size), AV_OPT_TYPE_INT, { .i64 = -1 },         -1, INT_MAX, .flags = D|E },
    { "tcp_nodelay", "Use TCP_NODELAY to disable nagle's algorithm",           OFFSET(tcp_nodelay), AV_OPT_TYPE_BOOL, { .i64 = 0 },             0, 1, .flags = D|E },
    { "localaddr",   "Local address",                                           OFFSET(localaddr),      AV_OPT_TYPE_STRING, { .str = NULL },       .flags = D|E },
    { "localif",     "Local interface name",                                    OFFSET(localif),        AV_OPT_TYPE_STRING, { .str = NULL },       .flags = D|E },
#if !HAVE_WINSOCK2_H
    { "tcp_mss",     "Maximum segment size for outgoing TCP packets",          OFFSET(tcp_mss),     AV_OPT_TYPE_INT, { .i64 = -1 },         -1, INT_MAX, .flags = D|E },
#endif /* !HAVE_WINSOCK2_H */
    { NULL }
};

static const AVClass tcp_class = {
    .class_name = "tcp",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void customize_fd(void *ctx, int fd)
{
    TCPContext *s = ctx;
    if (s->has_local_if) {
#ifdef IP_BOUND_IF
        if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &s->local_ifindex, sizeof(s->local_ifindex)) < 0) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(IP_BOUND_IF)");
        }
#endif
#ifdef IPV6_BOUND_IF
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &s->local_ifindex, sizeof(s->local_ifindex)) < 0) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(IPV6_BOUND_IF)");
        }
#endif
    }
    if (!s->listen && s->has_local_addr) {
        if (bind(fd, (struct sockaddr *)&s->local_addr, s->local_addr_len) < 0) {
            ff_log_net_error(ctx, AV_LOG_ERROR, "bind(localaddr)");
            closesocket(fd);
            return;
        }
    }
    /* Set the socket's send or receive buffer sizes, if specified.
       If unspecified or setting fails, system default is used. */
    if (s->recv_buffer_size > 0) {
        if (setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &s->recv_buffer_size, sizeof (s->recv_buffer_size))) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(SO_RCVBUF)");
        }
    }
    if (s->send_buffer_size > 0) {
        if (setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &s->send_buffer_size, sizeof (s->send_buffer_size))) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(SO_SNDBUF)");
        }
    }
    if (s->tcp_nodelay > 0) {
        if (setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &s->tcp_nodelay, sizeof (s->tcp_nodelay))) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(TCP_NODELAY)");
        }
    }
#if !HAVE_WINSOCK2_H
    if (s->tcp_mss > 0) {
        if (setsockopt (fd, IPPROTO_TCP, TCP_MAXSEG, &s->tcp_mss, sizeof (s->tcp_mss))) {
            ff_log_net_error(ctx, AV_LOG_WARNING, "setsockopt(TCP_MAXSEG)");
        }
    }
#endif /* !HAVE_WINSOCK2_H */
}

/* return non zero if error */
static int tcp_open(URLContext *h, const char *uri, int flags)
{
    struct addrinfo hints = { 0 }, *ai, *cur_ai;
    int port, fd = -1;
    TCPContext *s = h->priv_data;
    const char *p;
    char buf[256];
    int ret;
    char hostname[1024],proto[1024],path[1024];
    char portstr[10];
    s->open_timeout = 5000000;
    s->has_local_addr = 0;
    s->local_addr_len = 0;
    s->has_local_if = 0;

    av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname),
        &port, path, sizeof(path), uri);
    if (strcmp(proto, "tcp"))
        return AVERROR(EINVAL);
    if (port <= 0 || port >= 65536) {
        av_log(h, AV_LOG_ERROR, "Port missing in uri\n");
        return AVERROR(EINVAL);
    }
    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "listen", p)) {
            char *endptr = NULL;
            s->listen = strtol(buf, &endptr, 10);
            /* assume if no digits were found it is a request to enable it */
            if (buf == endptr)
                s->listen = 1;
        }
        if (av_find_info_tag(buf, sizeof(buf), "timeout", p)) {
            s->rw_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "listen_timeout", p)) {
            s->listen_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localaddr", p)) {
            av_freep(&s->localaddr);
            s->localaddr = av_strdup(buf);
            if (!s->localaddr)
                return AVERROR(ENOMEM);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localif", p)) {
            av_freep(&s->localif);
            s->localif = av_strdup(buf);
            if (!s->localif)
                return AVERROR(ENOMEM);
        }
    }
    if (s->localif && s->localif[0]) {
        s->local_ifindex = if_nametoindex(s->localif);
        if (!s->local_ifindex) {
            av_log(h, AV_LOG_ERROR, "Unknown interface name %s\n", s->localif);
            return AVERROR(EINVAL);
        }
        s->has_local_if = 1;
    }
    if (s->rw_timeout >= 0) {
        s->open_timeout =
        h->rw_timeout   = s->rw_timeout;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (s->listen)
        hints.ai_flags |= AI_PASSIVE;
    if (!s->listen && s->localaddr && s->localaddr[0]) {
        struct in_addr ipv4_addr;
#if HAVE_STRUCT_SOCKADDR_IN6
        struct in6_addr ipv6_addr;
#endif
        if (inet_pton(AF_INET, s->localaddr, &ipv4_addr) == 1) {
            hints.ai_family = AF_INET;
#if HAVE_STRUCT_SOCKADDR_IN6
        } else if (inet_pton(AF_INET6, s->localaddr, &ipv6_addr) == 1) {
            hints.ai_family = AF_INET6;
#endif
        }
    }
    if (hostname[0]) {
        struct in_addr ipv4_addr;
        if (inet_pton(AF_INET, hostname, &ipv4_addr) == 1) {
            // Avoid NAT64 synthesis for numeric IPv4 addresses (e.g., 192.168.x.x).
            hints.ai_family = AF_INET;
            hints.ai_flags |= AI_NUMERICHOST;
        }
    }
    if (!hostname[0])
        ret = getaddrinfo(NULL, portstr, &hints, &ai);
    else
        ret = getaddrinfo(hostname, portstr, &hints, &ai);
    if (ret) {
        av_log(h, AV_LOG_ERROR,
               "Failed to resolve hostname %s: %s\n",
               hostname, gai_strerror(ret));
        return AVERROR(EIO);
    }

    cur_ai = ai;

#if HAVE_STRUCT_SOCKADDR_IN6
    // workaround for IOS9 getaddrinfo in IPv6 only network use hardcode IPv4 address can not resolve port number.
    if (cur_ai->ai_family == AF_INET6){
        struct sockaddr_in6 * sockaddr_v6 = (struct sockaddr_in6 *)cur_ai->ai_addr;
        if (!sockaddr_v6->sin6_port){
            sockaddr_v6->sin6_port = htons(port);
        }
    }
#endif
    if (!s->listen && s->localaddr && s->localaddr[0]) {
        struct addrinfo lhints = { 0 }, *lai = NULL;
        lhints.ai_socktype = SOCK_STREAM;
        lhints.ai_family = hints.ai_family;
        if (lhints.ai_family == AF_UNSPEC) {
            struct in_addr ipv4_addr;
#if HAVE_STRUCT_SOCKADDR_IN6
            struct in6_addr ipv6_addr;
#endif
            if (inet_pton(AF_INET, s->localaddr, &ipv4_addr) == 1) {
                lhints.ai_family = AF_INET;
                lhints.ai_flags |= AI_NUMERICHOST;
#if HAVE_STRUCT_SOCKADDR_IN6
            } else if (inet_pton(AF_INET6, s->localaddr, &ipv6_addr) == 1) {
                lhints.ai_family = AF_INET6;
                lhints.ai_flags |= AI_NUMERICHOST;
#endif
            }
        }
        ret = getaddrinfo(s->localaddr, NULL, &lhints, &lai);
        if (ret) {
            av_log(h, AV_LOG_ERROR,
                   "Failed to resolve local address %s: %s\n",
                   s->localaddr, gai_strerror(ret));
            ret = AVERROR(EIO);
            goto fail1;
        }
        if (lai && lai->ai_addr && lai->ai_addrlen <= sizeof(s->local_addr)) {
            memcpy(&s->local_addr, lai->ai_addr, lai->ai_addrlen);
            s->local_addr_len = lai->ai_addrlen;
            s->has_local_addr = 1;
        }
        freeaddrinfo(lai);
    }

    if (s->listen > 0) {
        while (cur_ai && fd < 0) {
            fd = ff_socket(cur_ai->ai_family,
                           cur_ai->ai_socktype,
                           cur_ai->ai_protocol);
            if (fd < 0) {
                ret = ff_neterrno();
                cur_ai = cur_ai->ai_next;
            }
        }
        if (fd < 0)
            goto fail1;
        customize_fd(s, fd);
    }

    if (s->listen == 2) {
        // multi-client
        if ((ret = ff_listen(fd, cur_ai->ai_addr, cur_ai->ai_addrlen)) < 0)
            goto fail1;
    } else if (s->listen == 1) {
        // single client
        if ((ret = ff_listen_bind(fd, cur_ai->ai_addr, cur_ai->ai_addrlen,
                                  s->listen_timeout, h)) < 0)
            goto fail1;
        // Socket descriptor already closed here. Safe to overwrite to client one.
        fd = ret;
    } else {
        ret = ff_connect_parallel(ai, s->open_timeout / 1000, 3, h, &fd, customize_fd, s);
        if (ret < 0)
            goto fail1;
    }

    h->is_streamed = 1;
    s->fd = fd;

    freeaddrinfo(ai);
    return 0;

 fail1:
    if (fd >= 0)
        closesocket(fd);
    freeaddrinfo(ai);
    return ret;
}

static int tcp_accept(URLContext *s, URLContext **c)
{
    TCPContext *sc = s->priv_data;
    TCPContext *cc;
    int ret;
    av_assert0(sc->listen);
    if ((ret = ffurl_alloc(c, s->filename, s->flags, &s->interrupt_callback)) < 0)
        return ret;
    cc = (*c)->priv_data;
    ret = ff_accept(sc->fd, sc->listen_timeout, s);
    if (ret < 0) {
        ffurl_closep(c);
        return ret;
    }
    cc->fd = ret;
    return 0;
}

static int tcp_read(URLContext *h, uint8_t *buf, int size)
{
    TCPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd_timeout(s->fd, 0, h->rw_timeout, &h->interrupt_callback);
        if (ret)
            return ret;
    }
    ret = recv(s->fd, buf, size, 0);
    if (ret == 0)
        return AVERROR_EOF;
    return ret < 0 ? ff_neterrno() : ret;
}

static int tcp_write(URLContext *h, const uint8_t *buf, int size)
{
    TCPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd_timeout(s->fd, 1, h->rw_timeout, &h->interrupt_callback);
        if (ret)
            return ret;
    }
    ret = send(s->fd, buf, size, MSG_NOSIGNAL);
    return ret < 0 ? ff_neterrno() : ret;
}

static int tcp_shutdown(URLContext *h, int flags)
{
    TCPContext *s = h->priv_data;
    int how;

    if (flags & AVIO_FLAG_WRITE && flags & AVIO_FLAG_READ) {
        how = SHUT_RDWR;
    } else if (flags & AVIO_FLAG_WRITE) {
        how = SHUT_WR;
    } else {
        how = SHUT_RD;
    }

    return shutdown(s->fd, how);
}

static int tcp_close(URLContext *h)
{
    TCPContext *s = h->priv_data;
    av_freep(&s->localaddr);
    av_freep(&s->localif);
    closesocket(s->fd);
    return 0;
}

static int tcp_get_file_handle(URLContext *h)
{
    TCPContext *s = h->priv_data;
    return s->fd;
}

static int tcp_get_window_size(URLContext *h)
{
    TCPContext *s = h->priv_data;
    int avail;
    socklen_t avail_len = sizeof(avail);

#if HAVE_WINSOCK2_H
    /* SO_RCVBUF with winsock only reports the actual TCP window size when
    auto-tuning has been disabled via setting SO_RCVBUF */
    if (s->recv_buffer_size < 0) {
        return AVERROR(ENOSYS);
    }
#endif

    if (getsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &avail, &avail_len)) {
        return ff_neterrno();
    }
    return avail;
}

const URLProtocol ff_tcp_protocol = {
    .name                = "tcp",
    .url_open            = tcp_open,
    .url_accept          = tcp_accept,
    .url_read            = tcp_read,
    .url_write           = tcp_write,
    .url_close           = tcp_close,
    .url_get_file_handle = tcp_get_file_handle,
    .url_get_short_seek  = tcp_get_window_size,
    .url_shutdown        = tcp_shutdown,
    .priv_data_size      = sizeof(TCPContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &tcp_class,
};
