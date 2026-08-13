/*
 * vault_model.h — everything the board knows about the vault, in one struct.
 *
 * This is the seam of the whole project. `vault_t` is produced by exactly two
 * things — vault_parse.c (from the JSON a PC serves) and vault_mock.c (the
 * built-in demo snapshot) — and consumed by exactly two — the UI pages and the
 * companion-app JSON. Nothing else reads the network payload, so a change to
 * the wire format lands in one file.
 *
 * Every array is fixed-size and every count is clamped by the producer. The
 * struct is therefore bounded, copyable, and safe to snapshot under a mutex and
 * hand to the UI task without any ownership question. That is deliberate: on a
 * device where one task owns the panel and another owns the network, a plain
 * copyable value is worth more than the bytes it wastes.
 *
 * Portable: no LVGL, no ESP-IDF. The host tests build this directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- capacities -----------------------------------------------------------
 * These are display capacities, not protocol limits: the panel is 648x480 and
 * fits six agent rows, eight note rows and about fourteen graph nodes before
 * anything becomes unreadable. The parser drops the overflow rather than
 * failing, so a vault with 500 agents still renders its first six. */
#define VAULT_NAME_MAX      32
#define VAULT_TIME_MAX      24
#define VAULT_TITLE_MAX     64      /* bytes of UTF-8, ~21 Hangul syllables */
#define VAULT_TAG_NAME_MAX  24
#define VAULT_AGENT_NAME_MAX 20
#define VAULT_AGENT_NOTE_MAX 72

#define VAULT_DAILY_DAYS    7
#define VAULT_TAGS_MAX      6
#define VAULT_AGENTS_MAX    6
#define VAULT_NODES_MAX     14
#define VAULT_EDGES_MAX     32
#define VAULT_RECENT_MAX    8
#define VAULT_INBOX_MAX     8

#define ARTWORK_HEADWORD_MAX     64
#define ARTWORK_META_MAX         32
#define ARTWORK_PATH_MAX        128
#define ARTWORK_LINE_MAX        128

#define ARTWORK_HEADLINE_MAX      2
#define ARTWORK_DEFINITION_MAX    2
#define ARTWORK_BACKLINKS_MAX     3
#define ARTWORK_NODES_MAX         6
#define ARTWORK_EDGES_MAX         8

#define TAROT_DATE_MAX           11
#define TAROT_TIMEZONE_MAX       16
#define TAROT_CARD_ID_MAX        16
#define TAROT_ORIENTATION_MAX     9
#define TAROT_LINE_MAX          128
#define TAROT_LINES_MAX           2

typedef struct {
    char lines[TAROT_LINES_MAX][TAROT_LINE_MAX];
    int line_count;
} tarot_lines_t;

typedef struct {
    bool valid;
    char date[TAROT_DATE_MAX];
    char timezone[TAROT_TIMEZONE_MAX];
    char card_id[TAROT_CARD_ID_MAX];
    char orientation[TAROT_ORIENTATION_MAX];
    int copy_version;
    tarot_lines_t headline;
    tarot_lines_t flow;
    tarot_lines_t caution;
    tarot_lines_t action;
} daily_tarot_t;

typedef struct {
    char text[ARTWORK_LINE_MAX];
} artwork_line_t;

typedef struct {
    char headword[ARTWORK_HEADWORD_MAX];
    char meta[ARTWORK_META_MAX];
    artwork_line_t lines[ARTWORK_DEFINITION_MAX];
    int line_count;
} artwork_definition_t;

typedef struct {
    char title[VAULT_TITLE_MAX];
    char path[ARTWORK_PATH_MAX];
    int backlink_total;
    char backlinks[ARTWORK_BACKLINKS_MAX][VAULT_TITLE_MAX];
    int backlink_count;
} artwork_note_t;

typedef struct {
    char title[VAULT_TITLE_MAX];
    uint8_t slot;
} artwork_node_t;

typedef struct {
    uint8_t a, b;
} artwork_edge_t;

typedef struct {
    bool valid;
    artwork_line_t headline[ARTWORK_HEADLINE_MAX];
    int headline_count;
    artwork_definition_t definition;
    artwork_note_t note;
    artwork_node_t nodes[ARTWORK_NODES_MAX];
    int node_count;
    artwork_edge_t edges[ARTWORK_EDGES_MAX];
    int edge_count;
} vault_artwork_t;

/* --- pieces --------------------------------------------------------------- */

typedef enum {
    AGENT_IDLE = 0,
    AGENT_RUNNING,
    AGENT_ERROR,
    AGENT_DONE,
    AGENT_STATE_COUNT,
} agent_state_t;

typedef struct {
    char name[VAULT_TAG_NAME_MAX];
    int  count;
} vault_tag_t;

typedef struct {
    char          name[VAULT_AGENT_NAME_MAX];
    agent_state_t state;
    char          last_run[VAULT_TIME_MAX];   /* free text: "20:55", "2h ago" */
    int           processed;
    int           queued;
    int           progress;                   /* 0..100, or -1 for "no bar" */
    char          note[VAULT_AGENT_NOTE_MAX]; /* what it is doing right now */
} vault_agent_t;

typedef struct {
    char title[VAULT_TITLE_MAX];
    int  deg;                                 /* link degree — drives node size */
} vault_node_t;

/* Indices into vault_t.nodes. uint8_t is enough for VAULT_NODES_MAX and keeps
 * the edge list at two bytes a pair. */
typedef struct {
    uint8_t a, b;
} vault_edge_t;

typedef struct {
    char title[VAULT_TITLE_MAX];
    char time[VAULT_TIME_MAX];
    int  links;
} vault_note_t;

typedef struct {
    char title[VAULT_TITLE_MAX];
    int  age_days;
} vault_inbox_t;

typedef struct {
    int notes;
    int links;
    int orphans;
    int tags;
    int added_today;
    int added_7d;
    int daily[VAULT_DAILY_DAYS];              /* oldest first, today last */
} vault_stats_t;

/* --- the snapshot --------------------------------------------------------- */

typedef struct {
    bool valid;                 /* false = nothing has ever been loaded */
    bool demo;                  /* true = vault_mock, no vault_url configured */

    char vault[VAULT_NAME_MAX];
    char generated_at[VAULT_TIME_MAX];

    vault_stats_t stats;

    vault_tag_t   tags[VAULT_TAGS_MAX];
    int           tag_count;

    vault_agent_t agents[VAULT_AGENTS_MAX];
    int           agent_count;

    vault_node_t  nodes[VAULT_NODES_MAX];
    int           node_count;
    vault_edge_t  edges[VAULT_EDGES_MAX];
    int           edge_count;

    vault_note_t  recent[VAULT_RECENT_MAX];
    int           recent_count;

    vault_inbox_t inbox[VAULT_INBOX_MAX];
    int           inbox_count;
    int           inbox_total;  /* server's real count; >= inbox_count */

    vault_artwork_t artwork;
    daily_tarot_t daily_tarot;
} vault_t;

/* --- helpers (pure, shared by the UI, the API and the tests) -------------- */

/* Copy a UTF-8 string into a fixed buffer, truncating on a character boundary.
 *
 * strlcpy would happily cut a 3-byte Hangul syllable in half, and a lone
 * continuation byte does not render as "the title was long" — it renders as a
 * tofu box, or worse, sends LVGL's decoder past the NUL. Every string that
 * enters vault_t from the network goes through here. Always NUL-terminates.
 * Returns the number of bytes written (excluding the NUL). */
size_t vault_str_copy(char *dst, size_t dst_size, const char *src);

/* "RUNNING" / "IDLE" / "ERROR" / "DONE" — never NULL, even for a bad enum. */
const char *vault_agent_state_name(agent_state_t s);

/* Parse a wire state word ("running", "idle", ...). Unknown -> AGENT_IDLE. */
agent_state_t vault_agent_state_from(const char *word);

/* How many agents are in AGENT_RUNNING. */
int vault_running_agents(const vault_t *v);

/* Links per note, x100 (2.74 links/note -> 274). 0 when there are no notes. */
int vault_link_density_x100(const vault_t *v);

/* Orphan share of all notes, x10 (2.6% -> 26). 0 when there are no notes. */
int vault_orphan_rate_x10(const vault_t *v);

/* Largest value in stats.daily[], floored at 1 so a bar chart can divide by it. */
int vault_daily_peak(const vault_t *v);

/* A fingerprint of everything that is drawn. Two snapshots with the same
 * fingerprint produce the same pixels, so the caller can skip a panel refresh
 * entirely — which on e-Paper is the difference between a silent board and one
 * that flashes every five minutes for no reason.
 *
 * Deliberately excludes nothing that reaches the glass, including `demo`. */
uint32_t vault_hash(const vault_t *v);

/* Fingerprint only fields consumed by ui_artwork.c. Pollers use this to avoid
 * flashing the panel when timestamps or legacy dashboard aggregates change
 * but the fixed artwork would produce identical pixels. */
uint32_t vault_artwork_hash(const vault_t *v);

/* Fingerprint only the daily tarot fields that reach the new tarot canvas. */
uint32_t vault_tarot_hash(const vault_t *v);

#ifdef __cplusplus
}
#endif
