/*
 * Co-op session: one object for both roles.
 *
 * Star topology: guests talk only to the host (player id 0). The host
 * relays car states, runs the lobby / race state machine and ranks results.
 *
 * The caller owns time and sockets:
 *   - now is a local monotonic millisecond clock (uint32, may wrap)
 *   - the transport sends and receives datagrams
 * Call cn_update() often (every few ms) and drain cn_poll_event().
 */
#ifndef CN_SESSION_H
#define CN_SESSION_H

#include "cn_common.h"
#include "cn_interp.h"

#define CN_MAX_MSG 320
#define CN_REL_QUEUE 64

typedef enum { CN_ROLE_HOST = 0, CN_ROLE_GUEST = 1 } cn_role;

typedef enum {
    CN_PH_LOBBY = 0,
    CN_PH_LOADING,
    CN_PH_COUNTDOWN,
    CN_PH_RACING,
    CN_PH_RESULTS
} cn_phase;

typedef enum {
    CN_PS_EMPTY = 0,
    CN_PS_CONNECTED,
    CN_PS_DISCONNECTED /* lost mid-session; may rejoin by name */
} cn_pstate;

typedef enum {
    CN_LINK_IDLE = 0,
    CN_LINK_CONNECTING, /* guest, waiting for WELCOME */
    CN_LINK_UP,
    CN_LINK_FAILED, /* rejected, timed out or host lost */
    CN_LINK_LEFT
} cn_link;

typedef enum {
    CN_REJ_VERSION = 1,
    CN_REJ_BUILD = 2,
    CN_REJ_FULL = 3,
    CN_REJ_NAME_TAKEN = 4,
    CN_REJ_IN_PROGRESS = 5
} cn_reject_reason;

typedef struct {
    cn_pstate state;
    char name[CN_NAME_LEN];
    uint8_t ready, loaded, finished;
    uint8_t car[CN_CAR_BLOB];
    uint32_t finish_ms;
    uint16_t ping_ms;
} cn_player;

typedef struct {
    uint8_t id;
    uint8_t finished; /* 0 = DNF */
    uint8_t rank;     /* 1-based */
    uint32_t finish_ms;
} cn_result;

typedef enum {
    CN_EV_NONE = 0,
    CN_EV_CONNECTED,      /* guest: handshake done, player = our id */
    CN_EV_REJECTED,       /* guest: value = cn_reject_reason */
    CN_EV_CONNECT_FAILED, /* guest: no answer from host */
    CN_EV_PLAYER_JOINED,  /* host: player = id */
    CN_EV_PLAYER_BACK,    /* host: a lost player rejoined */
    CN_EV_PLAYER_LOST,    /* host: no packets for a while */
    CN_EV_PLAYER_LEFT,    /* host: player said goodbye */
    CN_EV_LIST_CHANGED,   /* players[] changed */
    CN_EV_START_EVENT,    /* value = event id; load it, then cn_report_loaded() */
    CN_EV_COUNTDOWN,      /* value = race start in host time (ms) */
    CN_EV_RACE_GO,        /* the countdown ended */
    CN_EV_PLAYER_FINISHED,/* player, value = finish ms */
    CN_EV_RESULTS,        /* s->results / s->n_results are valid */
    CN_EV_BACK_TO_LOBBY,
    CN_EV_HOST_LOST       /* guest: host gone; the race is over */
} cn_event_type;

typedef struct {
    uint8_t type;
    uint8_t player;
    uint32_t value;
} cn_event;

typedef struct {
    cn_transport tr;
    uint32_t build_hash; /* exe identity; mismatches are rejected */
    const char *name;    /* player name, 1..15 chars */
    uint32_t seed;       /* any value; seeds the connection nonce */
    /* host tuning (0 = default) */
    uint32_t countdown_ms;      /* default 3000 */
    uint32_t load_timeout_ms;   /* default 60000 */
    uint32_t finish_grace_ms;   /* after first finisher, default 30000 */
    uint32_t race_timeout_ms;   /* default 600000 */
    uint32_t peer_timeout_ms;   /* default 5000 */
} cn_config;

typedef struct {
    uint16_t seq;
    uint16_t len;
    uint8_t type;
    uint8_t sent;
    uint32_t last_sent;
    uint8_t data[CN_MAX_MSG];
} cn_rel_msg;

typedef struct {
    cn_addr addr;
    int active;
    uint32_t nonce;
    uint16_t send_next, recv_next;
    int need_ack;
    cn_rel_msg q[CN_REL_QUEUE];
    int qhead, qcount;
    uint32_t last_heard, last_hb;
} cn_peer;

typedef struct { uint32_t rtt; int32_t off; } cn_clk_sample;

typedef struct {
    cn_role role;
    cn_config cfg;
    char name[CN_NAME_LEN];
    cn_link link;
    uint8_t local_id;
    cn_phase phase;
    uint32_t event_id;
    uint32_t race_start; /* host time */
    cn_player players[CN_MAX_PLAYERS];
    cn_result results[CN_MAX_PLAYERS];
    int n_results;

    cn_peer peers[CN_MAX_PLAYERS]; /* host: by player id; guest: [0] = host */
    cn_addr host_addr;
    uint32_t nonce;
    uint32_t rng;

    /* guest clock sync */
    int32_t clock_offset; /* host_time = local + offset */
    int clock_samples;
    int clock_synced;
    cn_clk_sample samples[8];
    uint16_t last_rtt;
    uint32_t last_ping, hello_started, last_hello;
    int pings_sent;

    /* host race bookkeeping */
    uint32_t phase_since, first_finish_at;
    int list_dirty;
    int list_applied; /* guest: phase taken from the first list only */

    cn_interp interp[CN_MAX_PLAYERS];

    cn_event ev[64];
    int ev_head, ev_count;

    /* statistics */
    uint32_t stat_tx_packets, stat_rx_packets, stat_tx_bytes, stat_bad_packets;
    uint32_t stat_rel_queued, stat_rel_delivered; /* reliable messages queued by us / accepted in order from peers */
} cn_session;

void cn_host_init(cn_session *s, const cn_config *cfg, uint32_t now);
void cn_guest_init(cn_session *s, const cn_config *cfg, cn_addr host, uint32_t now);
void cn_update(cn_session *s, uint32_t now);
int cn_poll_event(cn_session *s, cn_event *out);

/* Local player actions (work for host and guest). */
void cn_set_car(cn_session *s, const uint8_t blob[CN_CAR_BLOB]);
void cn_set_ready(cn_session *s, int ready);
void cn_report_loaded(cn_session *s, uint32_t now);
void cn_report_finish(cn_session *s, uint32_t finish_ms, uint32_t now);
void cn_leave(cn_session *s, uint32_t now);

/* Host only. Returns 0 on success, negative if refused. */
#define CN_ERR_NOT_HOST -1
#define CN_ERR_BAD_PHASE -2
#define CN_ERR_NOT_READY -3
int cn_host_start_event(cn_session *s, uint32_t event_id, uint32_t now);
int cn_host_return_to_lobby(cn_session *s, uint32_t now);

/* Cars. Send ~20-30 times per second during the race. */
void cn_send_car_state(cn_session *s, const cn_car_state *st, uint32_t now);
cn_presence cn_remote_state(cn_session *s, uint8_t player, uint32_t now, cn_car_state *out);
/* How far in the past (ms) the remote car is currently drawn. */
uint32_t cn_remote_delay(const cn_session *s, uint8_t player);

uint32_t cn_host_time(const cn_session *s, uint32_t now);

/* Exposed for tests: finishers by time then DNFs by id. Returns the count. */
int cn_rank_results(const cn_player *players, cn_result *out);

#endif
