#include "cn_transports.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------- vnet */

void vnet_init(vnet *n) { memset(n, 0, sizeof(*n)); }

cn_addr vnet_addr(int index)
{
    cn_addr a;
    a.ip = 0x0A000001u + (uint32_t)index; /* 10.0.0.(1+index) */
    a.port = 47000;
    return a;
}

static int vnet_send(void *user, const cn_addr *to, const uint8_t *data, int len)
{
    vnet_ep *ep = (vnet_ep *)user;
    vnet *n = ep->net;
    int dst = (int)(to->ip - 0x0A000001u);
    if (dst < 0 || dst >= VNET_ENDPOINTS || len > CN_MAX_PACKET + 64)
        return -1;
    if (n->count[dst] >= VNET_INBOX)
        return -1; /* inbox full: behaves like a dropped packet */
    vnet_pkt *p = &n->inbox[dst][(n->head[dst] + n->count[dst]) % VNET_INBOX];
    p->from = vnet_addr(ep->index);
    p->len = len;
    memcpy(p->data, data, (size_t)len);
    n->count[dst]++;
    return len;
}

static int vnet_recv(void *user, cn_addr *from, uint8_t *buf, int cap)
{
    vnet_ep *ep = (vnet_ep *)user;
    vnet *n = ep->net;
    int i = ep->index;
    if (n->count[i] == 0)
        return 0;
    vnet_pkt *p = &n->inbox[i][n->head[i]];
    n->head[i] = (n->head[i] + 1) % VNET_INBOX;
    n->count[i]--;
    if (p->len > cap)
        return 0;
    *from = p->from;
    memcpy(buf, p->data, (size_t)p->len);
    return p->len;
}

cn_transport vnet_endpoint(vnet_ep *ep, vnet *n, int index)
{
    ep->net = n;
    ep->index = index;
    cn_transport t;
    t.user = ep;
    t.send = vnet_send;
    t.recv = vnet_recv;
    return t;
}

/* ------------------------------------------------------------------ fault */

static uint32_t frnd(cn_fault *f)
{
    uint32_t x = f->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    f->rng = x ? x : 0x1234567u;
    return f->rng;
}

static float frnd01(cn_fault *f) { return (float)(frnd(f) >> 8) / 16777216.0f; }

static void fault_enqueue(cn_fault *f, const cn_addr *to, const uint8_t *data, int len)
{
    for (int i = 0; i < FAULT_Q; i++) {
        if (!f->q[i].used) {
            uint32_t delay = f->lat_ms + (f->jitter_ms ? frnd(f) % (f->jitter_ms + 1) : 0);
            f->q[i].used = 1;
            f->q[i].due = f->now + delay;
            f->q[i].to = *to;
            f->q[i].len = len;
            memcpy(f->q[i].data, data, (size_t)len);
            return;
        }
    }
    f->dropped++; /* queue full */
}

static int fault_send(void *user, const cn_addr *to, const uint8_t *data, int len)
{
    cn_fault *f = (cn_fault *)user;
    f->sent++;
    if (len > CN_MAX_PACKET + 64)
        return -1;
    if (f->blackhole || frnd01(f) < f->loss) {
        f->dropped++;
        return len;
    }
    fault_enqueue(f, to, data, len);
    if (f->dup > 0.0f && frnd01(f) < f->dup)
        fault_enqueue(f, to, data, len);
    return len;
}

static int fault_recv(void *user, cn_addr *from, uint8_t *buf, int cap)
{
    cn_fault *f = (cn_fault *)user;
    return f->inner.recv(f->inner.user, from, buf, cap);
}

void cn_fault_init(cn_fault *f, cn_transport inner, uint32_t seed, float loss, float dup, uint32_t lat_ms,
                   uint32_t jitter_ms)
{
    memset(f, 0, sizeof(*f));
    f->inner = inner;
    f->rng = seed ? seed : 1;
    f->loss = loss;
    f->dup = dup;
    f->lat_ms = lat_ms;
    f->jitter_ms = jitter_ms;
}

cn_transport cn_fault_transport(cn_fault *f)
{
    cn_transport t;
    t.user = f;
    t.send = fault_send;
    t.recv = fault_recv;
    return t;
}

void cn_fault_pump(cn_fault *f, uint32_t now)
{
    f->now = now;
    for (;;) {
        int best = -1;
        for (int i = 0; i < FAULT_Q; i++) {
            if (f->q[i].used && (int32_t)(f->q[i].due - now) <= 0 &&
                (best < 0 || (int32_t)(f->q[i].due - f->q[best].due) < 0))
                best = i;
        }
        if (best < 0)
            break;
        f->inner.send(f->inner.user, &f->q[best].to, f->q[best].data, f->q[best].len);
        f->q[best].used = 0;
    }
}

/* -------------------------------------------------------------------- udp */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

int cn_udp_open(cn_udp *u, uint16_t port)
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return -1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(s);
        return -1;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    /* Windows reports ICMP "port unreachable" as a recv error; turn that off. */
    BOOL no = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &no, sizeof(no), NULL, 0, &ret, NULL, NULL);
    u->sock = (uintptr_t)s;
    return 0;
}

void cn_udp_close(cn_udp *u)
{
    if (u->sock)
        closesocket((SOCKET)u->sock);
    u->sock = 0;
}

static int udp_send(void *user, const cn_addr *to, const uint8_t *data, int len)
{
    cn_udp *u = (cn_udp *)user;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to->ip);
    a.sin_port = htons(to->port);
    return sendto((SOCKET)u->sock, (const char *)data, len, 0, (struct sockaddr *)&a, sizeof(a));
}

static int udp_recv(void *user, cn_addr *from, uint8_t *buf, int cap)
{
    cn_udp *u = (cn_udp *)user;
    struct sockaddr_in a;
    int al = (int)sizeof(a);
    int n = recvfrom((SOCKET)u->sock, (char *)buf, cap, 0, (struct sockaddr *)&a, &al);
    if (n <= 0)
        return 0;
    from->ip = ntohl(a.sin_addr.s_addr);
    from->port = ntohs(a.sin_port);
    return n;
}

cn_transport cn_udp_transport(cn_udp *u)
{
    cn_transport t;
    t.user = u;
    t.send = udp_send;
    t.recv = udp_recv;
    return t;
}

int cn_addr_parse(const char *text, cn_addr *out)
{
    unsigned a, b, c, d, port;
    if (sscanf(text, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) != 5)
        return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255 || port > 65535)
        return -1;
    out->ip = (a << 24) | (b << 16) | (c << 8) | d;
    out->port = (uint16_t)port;
    return 0;
}
#endif
