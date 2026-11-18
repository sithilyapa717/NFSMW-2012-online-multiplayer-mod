/*
 * Transports:
 *  - vnet:  in-memory switch between endpoints (tests, no sockets)
 *  - fault: wraps any transport and adds loss, duplication, latency, jitter
 *  - udp:   real UDP sockets (Windows)
 */
#ifndef CN_TRANSPORTS_H
#define CN_TRANSPORTS_H

#include "cn_common.h"

/* ---- vnet ---- */
#define VNET_ENDPOINTS 8
#define VNET_INBOX 512

typedef struct {
    cn_addr from;
    int len;
    uint8_t data[CN_MAX_PACKET + 64];
} vnet_pkt;

typedef struct {
    vnet_pkt inbox[VNET_ENDPOINTS][VNET_INBOX];
    int head[VNET_ENDPOINTS], count[VNET_ENDPOINTS];
    int my_index; /* for send(): which endpoint is the sender; set per endpoint below */
} vnet;

typedef struct {
    vnet *net;
    int index;
} vnet_ep;

void vnet_init(vnet *n);
cn_addr vnet_addr(int index);
cn_transport vnet_endpoint(vnet_ep *ep, vnet *n, int index);

/* ---- fault ---- */
#define FAULT_Q 512

typedef struct {
    uint32_t due;
    cn_addr to;
    int len;
    int used;
    uint8_t data[CN_MAX_PACKET + 64];
} fault_slot;

typedef struct {
    cn_transport inner;
    uint32_t rng;
    float loss;       /* 0..1 probability a packet is dropped */
    float dup;        /* 0..1 probability a packet is sent twice */
    uint32_t lat_ms;  /* base one-way latency */
    uint32_t jitter_ms; /* extra random 0..jitter latency (causes reordering) */
    uint32_t now;
    int blackhole;    /* drop everything (simulate a cable pull) */
    uint32_t sent, dropped;
    fault_slot q[FAULT_Q];
} cn_fault;

void cn_fault_init(cn_fault *f, cn_transport inner, uint32_t seed, float loss, float dup, uint32_t lat_ms,
                   uint32_t jitter_ms);
cn_transport cn_fault_transport(cn_fault *f);
/* Deliver packets that are due. Call before every cn_update with the true (shared) time. */
void cn_fault_pump(cn_fault *f, uint32_t now);

/* ---- udp (Windows) ---- */
#ifdef _WIN32
typedef struct {
    uintptr_t sock;
} cn_udp;

/* port 0 = any. Returns 0 on success. */
int cn_udp_open(cn_udp *u, uint16_t port);
void cn_udp_close(cn_udp *u);
cn_transport cn_udp_transport(cn_udp *u);
/* Parse "a.b.c.d:port". Returns 0 on success. */
int cn_addr_parse(const char *text, cn_addr *out);
#endif

#endif
