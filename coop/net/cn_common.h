/* Co-op net library: common types. No game headers allowed in coop/net. */
#ifndef CN_COMMON_H
#define CN_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CN_PROTOCOL_VERSION 1
#define CN_MAX_PLAYERS 4 /* host + 3 guests; player id 0 is always the host */
#define CN_NAME_LEN 16   /* including terminating NUL */
#define CN_CAR_BLOB 32   /* opaque car description chosen by the app */
#define CN_MAX_PACKET 1200

typedef struct {
    uint32_t ip;   /* host byte order */
    uint16_t port; /* host byte order */
} cn_addr;

static inline int cn_addr_eq(const cn_addr *a, const cn_addr *b)
{
    return a->ip == b->ip && a->port == b->port;
}

/* Datagram transport. recv returns the datagram length, or 0 if none waiting. */
typedef struct cn_transport {
    void *user;
    int (*send)(void *user, const cn_addr *to, const uint8_t *data, int len);
    int (*recv)(void *user, cn_addr *from, uint8_t *buf, int cap);
} cn_transport;

/* One sample of a car. Rotation is a unit quaternion (x, y, z, w). */
typedef struct {
    float pos[3];
    float rot[4];
    float vel[3];
    float steer;
    uint16_t flags;
} cn_car_state;

#endif
