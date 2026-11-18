/*
 * Co-op simulator: one process = one player with a fake car driving a circle.
 * Runs the real net library over real UDP (optionally with injected loss/latency).
 *
 *   sim host --port 47000 --name Host  --players 3
 *   sim join --addr 127.0.0.1:47000 --name Alice
 *
 * Every car follows a path that is a pure function of (player id, host time),
 * so each receiver can compare what it renders with the ground truth and
 * print its own interpolation error.
 *
 * Exit codes: 0 race finished and results printed, 2 host lost, 3 rejected,
 * 4 could not connect, 5 usage / socket error.
 */
#define WIN32_LEAN_AND_MEAN
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../cn_session.h"
#include "../cn_transports.h"

#define BUILD_HASH 0x4E465331u
#define R 100.0
#define W 0.3 /* rad/s: 30 m/s */

static uint32_t now_ms(void)
{
    static LARGE_INTEGER f, t0;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart) + 1000;
}

static void truth(int id, uint32_t race_ms, cn_car_state *c)
{
    double th = W * (double)race_ms / 1000.0 + id * 1.3;
    memset(c, 0, sizeof(*c));
    c->pos[0] = (float)(R * cos(th));
    c->pos[2] = (float)(R * sin(th));
    c->vel[0] = (float)(-R * W * sin(th));
    c->vel[2] = (float)(R * W * cos(th));
    c->rot[3] = 1.0f;
}

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
    for (int i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], key) == 0)
            return argv[i + 1];
    return def;
}

static cn_session g_s; /* large: keep it off the stack */

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "join"))) {
        fprintf(stderr, "usage: sim host [--port P] [--name N] [--players K] [--loss %%] [--lat ms] [--jitter ms]\n"
                        "       sim join --addr ip:port [--name N] [--loss %%] [--lat ms] [--jitter ms]\n");
        return 5;
    }
    int is_host = strcmp(argv[1], "host") == 0;
    const char *name = arg(argc, argv, "--name", is_host ? "Host" : "Guest");
    int want_players = atoi(arg(argc, argv, "--players", "2"));
    float loss = (float)atof(arg(argc, argv, "--loss", "0")) / 100.0f;
    uint32_t lat = (uint32_t)atoi(arg(argc, argv, "--lat", "0"));
    uint32_t jit = (uint32_t)atoi(arg(argc, argv, "--jitter", "0"));
    uint32_t race_len = (uint32_t)atoi(arg(argc, argv, "--race-ms", "8000"));
    uint16_t port = (uint16_t)atoi(arg(argc, argv, "--port", is_host ? "47000" : "0"));

    cn_udp udp;
    if (cn_udp_open(&udp, port) != 0) {
        fprintf(stderr, "[%s] cannot open UDP port %u\n", name, port);
        return 5;
    }
    static cn_fault fault;
    cn_fault_init(&fault, cn_udp_transport(&udp), (uint32_t)GetCurrentProcessId() * 2654435761u, loss, 0.0f, lat, jit);

    cn_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tr = cn_fault_transport(&fault);
    cfg.build_hash = BUILD_HASH;
    cfg.name = name;
    cfg.seed = (uint32_t)GetCurrentProcessId() ^ GetTickCount();
    cfg.countdown_ms = 2000;

    uint32_t t = now_ms();
    if (is_host) {
        cn_host_init(&g_s, &cfg, t);
        printf("[%s] hosting on UDP %u, waiting for %d players\n", name, port, want_players);
    } else {
        cn_addr a;
        if (cn_addr_parse(arg(argc, argv, "--addr", "127.0.0.1:47000"), &a) != 0) {
            fprintf(stderr, "bad --addr\n");
            return 5;
        }
        cn_guest_init(&g_s, &cfg, a, t);
    }
    fflush(stdout);

    int my_id = 0, ready_sent = 0, started = 0, loaded_at_set = 0, finished = 0, results_done = 0;
    uint32_t connected_at = 0, loaded_at = 0, race_t0 = 0, race_finish_at = 0, results_at = 0;
    uint32_t last_send = 0, last_print = 0;
    double sum_err = 0, max_err = 0;
    long err_n = 0;
    long pres_counts[5] = {0};
    int exit_code = 0;
    unsigned jit_rand = cfg.seed;

    for (;;) {
        t = now_ms();
        cn_fault_pump(&fault, t);
        cn_update(&g_s, t);

        cn_event e;
        while (cn_poll_event(&g_s, &e)) {
            switch (e.type) {
            case CN_EV_CONNECTED:
                my_id = e.player;
                connected_at = t;
                printf("[%s] connected as player %d\n", name, my_id);
                break;
            case CN_EV_REJECTED:
                printf("[%s] REJECTED reason=%u\n", name, e.value);
                return 3;
            case CN_EV_CONNECT_FAILED:
                printf("[%s] could not connect\n", name);
                return 4;
            case CN_EV_PLAYER_JOINED:
                printf("[%s] player %d joined: %s\n", name, e.player, g_s.players[e.player].name);
                break;
            case CN_EV_PLAYER_LOST:
            case CN_EV_PLAYER_LEFT:
                printf("[%s] player %d %s\n", name, e.player, e.type == CN_EV_PLAYER_LEFT ? "left" : "lost");
                break;
            case CN_EV_START_EVENT:
                printf("[%s] event %u: loading...\n", name, e.value);
                loaded_at = t + 600 + (jit_rand = jit_rand * 1664525u + 1013904223u) % 1200;
                loaded_at_set = 1;
                break;
            case CN_EV_COUNTDOWN:
                printf("[%s] countdown, start at host time %u (now %u)\n", name, e.value,
                       cn_host_time(&g_s, t));
                break;
            case CN_EV_RACE_GO:
                race_t0 = t;
                race_finish_at = t + race_len + (uint32_t)my_id * 400;
                printf("[%s] GO!\n", name);
                break;
            case CN_EV_RESULTS:
                results_at = t;
                printf("[%s] RESULTS:\n", name);
                for (int i = 0; i < g_s.n_results; i++) {
                    const cn_result *r = &g_s.results[i];
                    printf("[%s]   #%d  player %d (%s)  %s %u ms\n", name, r->rank, r->id, g_s.players[r->id].name,
                           r->finished ? "finished" : "DNF", r->finish_ms);
                }
                results_done = 1;
                break;
            case CN_EV_HOST_LOST:
                printf("[%s] HOST LOST\n", name);
                exit_code = 2;
                break;
            default:
                break;
            }
        }
        if (exit_code)
            return exit_code;

        if (g_s.link == CN_LINK_UP && g_s.phase == CN_PH_LOBBY && !ready_sent) {
            if (is_host || (uint32_t)(t - connected_at) > 700) {
                uint8_t car[CN_CAR_BLOB] = {0};
                car[0] = (uint8_t)(10 + my_id);
                cn_set_car(&g_s, car);
                cn_set_ready(&g_s, 1);
                ready_sent = 1;
            }
        }
        if (is_host && g_s.phase == CN_PH_LOBBY && !started) {
            int connected = 0, ready = 0;
            for (int i = 0; i < CN_MAX_PLAYERS; i++)
                if (g_s.players[i].state == CN_PS_CONNECTED) {
                    connected++;
                    ready += g_s.players[i].ready;
                }
            if (connected >= want_players && ready == connected && cn_host_start_event(&g_s, 4242, t) == 0) {
                started = 1;
                printf("[%s] all %d players ready: starting event 4242\n", name, connected);
            }
        }
        if (loaded_at_set && (int32_t)(t - loaded_at) >= 0 && g_s.phase == CN_PH_LOADING) {
            cn_report_loaded(&g_s, t);
            loaded_at_set = 0;
        }

        if (g_s.phase == CN_PH_RACING && !finished) {
            if ((uint32_t)(t - last_send) >= 33) {
                last_send = t;
                cn_car_state c;
                truth(my_id, cn_host_time(&g_s, t) - g_s.race_start, &c);
                cn_send_car_state(&g_s, &c, t);
            }
            for (int j = 0; j < CN_MAX_PLAYERS; j++) {
                if (j == my_id || g_s.players[j].state != CN_PS_CONNECTED)
                    continue;
                cn_car_state o;
                cn_presence p = cn_remote_state(&g_s, (uint8_t)j, t, &o);
                pres_counts[p]++;
                int32_t shown_ms = (int32_t)(cn_host_time(&g_s, t) - cn_remote_delay(&g_s, (uint8_t)j) - g_s.race_start);
                if (p == CN_PRES_ACTIVE && shown_ms > 0) {
                    cn_car_state ex;
                    truth(j, (uint32_t)shown_ms, &ex);
                    double dx = o.pos[0] - ex.pos[0], dz = o.pos[2] - ex.pos[2];
                    double err = sqrt(dx * dx + dz * dz);
                    sum_err += err;
                    if (err > max_err)
                        max_err = err;
                    err_n++;
                }
            }
            if ((uint32_t)(t - last_print) >= 1000) {
                last_print = t;
                printf("[%s] racing %.1fs, rtt %u ms, clock offset %d ms\n", name, (double)(t - race_t0) / 1000.0,
                       g_s.last_rtt, g_s.clock_offset);
            }
            if ((int32_t)(t - race_finish_at) >= 0) {
                cn_report_finish(&g_s, t - race_t0, t);
                finished = 1;
                printf("[%s] finished in %u ms\n", name, t - race_t0);
            }
        }

        if (results_done && (uint32_t)(t - results_at) > (is_host ? 1500u : 600u)) {
            if (err_n)
                printf("[%s] INTERP avg error %.3f m, max %.3f m over %ld samples (active %ld, predicted %ld, ghost %ld)\n",
                       name, sum_err / (double)err_n, max_err, err_n, pres_counts[CN_PRES_ACTIVE],
                       pres_counts[CN_PRES_EXTRAP], pres_counts[CN_PRES_GHOST]);
            printf("[%s] sent %u packets (%u bytes), received %u, dropped by fault layer %u\n", name,
                   g_s.stat_tx_packets, g_s.stat_tx_bytes, g_s.stat_rx_packets, fault.dropped);
            fflush(stdout);
            cn_leave(&g_s, t);
            cn_fault_pump(&fault, t + 1000);
            return 0;
        }
        fflush(stdout);
        Sleep(2);
    }
}
