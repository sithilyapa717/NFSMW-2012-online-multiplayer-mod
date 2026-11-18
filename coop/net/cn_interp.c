#include "cn_interp.h"

#include <math.h>

static int32_t tdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void cn_interp_init(cn_interp *ip)
{
    memset(ip, 0, sizeof(*ip));
    ip->delay = CN_INTERP_DELAY_MS;
}

uint32_t cn_interp_delay(const cn_interp *ip)
{
    return ip->fixed_delay_ms ? ip->fixed_delay_ms : (uint32_t)(ip->delay + 0.5f);
}

static float target_delay(const cn_interp *ip)
{
    if (ip->ntransit == 0)
        return CN_INTERP_DELAY_MS;
    uint16_t mx = 0;
    for (int i = 0; i < ip->ntransit; i++)
        if (ip->transit[i] > mx)
            mx = ip->transit[i];
    float d = (float)mx + CN_DELAY_MARGIN_MS;
    if (d < CN_DELAY_MIN_MS)
        d = CN_DELAY_MIN_MS;
    if (d > CN_DELAY_MAX_MS)
        d = CN_DELAY_MAX_MS;
    return d;
}

void cn_interp_push(cn_interp *ip, uint32_t t, const cn_car_state *s, uint32_t arrival)
{
    if (ip->count > 0 && tdiff(t, ip->buf[ip->count - 1].t) > CN_GHOST_MS) {
        /* the stream was dead for a long time: this is a fresh appearance */
        uint32_t fixed = ip->fixed_delay_ms;
        cn_interp_init(ip);
        ip->fixed_delay_ms = fixed;
    }
    int i = ip->count;
    while (i > 0 && tdiff(ip->buf[i - 1].t, t) > 0)
        i--;
    if (i > 0 && ip->buf[i - 1].t == t)
        return; /* duplicate */
    if (ip->count == CN_INTERP_CAP) {
        if (i == 0)
            return; /* older than everything we keep */
        memmove(&ip->buf[0], &ip->buf[1], sizeof(cn_sample) * (CN_INTERP_CAP - 1));
        i--;
        ip->count--;
    }
    {
        int32_t tr = tdiff(arrival, t);
        ip->transit[ip->transit_pos] = (uint16_t)(tr < 0 ? 0 : (tr > 2000 ? 2000 : tr));
        ip->transit_pos = (ip->transit_pos + 1) % CN_TRANSIT_WINDOW;
        if (ip->ntransit < CN_TRANSIT_WINDOW)
            ip->ntransit++;
    }
    memmove(&ip->buf[i + 1], &ip->buf[i], sizeof(cn_sample) * (size_t)(ip->count - i));
    ip->buf[i].t = t;
    ip->buf[i].s = *s;
    ip->count++;
}

static void nlerp_quat(const float *a, const float *b, float k, float *out)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sgn = dot < 0.0f ? -1.0f : 1.0f; /* shortest path */
    float len = 0.0f;
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] * (1.0f - k) + b[i] * sgn * k;
        len += out[i] * out[i];
    }
    len = sqrtf(len);
    if (len < 1e-6f) {
        memcpy(out, a, 4 * sizeof(float));
        return;
    }
    for (int i = 0; i < 4; i++)
        out[i] /= len;
}

cn_presence cn_interp_sample(cn_interp *ip, uint32_t now, cn_car_state *out)
{
    if (ip->count == 0)
        return CN_PRES_EMPTY;

    if (ip->fixed_delay_ms) {
        ip->delay = (float)ip->fixed_delay_ms;
    } else if (!ip->has_out) {
        ip->delay = target_delay(ip);
    } else {
        int32_t dtm = tdiff(now, ip->last_now);
        if (dtm < 0)
            dtm = 0;
        if (dtm > 1000)
            dtm = 1000;
        float step = 0.1f * (float)dtm; /* at most 10% playback-rate change */
        float diff = target_delay(ip) - ip->delay;
        if (diff > step)
            diff = step;
        if (diff < -step)
            diff = -step;
        ip->delay += diff;
    }
    uint32_t rt = now - (uint32_t)(ip->delay + 0.5f);
    const cn_sample *first = &ip->buf[0];
    const cn_sample *newest = &ip->buf[ip->count - 1];
    int32_t age = tdiff(rt, newest->t);

    if (age > CN_GHOST_MS)
        return CN_PRES_REMOVED;

    cn_car_state target;
    cn_presence mode = CN_PRES_ACTIVE;

    if (tdiff(rt, first->t) <= 0) {
        target = first->s; /* stream just started: hold the first sample */
    } else if (age <= 0) {
        int i = 0;
        while (i + 1 < ip->count && tdiff(ip->buf[i + 1].t, rt) <= 0)
            i++;
        const cn_sample *a = &ip->buf[i];
        const cn_sample *b = &ip->buf[i + 1 < ip->count ? i + 1 : i];
        float span = (float)tdiff(b->t, a->t);
        float k = span > 0.0f ? (float)tdiff(rt, a->t) / span : 0.0f;
        for (int j = 0; j < 3; j++) {
            target.pos[j] = a->s.pos[j] + (b->s.pos[j] - a->s.pos[j]) * k;
            target.vel[j] = a->s.vel[j] + (b->s.vel[j] - a->s.vel[j]) * k;
        }
        nlerp_quat(a->s.rot, b->s.rot, k, target.rot);
        target.steer = a->s.steer + (b->s.steer - a->s.steer) * k;
        target.flags = a->s.flags;
    } else {
        int32_t ms = age > CN_EXTRAP_MAX_MS ? CN_EXTRAP_MAX_MS : age;
        float dt = (float)ms / 1000.0f;
        target = newest->s;
        for (int j = 0; j < 3; j++)
            target.pos[j] += newest->s.vel[j] * dt;
        mode = age <= CN_EXTRAP_MAX_MS ? CN_PRES_EXTRAP : CN_PRES_GHOST;
    }

    /* Slide back after a prediction instead of teleporting. */
    if (ip->has_out && mode == CN_PRES_ACTIVE &&
        (ip->last_mode == CN_PRES_EXTRAP || ip->last_mode == CN_PRES_GHOST)) {
        float d2 = 0.0f;
        float off[3];
        for (int j = 0; j < 3; j++) {
            off[j] = ip->out.pos[j] - target.pos[j];
            d2 += off[j] * off[j];
        }
        if (d2 > CN_SNAP_DIST * CN_SNAP_DIST)
            memset(ip->off, 0, sizeof(ip->off));
        else
            memcpy(ip->off, off, sizeof(off));
    }
    if (ip->has_out) {
        int32_t dtm = tdiff(now, ip->last_now);
        if (dtm < 0)
            dtm = 0;
        if (dtm > 1000)
            dtm = 1000;
        float f = expf(-(float)dtm / CN_SMOOTH_MS);
        for (int j = 0; j < 3; j++)
            ip->off[j] *= f;
    }
    for (int j = 0; j < 3; j++)
        target.pos[j] += ip->off[j];

    ip->out = target;
    ip->has_out = 1;
    ip->last_mode = mode;
    ip->last_now = now;
    *out = target;
    return mode;
}
