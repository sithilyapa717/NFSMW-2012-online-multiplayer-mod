#include "cn_session.h"

#include "cn_buf.h"

#define MAGIC 0x4E43u
#define RTO_MS 100u
#define WINDOW 16
#define HB_MS 500u

enum {
    M_HELLO = 1,
    M_WELCOME,
    M_REJECT,
    M_PING,
    M_PONG,
    M_HEARTBEAT,
    M_CAR_STATE = 0x10,
    M_PLAYER_LIST = 0x20,
    M_CAR_CHOICE,
    M_READY,
    M_START_EVENT,
    M_LOADED,
    M_COUNTDOWN,
    M_FINISH,
    M_RESULTS,
    M_BACK_TO_LOBBY,
    M_LEAVE
};

/* ------------------------------------------------------------------ utils */

static uint32_t rnd(cn_session *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x ? x : 0x9E3779B9u;
    return s->rng;
}

static void push_ev(cn_session *s, cn_event_type t, uint8_t player, uint32_t value)
{
    if (s->ev_count == (int)(sizeof(s->ev) / sizeof(s->ev[0]))) {
        s->ev_head = (s->ev_head + 1) % (int)(sizeof(s->ev) / sizeof(s->ev[0]));
        s->ev_count--; /* drop the oldest */
    }
    int cap = (int)(sizeof(s->ev) / sizeof(s->ev[0]));
    cn_event *e = &s->ev[(s->ev_head + s->ev_count) % cap];
    e->type = (uint8_t)t;
    e->player = player;
    e->value = value;
    s->ev_count++;
}

int cn_poll_event(cn_session *s, cn_event *out)
{
    if (s->ev_count == 0)
        return 0;
    *out = s->ev[s->ev_head];
    s->ev_head = (s->ev_head + 1) % (int)(sizeof(s->ev) / sizeof(s->ev[0]));
    s->ev_count--;
    return 1;
}

static void clean_name(char *dst, const char *src)
{
    int n = 0;
    if (src) {
        for (; src[n] && n < CN_NAME_LEN - 1; n++) {
            unsigned char c = (unsigned char)src[n];
            dst[n] = (c >= 0x21 && c <= 0x7E) ? (char)c : '_';
        }
    }
    if (n == 0) {
        memcpy(dst, "Player", 6);
        n = 6;
    }
    memset(dst + n, 0, (size_t)(CN_NAME_LEN - n));
}

uint32_t cn_host_time(const cn_session *s, uint32_t now)
{
    return s->role == CN_ROLE_HOST ? now : (uint32_t)(now + (uint32_t)s->clock_offset);
}

static int seq_lt(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) < 0; }

/* ------------------------------------------------------------ packet layer */

static void tx(cn_session *s, const cn_addr *to, const uint8_t *d, size_t n)
{
    s->cfg.tr.send(s->cfg.tr.user, to, d, (int)n);
    s->stat_tx_packets++;
    s->stat_tx_bytes += (uint32_t)n;
}

static void pk_begin(cn_w *w, uint8_t *buf, int has_ack, uint16_t ack)
{
    cn_w_init(w, buf, CN_MAX_PACKET);
    cn_w_u16(w, MAGIC);
    cn_w_u8(w, has_ack ? 1 : 0);
    cn_w_u16(w, ack);
}

static void pk_unrel(cn_w *w, uint8_t type, const uint8_t *d, size_t n)
{
    cn_w_u8(w, 0);
    cn_w_u16(w, (uint16_t)n);
    cn_w_u8(w, type);
    cn_w_bytes(w, d, n);
}

static void pk_rel(cn_w *w, const cn_rel_msg *m)
{
    cn_w_u8(w, 1);
    cn_w_u16(w, m->seq);
    cn_w_u16(w, m->len);
    cn_w_u8(w, m->type);
    cn_w_bytes(w, m->data, m->len);
}

/* A datagram that is not tied to a connection (handshake replies). */
static void send_raw_unrel(cn_session *s, const cn_addr *to, uint8_t type, const uint8_t *d, size_t n)
{
    uint8_t buf[CN_MAX_PACKET];
    cn_w w;
    pk_begin(&w, buf, 0, 0);
    pk_unrel(&w, type, d, n);
    if (!w.err)
        tx(s, to, buf, w.len);
}

static void send_unrel(cn_session *s, cn_peer *p, uint8_t type, const uint8_t *d, size_t n)
{
    if (!p->active)
        return;
    uint8_t buf[CN_MAX_PACKET];
    cn_w w;
    pk_begin(&w, buf, 1, p->recv_next);
    pk_unrel(&w, type, d, n);
    if (!w.err) {
        tx(s, &p->addr, buf, w.len);
        p->need_ack = 0;
    }
}

static int peer_enqueue(cn_peer *p, uint8_t type, const uint8_t *d, size_t n)
{
    if (!p->active || n > CN_MAX_MSG || p->qcount >= CN_REL_QUEUE)
        return -1;
    cn_rel_msg *m = &p->q[(p->qhead + p->qcount) % CN_REL_QUEUE];
    m->seq = p->send_next++;
    m->len = (uint16_t)n;
    m->type = type;
    m->sent = 0;
    m->last_sent = 0;
    if (n)
        memcpy(m->data, d, n);
    p->qcount++;
    return 0;
}

static void peer_flush(cn_session *s, cn_peer *p, uint32_t now)
{
    if (!p->active)
        return;
    uint8_t buf[CN_MAX_PACKET];
    cn_w w;
    pk_begin(&w, buf, 1, p->recv_next);
    int added = 0;
    int lim = p->qcount < WINDOW ? p->qcount : WINDOW;
    for (int i = 0; i < lim; i++) {
        cn_rel_msg *m = &p->q[(p->qhead + i) % CN_REL_QUEUE];
        if (m->sent && (uint32_t)(now - m->last_sent) < RTO_MS)
            continue;
        if (w.len + 6u + m->len > CN_MAX_PACKET)
            break;
        pk_rel(&w, m);
        m->last_sent = now;
        m->sent++;
        added++;
    }
    if (added || p->need_ack) {
        if (!w.err)
            tx(s, &p->addr, buf, w.len);
        p->need_ack = 0;
    }
}

static void peer_ack(cn_peer *p, uint16_t ack)
{
    while (p->qcount > 0 && seq_lt(p->q[p->qhead].seq, ack)) {
        p->qhead = (p->qhead + 1) % CN_REL_QUEUE;
        p->qcount--;
    }
}

static void peer_reset(cn_peer *p, const cn_addr *addr, uint32_t nonce, uint32_t now)
{
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->addr = *addr;
    p->nonce = nonce;
    p->last_heard = now;
    p->last_hb = now;
}

/* ------------------------------------------------------------- host side */

static void host_lose_peer(cn_session *s, int id, int left)
{
    cn_peer *p = &s->peers[id];
    p->active = 0;
    if (s->phase == CN_PH_LOBBY) {
        memset(&s->players[id], 0, sizeof(s->players[id]));
        s->players[id].state = CN_PS_EMPTY;
    } else {
        s->players[id].state = CN_PS_DISCONNECTED;
    }
    cn_interp_init(&s->interp[id]);
    s->list_dirty = 1;
    push_ev(s, left ? CN_EV_PLAYER_LEFT : CN_EV_PLAYER_LOST, (uint8_t)id, 0);
}

static void host_rel(cn_session *s, int id, uint8_t type, const uint8_t *d, size_t n)
{
    if (!s->peers[id].active)
        return;
    if (peer_enqueue(&s->peers[id], type, d, n) < 0)
        host_lose_peer(s, id, 0); /* a peer that cannot keep up is dropped */
    else
        s->stat_rel_queued++;
}

static void host_bcast(cn_session *s, uint8_t type, const uint8_t *d, size_t n)
{
    for (int id = 1; id < CN_MAX_PLAYERS; id++)
        host_rel(s, id, type, d, n);
}

static size_t encode_list(const cn_session *s, uint8_t *out)
{
    cn_w w;
    cn_w_init(&w, out, CN_MAX_MSG);
    cn_w_u8(&w, (uint8_t)s->phase);
    cn_w_u32(&w, s->event_id);
    cn_w_u32(&w, s->race_start);
    cn_w_u8(&w, CN_MAX_PLAYERS);
    for (int i = 0; i < CN_MAX_PLAYERS; i++) {
        const cn_player *p = &s->players[i];
        cn_w_u8(&w, (uint8_t)p->state);
        cn_w_bytes(&w, p->name, CN_NAME_LEN);
        cn_w_u8(&w, p->ready);
        cn_w_u8(&w, p->loaded);
        cn_w_u8(&w, p->finished);
        cn_w_bytes(&w, p->car, CN_CAR_BLOB);
        cn_w_u32(&w, p->finish_ms);
        cn_w_u16(&w, p->ping_ms);
    }
    return w.err ? 0 : w.len;
}

static size_t encode_results(const cn_session *s, uint8_t *out)
{
    cn_w w;
    cn_w_init(&w, out, CN_MAX_MSG);
    cn_w_u8(&w, (uint8_t)s->n_results);
    for (int i = 0; i < s->n_results; i++) {
        cn_w_u8(&w, s->results[i].id);
        cn_w_u8(&w, s->results[i].finished);
        cn_w_u8(&w, s->results[i].rank);
        cn_w_u32(&w, s->results[i].finish_ms);
    }
    return w.err ? 0 : w.len;
}

/* Everything a (re)joining guest needs to catch up. */
static void host_send_state_to(cn_session *s, int id)
{
    uint8_t m[CN_MAX_MSG];
    size_t n = encode_list(s, m);
    host_rel(s, id, M_PLAYER_LIST, m, n);
    uint8_t b[4];
    if (s->phase >= CN_PH_LOADING) {
        cn_w w;
        cn_w_init(&w, b, 4);
        cn_w_u32(&w, s->event_id);
        host_rel(s, id, M_START_EVENT, b, 4);
    }
    if (s->phase >= CN_PH_COUNTDOWN && s->phase != CN_PH_RESULTS) {
        cn_w w;
        cn_w_init(&w, b, 4);
        cn_w_u32(&w, s->race_start);
        host_rel(s, id, M_COUNTDOWN, b, 4);
    }
    if (s->phase == CN_PH_RESULTS) {
        n = encode_results(s, m);
        host_rel(s, id, M_RESULTS, m, n);
    }
}

static void send_welcome(cn_session *s, const cn_addr *to, uint8_t id, uint32_t now)
{
    uint8_t m[6];
    cn_w w;
    cn_w_init(&w, m, sizeof(m));
    cn_w_u8(&w, id);
    cn_w_u8(&w, CN_PROTOCOL_VERSION);
    cn_w_u32(&w, now);
    send_raw_unrel(s, to, M_WELCOME, m, w.len);
}

static void send_reject(cn_session *s, const cn_addr *to, uint8_t reason)
{
    send_raw_unrel(s, to, M_REJECT, &reason, 1);
}

static void host_on_hello(cn_session *s, const cn_addr *from, const uint8_t *d, size_t n, uint32_t now)
{
    cn_r r;
    cn_r_init(&r, d, n);
    uint8_t ver = cn_r_u8(&r);
    uint32_t nonce = cn_r_u32(&r);
    uint32_t build = cn_r_u32(&r);
    char raw[CN_NAME_LEN];
    cn_r_bytes(&r, raw, CN_NAME_LEN);
    if (r.err) {
        s->stat_bad_packets++;
        return;
    }
    raw[CN_NAME_LEN - 1] = 0;
    char name[CN_NAME_LEN];
    clean_name(name, raw);

    if (ver != CN_PROTOCOL_VERSION) {
        send_reject(s, from, CN_REJ_VERSION);
        return;
    }
    if (build != s->cfg.build_hash) {
        send_reject(s, from, CN_REJ_BUILD);
        return;
    }

    /* Same address: duplicate HELLO or a restarted client. */
    for (int id = 1; id < CN_MAX_PLAYERS; id++) {
        cn_peer *p = &s->peers[id];
        if (p->active && cn_addr_eq(&p->addr, from)) {
            if (p->nonce == nonce) {
                send_welcome(s, from, (uint8_t)id, now);
                return;
            }
            peer_reset(p, from, nonce, now);
            memcpy(s->players[id].name, name, CN_NAME_LEN);
            s->players[id].state = CN_PS_CONNECTED;
            send_welcome(s, from, (uint8_t)id, now);
            host_send_state_to(s, id);
            s->list_dirty = 1;
            push_ev(s, CN_EV_PLAYER_BACK, (uint8_t)id, 0);
            return;
        }
    }
    /* A lost player coming back by name. */
    for (int id = 1; id < CN_MAX_PLAYERS; id++) {
        if (s->players[id].state == CN_PS_DISCONNECTED && strncmp(s->players[id].name, name, CN_NAME_LEN) == 0) {
            peer_reset(&s->peers[id], from, nonce, now);
            s->players[id].state = CN_PS_CONNECTED;
            send_welcome(s, from, (uint8_t)id, now);
            host_send_state_to(s, id);
            s->list_dirty = 1;
            push_ev(s, CN_EV_PLAYER_BACK, (uint8_t)id, 0);
            return;
        }
    }
    for (int id = 0; id < CN_MAX_PLAYERS; id++) {
        if (s->players[id].state == CN_PS_CONNECTED && strncmp(s->players[id].name, name, CN_NAME_LEN) == 0) {
            send_reject(s, from, CN_REJ_NAME_TAKEN);
            return;
        }
    }
    if (s->phase != CN_PH_LOBBY) {
        send_reject(s, from, CN_REJ_IN_PROGRESS);
        return;
    }
    for (int id = 1; id < CN_MAX_PLAYERS; id++) {
        if (s->players[id].state == CN_PS_EMPTY) {
            peer_reset(&s->peers[id], from, nonce, now);
            memset(&s->players[id], 0, sizeof(s->players[id]));
            memcpy(s->players[id].name, name, CN_NAME_LEN);
            s->players[id].state = CN_PS_CONNECTED;
            send_welcome(s, from, (uint8_t)id, now);
            host_send_state_to(s, id);
            s->list_dirty = 1;
            push_ev(s, CN_EV_PLAYER_JOINED, (uint8_t)id, 0);
            return;
        }
    }
    send_reject(s, from, CN_REJ_FULL);
}

static void read_car(cn_r *r, cn_car_state *c)
{
    for (int i = 0; i < 3; i++)
        c->pos[i] = cn_r_f32(r);
    for (int i = 0; i < 4; i++)
        c->rot[i] = cn_r_f32(r);
    for (int i = 0; i < 3; i++)
        c->vel[i] = cn_r_f32(r);
    c->steer = cn_r_f32(r);
    c->flags = cn_r_u16(r);
}

static void write_car(cn_w *w, const cn_car_state *c)
{
    for (int i = 0; i < 3; i++)
        cn_w_f32(w, c->pos[i]);
    for (int i = 0; i < 4; i++)
        cn_w_f32(w, c->rot[i]);
    for (int i = 0; i < 3; i++)
        cn_w_f32(w, c->vel[i]);
    cn_w_f32(w, c->steer);
    cn_w_u16(w, c->flags);
}

static void host_dispatch(cn_session *s, cn_peer *p, int id, const cn_addr *from, uint8_t type,
                          const uint8_t *d, size_t n, uint32_t now)
{
    if (type == M_HELLO) {
        host_on_hello(s, from, d, n, now);
        return;
    }
    if (!p)
        return;
    cn_r r;
    cn_r_init(&r, d, n);
    cn_player *pl = &s->players[id];
    switch (type) {
    case M_PING: {
        uint32_t t0 = cn_r_u32(&r);
        uint16_t rtt = cn_r_u16(&r);
        if (r.err)
            break;
        pl->ping_ms = rtt;
        uint8_t m[8];
        cn_w w;
        cn_w_init(&w, m, sizeof(m));
        cn_w_u32(&w, t0);
        cn_w_u32(&w, now);
        send_unrel(s, p, M_PONG, m, w.len);
        break;
    }
    case M_HEARTBEAT:
        break;
    case M_CAR_STATE: {
        (void)cn_r_u8(&r); /* claimed id is ignored: identity comes from the address */
        uint32_t t = cn_r_u32(&r);
        cn_car_state c;
        read_car(&r, &c);
        if (r.err || pl->state != CN_PS_CONNECTED)
            break;
        cn_interp_push(&s->interp[id], t, &c, now);
        uint8_t m[64];
        cn_w w;
        cn_w_init(&w, m, sizeof(m));
        cn_w_u8(&w, (uint8_t)id);
        cn_w_u32(&w, t);
        write_car(&w, &c);
        for (int j = 1; j < CN_MAX_PLAYERS; j++)
            if (j != id)
                send_unrel(s, &s->peers[j], M_CAR_STATE, m, w.len);
        break;
    }
    case M_CAR_CHOICE:
        if (s->phase == CN_PH_LOBBY && n >= CN_CAR_BLOB) {
            memcpy(pl->car, d, CN_CAR_BLOB);
            s->list_dirty = 1;
        }
        break;
    case M_READY:
        if (s->phase == CN_PH_LOBBY && n >= 1) {
            pl->ready = d[0] ? 1 : 0;
            s->list_dirty = 1;
        }
        break;
    case M_LOADED:
        if (s->phase == CN_PH_LOADING && !pl->loaded) {
            pl->loaded = 1;
            s->list_dirty = 1;
        }
        break;
    case M_FINISH: {
        uint32_t ms = cn_r_u32(&r);
        if (r.err || s->phase != CN_PH_RACING || pl->finished)
            break;
        pl->finished = 1;
        pl->finish_ms = ms;
        if (!s->first_finish_at)
            s->first_finish_at = now ? now : 1;
        s->list_dirty = 1;
        push_ev(s, CN_EV_PLAYER_FINISHED, (uint8_t)id, ms);
        break;
    }
    case M_LEAVE:
        host_lose_peer(s, id, 1);
        break;
    default:
        break;
    }
}

int cn_rank_results(const cn_player *players, cn_result *out)
{
    int idx[CN_MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < CN_MAX_PLAYERS; i++)
        if (players[i].state != CN_PS_EMPTY)
            idx[n++] = i;
    /* finishers first by time then id; DNFs after by id (insertion sort) */
    for (int i = 1; i < n; i++) {
        int v = idx[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            const cn_player *a = &players[idx[j]];
            const cn_player *b = &players[v];
            int a_before_b;
            if (a->finished != b->finished)
                a_before_b = a->finished > b->finished;
            else if (a->finished && a->finish_ms != b->finish_ms)
                a_before_b = a->finish_ms < b->finish_ms;
            else
                a_before_b = idx[j] < v;
            if (a_before_b)
                break;
            idx[j + 1] = idx[j];
        }
        idx[j + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        out[i].id = (uint8_t)idx[i];
        out[i].finished = players[idx[i]].finished ? 1 : 0;
        out[i].finish_ms = players[idx[i]].finished ? players[idx[i]].finish_ms : 0;
        out[i].rank = (uint8_t)(i + 1);
    }
    return n;
}

static void host_end_race(cn_session *s, uint32_t now)
{
    s->n_results = cn_rank_results(s->players, s->results);
    s->phase = CN_PH_RESULTS;
    s->phase_since = now;
    uint8_t m[CN_MAX_MSG];
    size_t n = encode_results(s, m);
    host_bcast(s, M_RESULTS, m, n);
    s->list_dirty = 1;
    push_ev(s, CN_EV_RESULTS, 0, 0);
}

static void host_tick(cn_session *s, uint32_t now)
{
    uint32_t timeout = s->cfg.peer_timeout_ms ? s->cfg.peer_timeout_ms : 5000;
    for (int id = 1; id < CN_MAX_PLAYERS; id++) {
        cn_peer *p = &s->peers[id];
        if (p->active && (uint32_t)(now - p->last_heard) > timeout)
            host_lose_peer(s, id, 0);
    }

    switch (s->phase) {
    case CN_PH_LOADING: {
        uint32_t lt = s->cfg.load_timeout_ms ? s->cfg.load_timeout_ms : 60000;
        if ((uint32_t)(now - s->phase_since) > lt) {
            for (int id = 1; id < CN_MAX_PLAYERS; id++)
                if (s->players[id].state == CN_PS_CONNECTED && !s->players[id].loaded)
                    host_lose_peer(s, id, 0);
        }
        int all = 1;
        for (int id = 0; id < CN_MAX_PLAYERS; id++)
            if (s->players[id].state == CN_PS_CONNECTED && !s->players[id].loaded)
                all = 0;
        if (all) {
            uint32_t cd = s->cfg.countdown_ms ? s->cfg.countdown_ms : 3000;
            s->race_start = now + cd;
            s->phase = CN_PH_COUNTDOWN;
            s->phase_since = now;
            uint8_t b[4];
            cn_w w;
            cn_w_init(&w, b, 4);
            cn_w_u32(&w, s->race_start);
            host_bcast(s, M_COUNTDOWN, b, 4);
            s->list_dirty = 1;
            push_ev(s, CN_EV_COUNTDOWN, 0, s->race_start);
        }
        break;
    }
    case CN_PH_COUNTDOWN:
        if ((int32_t)(now - s->race_start) >= 0) {
            s->phase = CN_PH_RACING;
            s->phase_since = now;
            s->first_finish_at = 0;
            s->list_dirty = 1;
            push_ev(s, CN_EV_RACE_GO, 0, 0);
        }
        break;
    case CN_PH_RACING: {
        int unfinished = 0;
        for (int id = 0; id < CN_MAX_PLAYERS; id++)
            if (s->players[id].state == CN_PS_CONNECTED && !s->players[id].finished)
                unfinished++;
        uint32_t grace = s->cfg.finish_grace_ms ? s->cfg.finish_grace_ms : 30000;
        uint32_t rt = s->cfg.race_timeout_ms ? s->cfg.race_timeout_ms : 600000;
        int end = unfinished == 0;
        if (s->first_finish_at && (uint32_t)(now - s->first_finish_at) > grace)
            end = 1;
        if ((uint32_t)(now - s->phase_since) > rt)
            end = 1;
        if (end)
            host_end_race(s, now);
        break;
    }
    default:
        break;
    }

    if (s->list_dirty) {
        s->list_dirty = 0;
        uint8_t m[CN_MAX_MSG];
        size_t n = encode_list(s, m);
        host_bcast(s, M_PLAYER_LIST, m, n);
        push_ev(s, CN_EV_LIST_CHANGED, 0, 0);
    }

    for (int id = 1; id < CN_MAX_PLAYERS; id++) {
        cn_peer *p = &s->peers[id];
        if (!p->active)
            continue;
        if ((uint32_t)(now - p->last_hb) >= HB_MS) {
            p->last_hb = now;
            send_unrel(s, p, M_HEARTBEAT, NULL, 0);
        }
        peer_flush(s, p, now);
    }
}

/* ------------------------------------------------------------ guest side */

static void guest_add_clock_sample(cn_session *s, uint32_t rtt, int32_t off)
{
    s->samples[s->clock_samples % 8].rtt = rtt;
    s->samples[s->clock_samples % 8].off = off;
    s->clock_samples++;
    int n = s->clock_samples < 8 ? s->clock_samples : 8;
    cn_clk_sample tmp[8];
    memcpy(tmp, s->samples, sizeof(tmp));
    for (int i = 1; i < n; i++) { /* sort by rtt */
        cn_clk_sample v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].rtt > v.rtt) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    int k = n < 3 ? n : 3;
    int32_t offs[3];
    for (int i = 0; i < k; i++)
        offs[i] = tmp[i].off;
    for (int i = 1; i < k; i++) { /* sort offsets, take the median */
        int32_t v = offs[i];
        int j = i - 1;
        while (j >= 0 && offs[j] > v) {
            offs[j + 1] = offs[j];
            j--;
        }
        offs[j + 1] = v;
    }
    s->clock_offset = offs[k / 2];
    s->clock_synced = s->clock_samples >= 3;
}

static void guest_dispatch(cn_session *s, cn_peer *p, const cn_addr *from, uint8_t type,
                           const uint8_t *d, size_t n, uint32_t now)
{
    cn_r r;
    cn_r_init(&r, d, n);
    (void)from;
    if (type == M_WELCOME) {
        uint8_t id = cn_r_u8(&r);
        uint8_t ver = cn_r_u8(&r);
        uint32_t ht = cn_r_u32(&r);
        if (r.err || s->link != CN_LINK_CONNECTING || ver != CN_PROTOCOL_VERSION || id == 0 || id >= CN_MAX_PLAYERS)
            return;
        peer_reset(&s->peers[0], &s->host_addr, 0, now);
        s->local_id = id;
        s->link = CN_LINK_UP;
        s->clock_offset = (int32_t)(ht - now);
        s->pings_sent = 0;
        s->last_ping = now - 1000;
        push_ev(s, CN_EV_CONNECTED, id, 0);
        return;
    }
    if (type == M_REJECT) {
        uint8_t why = cn_r_u8(&r);
        if (s->link == CN_LINK_CONNECTING) {
            s->link = CN_LINK_FAILED;
            push_ev(s, CN_EV_REJECTED, 0, why);
        }
        return;
    }
    if (!p || s->link != CN_LINK_UP)
        return;

    switch (type) {
    case M_PONG: {
        uint32_t t0 = cn_r_u32(&r);
        uint32_t ht = cn_r_u32(&r);
        if (r.err)
            break;
        uint32_t rtt = now - t0;
        if (rtt > 10000)
            break;
        s->last_rtt = (uint16_t)rtt;
        guest_add_clock_sample(s, rtt, (int32_t)(ht + rtt / 2 - now));
        break;
    }
    case M_HEARTBEAT:
        break;
    case M_CAR_STATE: {
        uint8_t id = cn_r_u8(&r);
        uint32_t t = cn_r_u32(&r);
        cn_car_state c;
        read_car(&r, &c);
        if (r.err || id >= CN_MAX_PLAYERS || id == s->local_id)
            break;
        cn_interp_push(&s->interp[id], t, &c, cn_host_time(s, now));
        break;
    }
    case M_PLAYER_LIST: {
        uint8_t ph = cn_r_u8(&r);
        uint32_t ev = cn_r_u32(&r);
        uint32_t rs = cn_r_u32(&r);
        uint8_t cnt = cn_r_u8(&r);
        cn_player tmp[CN_MAX_PLAYERS];
        memset(tmp, 0, sizeof(tmp));
        if (cnt != CN_MAX_PLAYERS)
            break;
        for (int i = 0; i < CN_MAX_PLAYERS; i++) {
            tmp[i].state = (cn_pstate)cn_r_u8(&r);
            cn_r_bytes(&r, tmp[i].name, CN_NAME_LEN);
            tmp[i].name[CN_NAME_LEN - 1] = 0;
            tmp[i].ready = cn_r_u8(&r);
            tmp[i].loaded = cn_r_u8(&r);
            tmp[i].finished = cn_r_u8(&r);
            cn_r_bytes(&r, tmp[i].car, CN_CAR_BLOB);
            tmp[i].finish_ms = cn_r_u32(&r);
            tmp[i].ping_ms = cn_r_u16(&r);
        }
        if (r.err || ph > CN_PH_RESULTS)
            break;
        memcpy(s->players, tmp, sizeof(tmp));
        if (!s->list_applied) {
            s->list_applied = 1;
            s->phase = (cn_phase)ph;
            s->event_id = ev;
            s->race_start = rs;
        }
        push_ev(s, CN_EV_LIST_CHANGED, 0, 0);
        break;
    }
    case M_START_EVENT: {
        uint32_t ev = cn_r_u32(&r);
        if (r.err)
            break;
        s->event_id = ev;
        s->phase = CN_PH_LOADING;
        s->n_results = 0;
        push_ev(s, CN_EV_START_EVENT, 0, ev);
        break;
    }
    case M_COUNTDOWN: {
        uint32_t st = cn_r_u32(&r);
        if (r.err)
            break;
        s->race_start = st;
        s->phase = CN_PH_COUNTDOWN;
        push_ev(s, CN_EV_COUNTDOWN, 0, st);
        break;
    }
    case M_RESULTS: {
        uint8_t cnt = cn_r_u8(&r);
        if (r.err || cnt > CN_MAX_PLAYERS)
            break;
        cn_result res[CN_MAX_PLAYERS];
        for (int i = 0; i < cnt; i++) {
            res[i].id = cn_r_u8(&r);
            res[i].finished = cn_r_u8(&r);
            res[i].rank = cn_r_u8(&r);
            res[i].finish_ms = cn_r_u32(&r);
        }
        if (r.err)
            break;
        memcpy(s->results, res, sizeof(cn_result) * cnt);
        s->n_results = cnt;
        s->phase = CN_PH_RESULTS;
        push_ev(s, CN_EV_RESULTS, 0, 0);
        break;
    }
    case M_BACK_TO_LOBBY:
        s->phase = CN_PH_LOBBY;
        s->n_results = 0;
        push_ev(s, CN_EV_BACK_TO_LOBBY, 0, 0);
        break;
    case M_LEAVE:
        s->link = CN_LINK_FAILED;
        p->active = 0;
        push_ev(s, CN_EV_HOST_LOST, 0, 0);
        break;
    default:
        break;
    }
}

static void guest_tick(cn_session *s, uint32_t now)
{
    cn_peer *p = &s->peers[0];
    if (s->link == CN_LINK_CONNECTING) {
        if ((uint32_t)(now - s->hello_started) > 5000) {
            s->link = CN_LINK_FAILED;
            push_ev(s, CN_EV_CONNECT_FAILED, 0, 0);
            return;
        }
        if ((uint32_t)(now - s->last_hello) >= 250) {
            s->last_hello = now;
            uint8_t m[1 + 4 + 4 + CN_NAME_LEN];
            cn_w w;
            cn_w_init(&w, m, sizeof(m));
            cn_w_u8(&w, CN_PROTOCOL_VERSION);
            cn_w_u32(&w, s->nonce);
            cn_w_u32(&w, s->cfg.build_hash);
            cn_w_bytes(&w, s->name, CN_NAME_LEN);
            send_raw_unrel(s, &s->host_addr, M_HELLO, m, w.len);
        }
        return;
    }
    if (s->link != CN_LINK_UP)
        return;

    uint32_t timeout = s->cfg.peer_timeout_ms ? s->cfg.peer_timeout_ms : 5000;
    if ((uint32_t)(now - p->last_heard) > timeout) {
        s->link = CN_LINK_FAILED;
        p->active = 0;
        push_ev(s, CN_EV_HOST_LOST, 0, 0);
        return;
    }

    uint32_t interval = s->pings_sent < 10 ? 100 : 1000;
    if ((uint32_t)(now - s->last_ping) >= interval) {
        s->last_ping = now;
        s->pings_sent++;
        uint8_t m[6];
        cn_w w;
        cn_w_init(&w, m, sizeof(m));
        cn_w_u32(&w, now);
        cn_w_u16(&w, s->last_rtt);
        send_unrel(s, p, M_PING, m, w.len);
    }

    if (s->phase == CN_PH_COUNTDOWN && (int32_t)(cn_host_time(s, now) - s->race_start) >= 0) {
        s->phase = CN_PH_RACING;
        push_ev(s, CN_EV_RACE_GO, 0, 0);
    }
    peer_flush(s, p, now);
}

/* ------------------------------------------------------------ entry points */

static void common_init(cn_session *s, const cn_config *cfg, cn_role role)
{
    memset(s, 0, sizeof(*s));
    s->role = role;
    s->cfg = *cfg;
    clean_name(s->name, cfg->name);
    s->rng = cfg->seed ^ 0xA5A5A5A5u;
    if (!s->rng)
        s->rng = 0x9E3779B9u;
    for (int i = 0; i < CN_MAX_PLAYERS; i++)
        cn_interp_init(&s->interp[i]);
}

void cn_host_init(cn_session *s, const cn_config *cfg, uint32_t now)
{
    common_init(s, cfg, CN_ROLE_HOST);
    s->link = CN_LINK_UP;
    s->local_id = 0;
    s->phase = CN_PH_LOBBY;
    s->phase_since = now;
    s->players[0].state = CN_PS_CONNECTED;
    memcpy(s->players[0].name, s->name, CN_NAME_LEN);
    s->list_dirty = 1;
}

void cn_guest_init(cn_session *s, const cn_config *cfg, cn_addr host, uint32_t now)
{
    common_init(s, cfg, CN_ROLE_GUEST);
    s->host_addr = host;
    s->link = CN_LINK_CONNECTING;
    s->hello_started = now;
    s->last_hello = now - 1000;
    s->nonce = rnd(s) | 1u;
}

static void on_datagram(cn_session *s, const cn_addr *from, const uint8_t *buf, int len, uint32_t now)
{
    cn_r r;
    cn_r_init(&r, buf, (size_t)len);
    uint16_t magic = cn_r_u16(&r);
    uint8_t flags = cn_r_u8(&r);
    uint16_t ack = cn_r_u16(&r);
    if (r.err || magic != MAGIC) {
        s->stat_bad_packets++;
        return;
    }
    s->stat_rx_packets++;

    int id = -1;
    cn_peer *p = NULL;
    if (s->role == CN_ROLE_GUEST) {
        if (!cn_addr_eq(from, &s->host_addr))
            return;
        if (s->peers[0].active) {
            p = &s->peers[0];
            id = 0;
        }
    } else {
        for (int i = 1; i < CN_MAX_PLAYERS; i++) {
            if (s->peers[i].active && cn_addr_eq(&s->peers[i].addr, from)) {
                p = &s->peers[i];
                id = i;
                break;
            }
        }
    }
    if (p) {
        p->last_heard = now;
        if (flags & 1)
            peer_ack(p, ack);
    }

    while (r.pos < r.len) {
        uint8_t kind = cn_r_u8(&r);
        uint16_t seq = 0;
        if (kind == 1)
            seq = cn_r_u16(&r);
        else if (kind != 0) {
            s->stat_bad_packets++;
            return;
        }
        uint16_t n = cn_r_u16(&r);
        uint8_t type = cn_r_u8(&r);
        if (r.err || n > CN_MAX_MSG || r.pos + n > r.len) {
            s->stat_bad_packets++;
            return;
        }
        const uint8_t *d = r.p + r.pos;
        r.pos += n;

        if (kind == 1) {
            if (!p)
                continue;
            p->need_ack = 1;
            if (seq != p->recv_next)
                continue; /* old duplicate or a gap: the sender will resend in order */
            p->recv_next++;
            s->stat_rel_delivered++;
        }
        if (s->role == CN_ROLE_HOST)
            host_dispatch(s, p, id, from, type, d, n, now);
        else
            guest_dispatch(s, p, from, type, d, n, now);
        if (p && !p->active)
            p = NULL;
        if (s->role == CN_ROLE_GUEST && !p && s->peers[0].active) {
            p = &s->peers[0]; /* WELCOME just created it */
            id = 0;
            p->last_heard = now;
        }
    }
}

void cn_update(cn_session *s, uint32_t now)
{
    uint8_t buf[2048];
    cn_addr from;
    for (int i = 0; i < 256; i++) {
        int n = s->cfg.tr.recv(s->cfg.tr.user, &from, buf, (int)sizeof(buf));
        if (n <= 0)
            break;
        on_datagram(s, &from, buf, n, now);
    }
    if (s->role == CN_ROLE_HOST)
        host_tick(s, now);
    else
        guest_tick(s, now);
}

/* ------------------------------------------------------------ local actions */

static void guest_rel(cn_session *s, uint8_t type, const uint8_t *d, size_t n)
{
    if (s->link != CN_LINK_UP)
        return;
    if (peer_enqueue(&s->peers[0], type, d, n) == 0) {
        s->stat_rel_queued++;
    } else {
        /* more than CN_REL_QUEUE unacknowledged messages: the link is broken */
        s->link = CN_LINK_FAILED;
        s->peers[0].active = 0;
        push_ev(s, CN_EV_HOST_LOST, 0, 0);
    }
}

void cn_set_car(cn_session *s, const uint8_t blob[CN_CAR_BLOB])
{
    if (s->phase != CN_PH_LOBBY)
        return;
    memcpy(s->players[s->local_id].car, blob, CN_CAR_BLOB);
    if (s->role == CN_ROLE_HOST)
        s->list_dirty = 1;
    else
        guest_rel(s, M_CAR_CHOICE, blob, CN_CAR_BLOB);
}

void cn_set_ready(cn_session *s, int ready)
{
    if (s->phase != CN_PH_LOBBY)
        return;
    uint8_t v = ready ? 1 : 0;
    s->players[s->local_id].ready = v;
    if (s->role == CN_ROLE_HOST)
        s->list_dirty = 1;
    else
        guest_rel(s, M_READY, &v, 1);
}

void cn_report_loaded(cn_session *s, uint32_t now)
{
    (void)now;
    if (s->phase != CN_PH_LOADING)
        return;
    s->players[s->local_id].loaded = 1;
    if (s->role == CN_ROLE_HOST)
        s->list_dirty = 1;
    else
        guest_rel(s, M_LOADED, NULL, 0);
}

void cn_report_finish(cn_session *s, uint32_t finish_ms, uint32_t now)
{
    if (s->phase != CN_PH_RACING || s->players[s->local_id].finished)
        return;
    s->players[s->local_id].finished = 1;
    s->players[s->local_id].finish_ms = finish_ms;
    if (s->role == CN_ROLE_HOST) {
        if (!s->first_finish_at)
            s->first_finish_at = now ? now : 1;
        s->list_dirty = 1;
        push_ev(s, CN_EV_PLAYER_FINISHED, 0, finish_ms);
    } else {
        uint8_t b[4];
        cn_w w;
        cn_w_init(&w, b, 4);
        cn_w_u32(&w, finish_ms);
        guest_rel(s, M_FINISH, b, 4);
    }
}

void cn_leave(cn_session *s, uint32_t now)
{
    if (s->link != CN_LINK_UP) {
        s->link = CN_LINK_LEFT;
        return;
    }
    if (s->role == CN_ROLE_GUEST) {
        cn_peer *p = &s->peers[0];
        guest_rel(s, M_LEAVE, NULL, 0);
        peer_flush(s, p, now);
        for (int i = 0; i < 2; i++) /* extra copies in case the first is lost */
            send_unrel(s, p, M_LEAVE, NULL, 0);
    } else {
        for (int id = 1; id < CN_MAX_PLAYERS; id++) {
            cn_peer *p = &s->peers[id];
            if (!p->active)
                continue;
            host_rel(s, id, M_LEAVE, NULL, 0);
            peer_flush(s, p, now);
            for (int i = 0; i < 2; i++)
                send_unrel(s, p, M_LEAVE, NULL, 0);
        }
    }
    s->link = CN_LINK_LEFT;
}

int cn_host_start_event(cn_session *s, uint32_t event_id, uint32_t now)
{
    if (s->role != CN_ROLE_HOST)
        return CN_ERR_NOT_HOST;
    if (s->phase != CN_PH_LOBBY)
        return CN_ERR_BAD_PHASE;
    for (int id = 0; id < CN_MAX_PLAYERS; id++)
        if (s->players[id].state == CN_PS_CONNECTED && !s->players[id].ready)
            return CN_ERR_NOT_READY;
    for (int id = 0; id < CN_MAX_PLAYERS; id++) {
        s->players[id].loaded = 0;
        s->players[id].finished = 0;
        s->players[id].finish_ms = 0;
    }
    s->event_id = event_id;
    s->phase = CN_PH_LOADING;
    s->phase_since = now;
    s->n_results = 0;
    uint8_t b[4];
    cn_w w;
    cn_w_init(&w, b, 4);
    cn_w_u32(&w, event_id);
    host_bcast(s, M_START_EVENT, b, 4);
    s->list_dirty = 1;
    push_ev(s, CN_EV_START_EVENT, 0, event_id);
    return 0;
}

int cn_host_return_to_lobby(cn_session *s, uint32_t now)
{
    if (s->role != CN_ROLE_HOST)
        return CN_ERR_NOT_HOST;
    if (s->phase != CN_PH_RESULTS)
        return CN_ERR_BAD_PHASE;
    s->phase = CN_PH_LOBBY;
    s->phase_since = now;
    s->n_results = 0;
    for (int id = 0; id < CN_MAX_PLAYERS; id++) {
        if (s->players[id].state == CN_PS_DISCONNECTED)
            memset(&s->players[id], 0, sizeof(s->players[id]));
        s->players[id].ready = 0;
        s->players[id].loaded = 0;
        s->players[id].finished = 0;
        s->players[id].finish_ms = 0;
    }
    host_bcast(s, M_BACK_TO_LOBBY, NULL, 0);
    s->list_dirty = 1;
    push_ev(s, CN_EV_BACK_TO_LOBBY, 0, 0);
    return 0;
}

void cn_send_car_state(cn_session *s, const cn_car_state *st, uint32_t now)
{
    if (s->link != CN_LINK_UP)
        return;
    uint8_t m[64];
    cn_w w;
    cn_w_init(&w, m, sizeof(m));
    cn_w_u8(&w, s->local_id);
    cn_w_u32(&w, cn_host_time(s, now));
    write_car(&w, st);
    if (s->role == CN_ROLE_GUEST) {
        send_unrel(s, &s->peers[0], M_CAR_STATE, m, w.len);
    } else {
        for (int id = 1; id < CN_MAX_PLAYERS; id++)
            send_unrel(s, &s->peers[id], M_CAR_STATE, m, w.len);
    }
}

cn_presence cn_remote_state(cn_session *s, uint8_t player, uint32_t now, cn_car_state *out)
{
    if (player >= CN_MAX_PLAYERS || player == s->local_id)
        return CN_PRES_EMPTY;
    return cn_interp_sample(&s->interp[player], cn_host_time(s, now), out);
}

uint32_t cn_remote_delay(const cn_session *s, uint8_t player)
{
    return player < CN_MAX_PLAYERS ? cn_interp_delay(&s->interp[player]) : 0;
}
