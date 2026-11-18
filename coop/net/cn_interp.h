/*
 * Interpolation buffer for one remote car.
 *
 * Samples are stamped with host time (ms). The caller asks for the car at
 * "now" in host time; we render a little in the past so there are normally
 * samples on both sides. The delay adapts: it follows the worst transit time
 * seen recently (plus one send interval), moving at most 10% of real time so
 * the cars never jump. If data stops we extrapolate with the
 * velocity for up to CN_EXTRAP_MAX_MS, then hold the car as a "ghost", and
 * after CN_GHOST_MS we report it removed. When data resumes after an
 * extrapolation the car slides back instead of teleporting.
 */
#ifndef CN_INTERP_H
#define CN_INTERP_H

#include "cn_common.h"

#define CN_INTERP_CAP 32
#define CN_INTERP_DELAY_MS 100     /* initial render delay, before any measurement */
#define CN_DELAY_MIN_MS 50
#define CN_DELAY_MAX_MS 300
#define CN_DELAY_MARGIN_MS 43      /* one send interval (33 ms) plus slack */
#define CN_TRANSIT_WINDOW 32
#define CN_EXTRAP_MAX_MS 500
#define CN_GHOST_MS 5000
#define CN_SNAP_DIST 30.0f   /* corrections bigger than this snap instead of sliding */
#define CN_SMOOTH_MS 150.0f  /* time constant of the slide-back */

typedef enum {
    CN_PRES_EMPTY = 0, /* no data yet */
    CN_PRES_ACTIVE,    /* interpolating real data */
    CN_PRES_EXTRAP,    /* data late, predicting */
    CN_PRES_GHOST,     /* data lost, car frozen, should be drawn as ghost */
    CN_PRES_REMOVED    /* gone for good */
} cn_presence;

typedef struct {
    uint32_t t;
    cn_car_state s;
} cn_sample;

typedef struct {
    cn_sample buf[CN_INTERP_CAP];
    int count; /* ascending by t */
    int has_out;
    cn_car_state out;
    cn_presence last_mode;
    uint32_t last_now;
    float off[3];
    /* render delay: adapts to the worst recent transit time (sample age on arrival) */
    float delay;
    uint32_t fixed_delay_ms; /* nonzero: use this and do not adapt (tests) */
    uint16_t transit[CN_TRANSIT_WINDOW];
    int ntransit, transit_pos;
} cn_interp;

void cn_interp_init(cn_interp *ip);
/* Add a sample stamped t (host ms). Duplicates and very old samples are ignored. */
void cn_interp_push(cn_interp *ip, uint32_t t, const cn_car_state *s, uint32_t arrival_host_ms);
/* Current render delay in ms. */
uint32_t cn_interp_delay(const cn_interp *ip);
/* Get the car for host time now. out is only written when the result is not EMPTY/REMOVED. */
cn_presence cn_interp_sample(cn_interp *ip, uint32_t now_host_ms, cn_car_state *out);

#endif
