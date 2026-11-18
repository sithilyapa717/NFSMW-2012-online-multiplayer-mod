/* Automated tests for the co-op net library. No game, no sockets: a virtual
 * network with loss, duplication, latency and jitter, and a virtual clock.
 * Each peer also has its own clock offset to exercise clock sync. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cn_interp.h"
#include "cn_session.h"
#include "cn_transports.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                                   \
    do {                                                                                                   \
        g_checks++;                                                                                        \
        if (!(cond)) {                                                                                     \
            g_fail++;                                                                                      \
            printf("    FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                   \
            printf(__VA_ARGS__);                                                                           \
            printf("\n");                                                                                  \
        }                                                                                                  \
    } while (0)

#define MAXP 6
#define BUILD 0xBEEF0001u

typedef struct {
    vnet net;
    vnet_ep ep[MAXP];
    cn_fault f[MAXP];
    cn_session s[MAXP];
    int exists[MAXP], active[MAXP];
    int32_t skew[MAXP];
    uint32_t t; /* true time */
    cn_event log[MAXP][512];
    int nlog[MAXP];
    char names[MAXP][CN_NAME_LEN];
    float loss, dup;
    uint32_t lat, jit, peer_timeout, countdown;
} world;

static uint32_t lt(world *w, int i) { return (uint32_t)((int64_t)w->t + w->skew[i]); }

static cn_config make_cfg(world *w, int i, uint32_t build, uint32_t seed)
{
    cn_config c;
    memset(&c, 0, sizeof(c));
    cn_fault_init(&w->f[i], vnet_endpoint(&w->ep[i], &w->net, i), seed, w->loss, w->dup, w->lat, w->jit);
    c.tr = cn_fault_transport(&w->f[i]);
    c.build_hash = build;
    c.name = w->names[i];
    c.seed = seed;
    c.peer_timeout_ms = w->peer_timeout;
    c.countdown_ms = w->countdown;
    return c;
}

static void world_init(world *w, float loss, float dup, uint32_t lat, uint32_t jit)
{
    memset(w, 0, sizeof(*w));
    vnet_init(&w->net);
    w->loss = loss;
    w->dup = dup;
    w->lat = lat;
    w->jit = jit;
    w->peer_timeout = 5000;
    w->countdown = 3000;
    w->t = 1000;
    for (int i = 0; i < MAXP; i++)
        w->skew[i] = (int32_t)(i * 1234567 - (i & 1) * 9000000);
    w->skew[0] = 0;
    snprintf(w->names[0], CN_NAME_LEN, "Host");
    for (int i = 1; i < MAXP; i++)
        snprintf(w->names[i], CN_NAME_LEN, "Guest%d", i);
}

static void start_host(world *w)
{
    cn_config c = make_cfg(w, 0, BUILD, 100);
    cn_host_init(&w->s[0], &c, lt(w, 0));
    w->exists[0] = w->active[0] = 1;
}

static void start_guest_ex(world *w, int i, uint32_t build, uint32_t seed)
{
    cn_config c = make_cfg(w, i, build, seed);
    cn_guest_init(&w->s[i], &c, vnet_addr(0), lt(w, i));
    w->exists[i] = w->active[i] = 1;
    w->nlog[i] = 0;
}

static void start_guest(world *w, int i) { start_guest_ex(w, i, BUILD, 100 + (uint32_t)i * 7); }

static void drain(world *w, int i)
{
    cn_event e;
    while (cn_poll_event(&w->s[i], &e))
        if (w->nlog[i] < 512)
            w->log[i][w->nlog[i]++] = e;
}

static void tick(world *w)
{
    w->t += 5;
    for (int i = 0; i < MAXP; i++)
        if (w->exists[i])
            cn_fault_pump(&w->f[i], w->t);
    for (int i = 0; i < MAXP; i++)
        if (w->exists[i] && w->active[i]) {
            cn_update(&w->s[i], lt(w, i));
            drain(w, i);
        }
}

static void run_for(world *w, uint32_t ms)
{
    for (uint32_t e = 0; e < ms; e += 5)
        tick(w);
}

static int count_ev(world *w, int i, cn_event_type t)
{
    int n = 0;
    for (int k = 0; k < w->nlog[i]; k++)
        if (w->log[i][k].type == t)
            n++;
    return n;
}

static const cn_event *find_ev(world *w, int i, cn_event_type t)
{
    for (int k = 0; k < w->nlog[i]; k++)
        if (w->log[i][k].type == t)
            return &w->log[i][k];
    return NULL;
}

typedef int (*pred_fn)(world *, void *);
static int run_until(world *w, pred_fn p, void *arg, uint32_t max_ms)
{
    for (uint32_t e = 0; e < max_ms; e += 5) {
        if (p(w, arg))
            return 1;
        tick(w);
    }
    return p(w, arg);
}

static int all_connected(world *w, void *a)
{
    int n = *(int *)a;
    for (int i = 1; i < n; i++)
        if (w->s[i].link != CN_LINK_UP)
            return 0;
    return 1;
}

/* Bring up a host and n-1 guests, wait until everyone sees everyone. */
static int bring_up(world *w, int n)
{
    start_host(w);
    for (int i = 1; i < n; i++) { /* one at a time so that guest index == player id */
        start_guest(w, i);
        int k = i + 1;
        if (!run_until(w, all_connected, &k, 10000))
            return 0;
    }
    run_for(w, 300);
    return 1;
}

static int phase_is(world *w, void *a)
{
    int *p = (int *)a; /* p[0]=phase p[1]=n */
    for (int i = 0; i < p[1]; i++)
        if (w->exists[i] && w->active[i] && w->s[i].link == CN_LINK_UP && (int)w->s[i].phase != p[0])
            return 0;
    return 1;
}

/* ------------------------------------------------------------ test cases */

static void test_interp(void)
{
    printf("[interp]\n");
    cn_interp ip;
    cn_interp_init(&ip);
    ip.fixed_delay_ms = 100;
    cn_car_state s, o;
    memset(&s, 0, sizeof(s));
    s.rot[3] = 1;
    s.vel[0] = 10.0f; /* 10 m/s along x */
    /* samples every 33 ms from t=1000 to t=2000: x = 10 * t/1000 */
    for (uint32_t t = 1000; t <= 2000; t += 33) {
        s.pos[0] = 10.0f * (float)t / 1000.0f;
        cn_interp_push(&ip, t, &s, t);
    }
    /* now - 100 == 1500: between samples 1485 and 1518 */
    cn_presence pr = cn_interp_sample(&ip, 1600, &o);
    CHECK(pr == CN_PRES_ACTIVE, "pres %d", pr);
    CHECK(fabsf(o.pos[0] - 15.0f) < 0.01f, "x=%f want 15", o.pos[0]);

    /* extrapolation: newest is 1990 (1000+33*30). */
    uint32_t newest = 1000 + 33 * 30;
    pr = cn_interp_sample(&ip, newest + 100 + 300, &o);
    CHECK(pr == CN_PRES_EXTRAP, "pres %d", pr);
    CHECK(fabsf(o.pos[0] - (10.0f * newest / 1000.0f + 3.0f)) < 0.05f, "x=%f", o.pos[0]);
    pr = cn_interp_sample(&ip, newest + 100 + 800, &o);
    CHECK(pr == CN_PRES_GHOST, "pres %d", pr);
    CHECK(fabsf(o.pos[0] - (10.0f * newest / 1000.0f + 5.0f)) < 0.05f, "ghost x=%f", o.pos[0]);
    pr = cn_interp_sample(&ip, newest + 100 + CN_GHOST_MS + 50, &o);
    CHECK(pr == CN_PRES_REMOVED, "pres %d", pr);

    /* duplicates and out-of-order samples */
    cn_interp_init(&ip);
    ip.fixed_delay_ms = 100;
    uint32_t order[] = {1100, 1000, 1066, 1033, 1033, 1100};
    for (int i = 0; i < 6; i++) {
        s.pos[0] = (float)order[i];
        cn_interp_push(&ip, order[i], &s, order[i]);
    }
    CHECK(ip.count == 4, "count %d", ip.count);
    int sorted = 1;
    for (int i = 1; i < ip.count; i++)
        if ((int32_t)(ip.buf[i].t - ip.buf[i - 1].t) <= 0)
            sorted = 0;
    CHECK(sorted, "buffer not sorted");

    /* resume after prediction: no teleport, converges */
    cn_interp_init(&ip);
    ip.fixed_delay_ms = 100;
    memset(&s, 0, sizeof(s));
    s.rot[3] = 1;
    s.vel[0] = 20.0f;
    for (uint32_t t = 0; t <= 1000; t += 33) {
        s.pos[0] = 20.0f * (float)t / 1000.0f;
        cn_interp_push(&ip, t, &s, t);
    }
    uint32_t last = 33 * 30; /* 990 */
    uint32_t now = last + 100 + 400;
    cn_interp_sample(&ip, now, &o);
    float before = o.pos[0];
    /* data resumes: the car actually braked, so it is 6 m behind the prediction */
    for (uint32_t t = last + 33; t <= now - 100 + 200; t += 33) {
        s.vel[0] = 0.0f;
        s.pos[0] = 20.0f * (float)last / 1000.0f + 4.0f; /* stopped at +4m */
        cn_interp_push(&ip, t, &s, t);
    }
    now += 16;
    pr = cn_interp_sample(&ip, now, &o);
    CHECK(pr == CN_PRES_ACTIVE, "pres %d", pr);
    CHECK(fabsf(o.pos[0] - before) < 2.0f, "jumped %f m in one frame", fabsf(o.pos[0] - before));
    float target = 20.0f * (float)last / 1000.0f + 4.0f;
    for (int i = 0; i < 70; i++) { /* ~1.1 s of frames */
        now += 16;
        cn_interp_sample(&ip, now, &o);
    }
    CHECK(fabsf(o.pos[0] - target) < 0.05f, "did not converge: %f vs %f", o.pos[0], target);

    /* long silence then fresh stream: no slide from the old position */
    cn_interp_init(&ip);
    ip.fixed_delay_ms = 100;
    memset(&s, 0, sizeof(s));
    s.rot[3] = 1;
    s.pos[0] = 0;
    cn_interp_push(&ip, 0, &s, 0);
    cn_interp_sample(&ip, 300, &o);
    s.pos[0] = 500.0f;
    cn_interp_push(&ip, 20000, &s, 20000);
    pr = cn_interp_sample(&ip, 20050, &o);
    CHECK(pr == CN_PRES_ACTIVE && fabsf(o.pos[0] - 500.0f) < 0.01f, "fresh stream pos %f", o.pos[0]);
}

static void test_adaptive_delay(void)
{
    printf("[adaptive render delay]\n");
    cn_interp ip;
    cn_interp_init(&ip);
    cn_car_state s, o;
    memset(&s, 0, sizeof(s));
    s.rot[3] = 1;
    s.vel[0] = 10.0f;
    uint32_t rs = 7;
    int active = 0, total = 0;
    float max_jump = 0, prev = 0;
    /* samples every 33 ms, each delayed 150..210 ms in transit (reordering allowed) */
    static struct { uint32_t arrive, t; } pend[400];
    int np = 0;
    for (uint32_t t = 0; t < 8000; t += 33) {
        rs = rs * 1664525u + 1013904223u;
        pend[np].t = t;
        pend[np].arrive = t + 150 + (rs >> 24) % 61;
        np++;
    }
    for (uint32_t now = 0; now < 8300; now += 16) {
        for (int i = 0; i < np; i++)
            if (pend[i].arrive != 0xFFFFFFFFu && pend[i].arrive <= now) {
                s.pos[0] = 10.0f * (float)pend[i].t / 1000.0f;
                cn_interp_push(&ip, pend[i].t, &s, pend[i].arrive);
                pend[i].arrive = 0xFFFFFFFFu;
            }
        cn_presence p = cn_interp_sample(&ip, now, &o);
        if (now > 3000 && now < 7500) {
            total++;
            if (p == CN_PRES_ACTIVE)
                active++;
            float jump = fabsf(o.pos[0] - prev);
            if (jump > max_jump)
                max_jump = jump;
        }
        prev = o.pos[0];
    }
    CHECK(active * 100 >= total * 98, "only %d/%d frames interpolated", active, total);
    CHECK(max_jump < 0.5f, "car moved %.2f m in one 16 ms frame (10 m/s => 0.16 expected)", max_jump);
    CHECK(cn_interp_delay(&ip) >= 200 && cn_interp_delay(&ip) <= 280, "delay settled at %u ms", cn_interp_delay(&ip));
    printf("    settled delay %u ms, %d/%d frames interpolated, max step %.2f m\n", cn_interp_delay(&ip), active, total,
           max_jump);
    /* a quiet fast link gets a short delay */
    cn_interp_init(&ip);
    for (uint32_t t = 0; t < 3000; t += 33) {
        s.pos[0] = 10.0f * (float)t / 1000.0f;
        cn_interp_push(&ip, t, &s, t + 3);
        cn_interp_sample(&ip, t + 3, &o);
    }
    CHECK(cn_interp_delay(&ip) <= 70, "fast link delay %u ms", cn_interp_delay(&ip));
    printf("    fast-link delay %u ms\n", cn_interp_delay(&ip));
}

static void test_ranking(void)
{
    printf("[ranking]\n");
    cn_player p[CN_MAX_PLAYERS];
    memset(p, 0, sizeof(p));
    for (int i = 0; i < 4; i++)
        p[i].state = CN_PS_CONNECTED;
    p[0].finished = 1; p[0].finish_ms = 15000;
    p[1].finished = 1; p[1].finish_ms = 9000;
    p[2].finished = 0;
    p[3].finished = 1; p[3].finish_ms = 9000; /* tie with 1: lower id first */
    cn_result r[CN_MAX_PLAYERS];
    int n = cn_rank_results(p, r);
    CHECK(n == 4, "n=%d", n);
    CHECK(r[0].id == 1 && r[1].id == 3 && r[2].id == 0 && r[3].id == 2, "order %d %d %d %d", r[0].id, r[1].id, r[2].id, r[3].id);
    CHECK(r[3].finished == 0 && r[0].rank == 1 && r[3].rank == 4, "ranks");
    p[1].state = CN_PS_EMPTY;
    n = cn_rank_results(p, r);
    CHECK(n == 3 && r[0].id == 3, "empty slots skipped n=%d first=%d", n, r[0].id);
    p[0].state = p[2].state = p[3].state = CN_PS_DISCONNECTED;
    p[3].finished = 0;
    n = cn_rank_results(p, r);
    CHECK(n == 3 && r[0].id == 0, "disconnected players are ranked too");
}

static void send_raw(world *w, int from, const uint8_t *d, int len)
{
    cn_transport t = vnet_endpoint(&w->ep[from], &w->net, from);
    cn_addr to = vnet_addr(0);
    t.send(t.user, &to, d, len);
}

static void test_garbage(void)
{
    printf("[garbage packets]\n");
    static world w;
    world_init(&w, 0, 0, 0, 0);
    CHECK(bring_up(&w, 3), "bring up");
    uint32_t rs = 12345;
    for (int n = 0; n < 20000; n++) {
        uint8_t buf[300];
        int len = 1 + (int)(rs % 299);
        for (int i = 0; i < len; i++) {
            rs = rs * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(rs >> 24);
        }
        if (n % 2 == 0 && len > 5) { /* valid header, garbage body */
            buf[0] = 0x43; buf[1] = 0x4E; buf[2] = (uint8_t)(n % 2);
        }
        send_raw(&w, 1 + (n % 2), buf, len); /* from known guests */
        send_raw(&w, 3, buf, len);           /* from a stranger */
        if (n % 50 == 0) {
            tick(&w);
        }
    }
    run_for(&w, 500);
    /* also feed garbage to a guest */
    for (int n = 0; n < 2000; n++) {
        uint8_t buf[100];
        int len = 1 + n % 99;
        for (int i = 0; i < len; i++) {
            rs = rs * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(rs >> 24);
        }
        cn_transport t = vnet_endpoint(&w.ep[0], &w.net, 0);
        cn_addr to = vnet_addr(1);
        t.send(t.user, &to, buf, len);
    }
    run_for(&w, 500);
    CHECK(w.s[0].stat_bad_packets > 0, "garbage was counted");
    CHECK(w.s[0].players[1].state == CN_PS_CONNECTED && w.s[0].players[2].state == CN_PS_CONNECTED,
          "legit players survived");
    CHECK(w.s[1].link == CN_LINK_UP, "guest link survived");
}

static void test_handshake(void)
{
    printf("[handshake]\n");
    static world w;
    world_init(&w, 0, 0, 20, 5);
    start_host(&w);

    /* wrong build */
    start_guest_ex(&w, 1, BUILD + 1, 5);
    run_for(&w, 1000);
    const cn_event *e = find_ev(&w, 1, CN_EV_REJECTED);
    CHECK(e && e->value == CN_REJ_BUILD, "build mismatch rejected");
    CHECK(w.s[1].link == CN_LINK_FAILED, "link failed");

    /* wrong protocol version: craft a HELLO by hand and read the reply */
    {
        uint8_t m[5 + 4 + 1 + 4 + 4 + CN_NAME_LEN];
        memset(m, 0, sizeof(m));
        m[0] = 0x43; m[1] = 0x4E; /* magic */
        m[5] = 0; m[6] = 1 + 4 + 4 + CN_NAME_LEN; m[7] = 0; m[8] = 1; /* unrel HELLO */
        m[9] = 99;                                                     /* bad version */
        memcpy(&m[18], "Old", 3);
        send_raw(&w, 2, m, 9 + 1 + 4 + 4 + CN_NAME_LEN);
        run_for(&w, 200);
        cn_addr from;
        uint8_t reply[200];
        cn_transport t = vnet_endpoint(&w.ep[2], &w.net, 2);
        int n = t.recv(t.user, &from, reply, sizeof(reply));
        CHECK(n > 9 && reply[8] == 3 && reply[9] == CN_REJ_VERSION, "version reject n=%d type=%d reason=%d", n,
              n > 8 ? reply[8] : -1, n > 9 ? reply[9] : -1);
    }

    /* normal join */
    start_guest(&w, 2);
    run_for(&w, 1000);
    CHECK(w.s[2].link == CN_LINK_UP && w.s[2].local_id == 1, "joined as id %d", w.s[2].local_id);
    CHECK(strcmp(w.s[2].players[1].name, "Guest2") == 0, "name in list '%s'", w.s[2].players[1].name);
    CHECK(strcmp(w.s[2].players[0].name, "Host") == 0, "host name in list");

    /* duplicate name */
    snprintf(w.names[3], CN_NAME_LEN, "Guest2");
    start_guest(&w, 3);
    run_for(&w, 1000);
    e = find_ev(&w, 3, CN_EV_REJECTED);
    CHECK(e && e->value == CN_REJ_NAME_TAKEN, "name taken rejected");

    /* fill the room (4 players), the 5th is refused */
    snprintf(w.names[3], CN_NAME_LEN, "Guest3");
    start_guest(&w, 3);
    start_guest(&w, 4);
    snprintf(w.names[5], CN_NAME_LEN, "Guest5");
    run_for(&w, 1500);
    CHECK(w.s[3].link == CN_LINK_UP, "third guest up");
    start_guest(&w, 5);
    run_for(&w, 1500);
    /* index 4 took the last slot or index 5 did; exactly one of them must be refused */
    int up = (w.s[4].link == CN_LINK_UP) + (w.s[5].link == CN_LINK_UP);
    CHECK(up == 1, "exactly one more guest fits, up=%d", up);
    int refused = count_ev(&w, 4, CN_EV_REJECTED) + count_ev(&w, 5, CN_EV_REJECTED);
    CHECK(refused == 1, "one refusal, got %d", refused);
    const cn_event *r = find_ev(&w, up && w.s[4].link == CN_LINK_UP ? 5 : 4, CN_EV_REJECTED);
    CHECK(r && r->value == CN_REJ_FULL, "reason full");

    /* nobody answers: guest times out */
    {
        static world w2;
        world_init(&w2, 0, 0, 0, 0);
        start_guest(&w2, 1);
        run_for(&w2, 6000);
        CHECK(count_ev(&w2, 1, CN_EV_CONNECT_FAILED) == 1 && w2.s[1].link == CN_LINK_FAILED, "connect timeout");
    }
}

static int counter_arrived(world *w, void *a)
{
    (void)a;
    return w->s[0].stat_rel_delivered >= w->s[1].stat_rel_queued && w->s[1].peers[0].qcount == 0;
}

static void test_reliable(void)
{
    printf("[reliable channel: 30%% loss, 10%% duplicates, jitter]\n");
    static world w;
    world_init(&w, 0.30f, 0.10f, 30, 60);
    w.peer_timeout = 30000; /* lossy: do not drop the peer */
    CHECK(bring_up(&w, 2), "bring up under loss");
    uint32_t base = w.s[1].stat_rel_queued;
    uint32_t base_rx = w.s[0].stat_rel_delivered;
    uint8_t blob[CN_CAR_BLOB];
    for (int i = 0; i < 100; i++) { /* control messages are rare in practice: ~25/s here */
        memset(blob, 0, sizeof(blob));
        blob[0] = (uint8_t)i;
        blob[1] = (uint8_t)(i >> 8);
        cn_set_car(&w.s[1], blob);
        cn_set_ready(&w.s[1], i & 1);
        run_for(&w, 80);
    }
    int ok = run_until(&w, counter_arrived, NULL, 30000);
    CHECK(ok, "all reliable messages delivered");
    CHECK(w.s[1].stat_rel_queued - base == 200, "queued %u", w.s[1].stat_rel_queued - base);
    CHECK(w.s[0].stat_rel_delivered - base_rx == 200, "delivered exactly once: %u", w.s[0].stat_rel_delivered - base_rx);
    CHECK(w.s[0].players[1].car[0] == (uint8_t)99 && w.s[0].players[1].car[1] == 0, "last car choice won");
    CHECK(w.s[0].players[1].ready == 1, "last ready state won (99 is odd)");
    run_for(&w, 1000);
    CHECK(w.s[1].players[1].car[0] == 99, "host list echoed the final choice to the guest");
    printf("    sent %u dropped %u packets\n", w.f[1].sent, w.f[1].dropped);
}

static void test_clock(void)
{
    printf("[clock sync]\n");
    static world w;
    world_init(&w, 0.02f, 0, 40, 20);
    w.skew[1] = 987654321;
    w.skew[2] = -123456789;
    CHECK(bring_up(&w, 3), "bring up");
    run_for(&w, 3000);
    for (int i = 1; i < 3; i++) {
        int32_t err = (int32_t)(cn_host_time(&w.s[i], lt(&w, i)) - lt(&w, 0));
        CHECK(abs(err) <= 15, "guest %d clock error %d ms (synced=%d, rtt=%u)", i, err, w.s[i].clock_synced,
              w.s[i].last_rtt);
        CHECK(w.s[i].clock_synced, "synced flag");
        printf("    guest %d error %d ms, rtt %u ms\n", i, err, w.s[i].last_rtt);
    }
}

typedef struct {
    int n;
    uint32_t fin[MAXP];
    int reported[MAXP];
} racecfg;

static int race_done(world *w, void *a)
{
    racecfg *rc = (racecfg *)a;
    for (int i = 0; i < rc->n; i++) {
        cn_session *s = &w->s[i];
        if (w->active[i] && s->link == CN_LINK_UP && s->phase == CN_PH_RACING && !rc->reported[i]) {
            uint32_t el = cn_host_time(s, lt(w, i)) - s->race_start;
            if (el >= rc->fin[i]) {
                cn_report_finish(s, rc->fin[i], lt(w, i));
                rc->reported[i] = 1;
            }
        }
    }
    for (int i = 0; i < rc->n; i++)
        if (w->active[i] && w->s[i].link == CN_LINK_UP && w->s[i].phase != CN_PH_RESULTS)
            return 0;
    return 1;
}

static int lobby_ready(world *w, void *a)
{
    int n = *(int *)a;
    for (int i = 0; i < n; i++)
        if (w->active[i] && w->s[i].link == CN_LINK_UP && w->s[i].phase != CN_PH_LOBBY)
            return 0;
    return 1;
}

static void check_same_results(world *w, int n, const int *order, int ndnf_last)
{
    (void)ndnf_last;
    for (int i = 0; i < n; i++) {
        if (!w->active[i] || w->s[i].link != CN_LINK_UP)
            continue;
        CHECK(w->s[i].n_results == n, "peer %d has %d results", i, w->s[i].n_results);
        for (int k = 0; k < w->s[i].n_results && k < n; k++)
            CHECK(w->s[i].results[k].id == order[k], "peer %d rank %d is player %d, want %d", i, k + 1,
                  w->s[i].results[k].id, order[k]);
    }
}

static int run_full_race(world *w, int n, uint32_t event_id, const uint32_t *fin, const int *order)
{
    for (int i = 0; i < n; i++)
        cn_set_ready(&w->s[i], 1);
    run_for(w, 1500);
    CHECK(cn_host_start_event(&w->s[0], event_id, lt(w, 0)) == 0, "start event");
    int ph[2] = {CN_PH_LOADING, n};
    CHECK(run_until(w, phase_is, ph, 5000), "everyone loading");
    for (int i = 0; i < n; i++)
        CHECK(w->s[i].event_id == event_id, "peer %d event id %u", i, w->s[i].event_id);
    /* staggered load times */
    for (int i = n - 1; i >= 0; i--) {
        run_for(w, 300 + 100u * (uint32_t)i);
        cn_report_loaded(&w->s[i], lt(w, i));
    }
    int cd[2] = {CN_PH_COUNTDOWN, n};
    CHECK(run_until(w, phase_is, cd, 10000), "everyone in countdown");
    uint32_t start0 = w->s[0].race_start;
    for (int i = 1; i < n; i++)
        CHECK(w->s[i].race_start == start0, "peer %d start %u vs %u", i, w->s[i].race_start, start0);
    int race[2] = {CN_PH_RACING, n};
    CHECK(run_until(w, phase_is, race, 10000), "everyone racing");
    for (int i = 0; i < n; i++) {
        /* RACE_GO must not fire early on anyone's clock estimate */
        int32_t early = (int32_t)(cn_host_time(&w->s[i], lt(w, i)) - start0);
        CHECK(early >= -25 && early < 400, "peer %d race go at %d ms from start", i, early);
    }
    racecfg rc;
    memset(&rc, 0, sizeof(rc));
    rc.n = n;
    memcpy(rc.fin, fin, sizeof(uint32_t) * (size_t)n);
    int ok = run_until(w, race_done, &rc, 60000);
    CHECK(ok, "results arrived");
    run_for(w, 500);
    check_same_results(w, n, order, 0);
    CHECK(cn_host_return_to_lobby(&w->s[0], lt(w, 0)) == 0, "return to lobby");
    int lob = 0;
    CHECK(run_until(w, lobby_ready, &n, 5000) || lob, "everyone back in lobby");
    run_for(w, 500);
    for (int i = 0; i < n; i++)
        if (w->s[i].link == CN_LINK_UP) {
            CHECK(w->s[i].phase == CN_PH_LOBBY, "peer %d phase", i);
            for (int k = 0; k < CN_MAX_PLAYERS; k++)
                CHECK(w->s[i].players[k].ready == 0, "peer %d sees ready cleared for %d", i, k);
        }
    return ok;
}

static void test_full_flow(void)
{
    printf("[full flow: 4 players, 5%% loss, 30 ms + 100 ms jitter, clock skew; two races back to back]\n");
    static world w;
    world_init(&w, 0.05f, 0.02f, 30, 100);
    CHECK(bring_up(&w, 4), "bring up");
    uint32_t fin1[4] = {12000, 9000, 15000, 9000};
    int order1[4] = {1, 3, 0, 2};
    run_full_race(&w, 4, 1001, fin1, order1);
    uint32_t fin2[4] = {8000, 9500, 7000, 11000};
    int order2[4] = {2, 0, 1, 3};
    run_full_race(&w, 4, 1002, fin2, order2);
    for (int i = 0; i < 4; i++)
        CHECK(count_ev(&w, i, CN_EV_RACE_GO) == 2, "peer %d race go count %d", i, count_ev(&w, i, CN_EV_RACE_GO));
}

static void test_rules(void)
{
    printf("[rules]\n");
    static world w;
    world_init(&w, 0, 0, 10, 0);
    CHECK(bring_up(&w, 3), "bring up");
    CHECK(cn_host_start_event(&w.s[1], 5, lt(&w, 1)) == CN_ERR_NOT_HOST, "guest cannot start");
    CHECK(cn_host_start_event(&w.s[0], 5, lt(&w, 0)) == CN_ERR_NOT_READY, "nobody ready");
    cn_set_ready(&w.s[0], 1);
    cn_set_ready(&w.s[1], 1);
    run_for(&w, 500);
    CHECK(cn_host_start_event(&w.s[0], 5, lt(&w, 0)) == CN_ERR_NOT_READY, "one guest still not ready");
    cn_set_ready(&w.s[2], 1);
    run_for(&w, 500);
    CHECK(cn_host_start_event(&w.s[0], 5, lt(&w, 0)) == 0, "all ready: starts");
    CHECK(cn_host_start_event(&w.s[0], 6, lt(&w, 0)) == CN_ERR_BAD_PHASE, "second start refused");
    CHECK(cn_host_return_to_lobby(&w.s[0], lt(&w, 0)) == CN_ERR_BAD_PHASE, "cannot return mid-load");
    run_for(&w, 300);
    /* a stranger cannot join mid-race */
    start_guest(&w, 3);
    run_for(&w, 1000);
    const cn_event *e = find_ev(&w, 3, CN_EV_REJECTED);
    CHECK(e && e->value == CN_REJ_IN_PROGRESS, "join during an event refused");
    /* ready/car changes are ignored outside the lobby */
    uint8_t before = w.s[0].players[1].ready;
    cn_set_ready(&w.s[1], 0);
    run_for(&w, 300);
    CHECK(w.s[0].players[1].ready == before, "ready ignored outside lobby");
    /* loading timeout drops the player who never loads */
    {
        static world w2;
        world_init(&w2, 0, 0, 10, 0);
        CHECK(bring_up(&w2, 3), "bring up 2");
        w2.s[0].cfg.load_timeout_ms = 2000;
        for (int i = 0; i < 3; i++)
            cn_set_ready(&w2.s[i], 1);
        run_for(&w2, 500);
        cn_host_start_event(&w2.s[0], 9, lt(&w2, 0));
        run_for(&w2, 300);
        cn_report_loaded(&w2.s[0], lt(&w2, 0));
        cn_report_loaded(&w2.s[1], lt(&w2, 1)); /* guest 2 never loads */
        run_for(&w2, 6000);
        CHECK(w2.s[0].players[2].state == CN_PS_DISCONNECTED, "slow loader dropped");
        CHECK(w2.s[0].phase == CN_PH_COUNTDOWN || w2.s[0].phase == CN_PH_RACING, "race goes on without them");
    }
}

static void test_guest_drop(void)
{
    printf("[guest vanishes mid-race]\n");
    static world w;
    world_init(&w, 0.03f, 0, 25, 30);
    w.peer_timeout = 2000;
    CHECK(bring_up(&w, 4), "bring up");
    for (int i = 0; i < 4; i++)
        cn_set_ready(&w.s[i], 1);
    run_for(&w, 1500);
    cn_host_start_event(&w.s[0], 42, lt(&w, 0));
    run_for(&w, 500);
    for (int i = 0; i < 4; i++)
        cn_report_loaded(&w.s[i], lt(&w, i));
    int race[2] = {CN_PH_RACING, 4};
    CHECK(run_until(&w, phase_is, race, 10000), "racing");
    w.active[2] = 0; /* guest 2's process dies */
    racecfg rc;
    memset(&rc, 0, sizeof(rc));
    rc.n = 4;
    rc.fin[0] = 6000; rc.fin[1] = 7000; rc.fin[2] = 5000; rc.fin[3] = 8000;
    CHECK(run_until(&w, race_done, &rc, 60000), "race ended for the rest");
    run_for(&w, 500);
    CHECK(count_ev(&w, 0, CN_EV_PLAYER_LOST) == 1, "host noticed the loss");
    int order[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        if (i == 2)
            continue;
        CHECK(w.s[i].n_results == 4, "peer %d results %d", i, w.s[i].n_results);
        for (int k = 0; k < 4 && k < w.s[i].n_results; k++)
            CHECK(w.s[i].results[k].id == order[k], "peer %d rank %d is %d want %d", i, k + 1, w.s[i].results[k].id,
                  order[k]);
        CHECK(w.s[i].results[3].finished == 0, "dropped player is a DNF");
    }
}

static void test_host_drop(void)
{
    printf("[host vanishes]\n");
    static world w;
    world_init(&w, 0.02f, 0, 25, 20);
    w.peer_timeout = 2000;
    CHECK(bring_up(&w, 3), "bring up");
    w.active[0] = 0;
    run_for(&w, 4000);
    for (int i = 1; i < 3; i++) {
        CHECK(count_ev(&w, i, CN_EV_HOST_LOST) == 1, "guest %d saw the host go", i);
        CHECK(w.s[i].link == CN_LINK_FAILED, "guest %d link failed", i);
    }
    /* clean shutdown reaches guests quickly */
    static world w2;
    world_init(&w2, 0, 0, 20, 0);
    CHECK(bring_up(&w2, 3), "bring up 2");
    cn_leave(&w2.s[0], lt(&w2, 0));
    run_for(&w2, 300);
    CHECK(count_ev(&w2, 1, CN_EV_HOST_LOST) == 1 && count_ev(&w2, 2, CN_EV_HOST_LOST) == 1, "host leave noticed fast");
}

static void test_leave_and_rejoin(void)
{
    printf("[guest leaves in lobby, another vanishes and rejoins during countdown]\n");
    static world w;
    world_init(&w, 0.03f, 0, 25, 30);
    w.peer_timeout = 2000;
    w.countdown = 12000;
    CHECK(bring_up(&w, 4), "bring up");
    cn_leave(&w.s[3], lt(&w, 3));
    w.active[3] = 0;
    run_for(&w, 500);
    CHECK(count_ev(&w, 0, CN_EV_PLAYER_LEFT) == 1, "host saw guest 3 leave");
    CHECK(w.s[0].players[3].state == CN_PS_EMPTY, "slot freed in the lobby");
    CHECK(w.s[1].players[3].state == CN_PS_EMPTY, "other guests see the free slot");

    for (int i = 0; i < 3; i++)
        cn_set_ready(&w.s[i], 1);
    run_for(&w, 1000);
    cn_host_start_event(&w.s[0], 77, lt(&w, 0));
    run_for(&w, 400);
    for (int i = 0; i < 3; i++)
        cn_report_loaded(&w.s[i], lt(&w, i));
    int cd[2] = {CN_PH_COUNTDOWN, 3};
    CHECK(run_until(&w, phase_is, cd, 5000), "countdown");
    run_for(&w, 500);
    w.active[2] = 0; /* crash */
    run_for(&w, 3500);
    CHECK(w.s[0].players[2].state == CN_PS_DISCONNECTED, "host marked guest 2 lost");
    /* restart the process: same name, new nonce, new local time base */
    w.skew[2] += 55555;
    start_guest_ex(&w, 2, BUILD, 999);
    run_for(&w, 1500);
    CHECK(w.s[2].link == CN_LINK_UP && w.s[2].local_id == 2, "rejoined with the same id (%d)", w.s[2].local_id);
    CHECK(count_ev(&w, 0, CN_EV_PLAYER_BACK) == 1, "host saw it come back");
    CHECK(count_ev(&w, 2, CN_EV_START_EVENT) == 1 && w.s[2].event_id == 77, "rejoiner got the event id");
    CHECK(count_ev(&w, 2, CN_EV_COUNTDOWN) == 1 && w.s[2].race_start == w.s[0].race_start, "rejoiner got the start time");
    run_for(&w, 12000);
    CHECK(count_ev(&w, 2, CN_EV_RACE_GO) == 1, "rejoiner reached race go");
    CHECK(w.s[0].players[2].state == CN_PS_CONNECTED, "host has it connected");
}

static void test_cars(void)
{
    printf("[car relay through host, spoof protection, interpolation error]\n");
    static world w;
    world_init(&w, 0.03f, 0, 15, 30);
    CHECK(bring_up(&w, 4), "bring up");
    /* get into a race so states mean something */
    for (int i = 0; i < 4; i++)
        cn_set_ready(&w.s[i], 1);
    run_for(&w, 1500);
    cn_host_start_event(&w.s[0], 1, lt(&w, 0));
    run_for(&w, 500);
    for (int i = 0; i < 4; i++)
        cn_report_loaded(&w.s[i], lt(&w, i));
    int race[2] = {CN_PH_RACING, 4};
    CHECK(run_until(&w, phase_is, race, 10000), "racing");

    /* every player drives a circle: p(t) = (R cos(wt + id), 0, R sin(wt + id)) in HOST time */
    const float R = 100.0f, W = 0.3f;
    double sum_err[4][4] = {{0}}, max_err[4][4] = {{0}};
    int samples[4][4] = {{0}};
    uint32_t last_send = 0;
    for (int step = 0; step < 600; step++) { /* 3 s at 5 ms ticks */
        tick(&w);
        for (int i = 0; i < 4; i++) {
            uint32_t now = lt(&w, i);
            if ((uint32_t)(w.t - last_send) >= 33 || step == 0) {
                if (i == 3)
                    last_send = w.t;
                double th = W * (double)(cn_host_time(&w.s[i], now) % 600000) / 1000.0 + i;
                cn_car_state c;
                memset(&c, 0, sizeof(c));
                c.pos[0] = (float)(R * cos(th));
                c.pos[2] = (float)(R * sin(th));
                c.vel[0] = (float)(-R * W * sin(th));
                c.vel[2] = (float)(R * W * cos(th));
                c.rot[3] = 1;
                cn_send_car_state(&w.s[i], &c, now);
            }
        }
        if (step > 150) {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    if (i == j)
                        continue;
                    cn_car_state o;
                    uint32_t now = lt(&w, i);
                    cn_presence p = cn_remote_state(&w.s[i], (uint8_t)j, now, &o);
                    if (p != CN_PRES_ACTIVE)
                        continue;
                    double th = W * (double)((cn_host_time(&w.s[i], now) - cn_remote_delay(&w.s[i], (uint8_t)j)) % 600000) / 1000.0 + j;
                    double ex = R * cos(th), ez = R * sin(th);
                    double err = sqrt((o.pos[0] - ex) * (o.pos[0] - ex) + (o.pos[2] - ez) * (o.pos[2] - ez));
                    sum_err[i][j] += err;
                    if (err > max_err[i][j])
                        max_err[i][j] = err;
                    samples[i][j]++;
                }
        }
    }
    double worst = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            if (i == j)
                continue;
            CHECK(samples[i][j] > 300, "peer %d saw car %d in %d samples", i, j, samples[i][j]);
            if (samples[i][j]) {
                double avg = sum_err[i][j] / samples[i][j];
                if (avg > worst)
                    worst = avg;
                CHECK(avg < 2.0, "peer %d sees car %d with avg error %.2f m (max %.2f)", i, j, avg, max_err[i][j]);
            }
        }
    printf("    worst average error %.2f m on a 30 m/s car (loss 3%%, 15 ms + 30 ms jitter per hop, relayed)\n", worst);
    CHECK(w.s[1].interp[1].count == 0, "own car never enters own buffer");

    /* spoof: guest 1 claims to be player 3 */
    {
        uint8_t m[64];
        memset(m, 0, sizeof(m));
        m[0] = 0x43; m[1] = 0x4E;       /* magic; flags and ack stay 0 */
        m[5] = 0; m[6] = 55; m[7] = 0;  /* unreliable item, payload 55 bytes */
        m[8] = 0x10;                    /* CAR_STATE */
        m[9] = 3;                       /* claimed id */
        uint32_t t = cn_host_time(&w.s[0], lt(&w, 0)) + 50;
        memcpy(&m[10], &t, 4);
        float x = 99999.0f;
        memcpy(&m[14], &x, 4); /* pos.x */
        int len = 64;
        send_raw(&w, 1, m, len);
        run_for(&w, 400);
        int polluted = 0;
        for (int k = 0; k < w.s[2].interp[3].count; k++)
            if (w.s[2].interp[3].buf[k].s.pos[0] > 90000.0f)
                polluted = 1;
        int as1 = 0;
        for (int k = 0; k < w.s[2].interp[1].count; k++)
            if (w.s[2].interp[1].buf[k].s.pos[0] > 90000.0f)
                as1 = 1;
        CHECK(!polluted, "spoofed id did not reach player 3's buffer");
        CHECK(as1, "the state was attributed to the real sender (player 1)");
    }
}

int main(void)
{
    printf("co-op net library tests\n");
    test_interp();
    test_adaptive_delay();
    test_ranking();
    test_garbage();
    test_handshake();
    test_reliable();
    test_clock();
    test_rules();
    test_full_flow();
    test_guest_drop();
    test_host_drop();
    test_leave_and_rejoin();
    test_cars();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
