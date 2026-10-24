#ifndef PAD_CORE_H
#define PAD_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Portable C11 keypad interface; link with src/core.c.
 *
 * Feed debounced button edges and call pad_tick() at least every 10 ms.
 * Use one event loop and a monotonic millisecond clock for all calls.
 * Actions go to the callback; the Mac controller performs the Claude actions.
 *
 * Controls: S1-S4 select / hold to peek at agents / FN to create or close.
 * Approve and Deny answer the current request; Always requires a 1 s hold.
 * Mode cycles permissions; FN + Mode enters Plan.
 * Hold Talk to dictate, release to keep the draft, tap to send.
 * Stop interrupts; FN + Stop rewinds code and conversation.
 */
enum {
    PAD_SLOTS = 4,
    PAD_TALK_HOLD_MS = 180,
    PAD_SESSION_HOLD_MS = 500,
    PAD_ALWAYS_HOLD_MS = 1000
};

typedef enum {
    PAD_S1,
    PAD_S2,
    PAD_S3,
    PAD_S4,
    PAD_APPROVE,
    PAD_ALWAYS,
    PAD_DENY,
    PAD_MODE,
    PAD_TALK,
    PAD_STOP,
    PAD_FN,
    PAD_KEY_COUNT
} PadKey;

typedef enum {
    PAD_SELECT,
    PAD_AGENTS_SHOW,
    PAD_AGENTS_HIDE,
    PAD_CREATE,
    PAD_CLOSE,
    PAD_ALLOW_ONCE,
    PAD_ALLOW_SESSION,
    PAD_REJECT,
    PAD_CYCLE_MODE,
    PAD_PLAN,
    PAD_VOICE_START,
    PAD_VOICE_END,
    PAD_SEND,
    PAD_INTERRUPT,
    PAD_REWIND
} PadActionType;

/* Slots are zero-based. Captured identity prevents a held key from acting on
 * a replacement session or a different permission request. */
typedef struct {
    PadActionType type;
    int slot;
    uint64_t generation, request;
} PadAction;

/* Called synchronously. Copy the action if retaining it after the callback. */
typedef void (*PadEmit)(void *user, const PadAction *action);

typedef struct {
    bool occupied;
    uint64_t generation, request;
} PadSlotRef;

typedef struct {
    bool down, fn, fired, occupied;
    int slot;
    uint64_t since, generation, request;
} PadPress;

/* Caller-owned state; no allocation or platform libraries required. */
typedef struct {
    int selected;
    uint64_t now;
    PadSlotRef slots[PAD_SLOTS];
    PadPress keys[PAD_KEY_COUNT];
    PadEmit emit;
    void *user;
} PadCore;

/* Initialize empty slots with S1 selected. */
void pad_init(PadCore *pad, PadEmit emit, void *user);

/* Publish current session identity. Change generation whenever a session is
 * replaced; request is the current permission ID, or zero when none waits. */
void pad_slot(PadCore *pad, int slot, bool occupied, uint64_t generation, uint64_t request);

/* Confirm the selected slot after the controller handles PAD_SELECT. */
void pad_select(PadCore *pad, int slot);

/* down=true is a press; down=false is a release. Feed FN first when several
 * buttons change together. Repeated edges do not create extra taps. */
void pad_edge(PadCore *pad, PadKey key, bool down, uint64_t now_ms);

/* Recognize holds while buttons remain pressed. */
void pad_tick(PadCore *pad, uint64_t now_ms);

/* Call on disconnect or reset: end recording/hide agent peeks and clear held
 * keys without sending a draft, approving a request, or closing a session. */
void pad_cancel(PadCore *pad, uint64_t now_ms);

const char *pad_key_name(PadKey key);
/* Parse the exact key name (for example "S1" or "Talk"); return -1 if unknown. */
int pad_key_parse(const char *name);
const char *pad_action_name(PadActionType type);

#endif /* PAD_CORE_H */
