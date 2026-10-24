#include "pad_core.h"
#include <string.h>

static const char *const keys[] = {"S1",   "S2",   "S3",   "S4",   "Approve", "Always",
                                   "Deny", "Mode", "Talk", "Stop", "FN"};
static const char *const actions[] = {"select",     "agents_show", "agents_hide",   "create",
                                      "close",      "allow_once",  "allow_session", "deny",
                                      "cycle_mode", "plan",        "voice_start",   "voice_end",
                                      "send",       "interrupt",   "rewind"};
const char *pad_key_name(PadKey k) {
    return k >= 0 && k < PAD_KEY_COUNT ? keys[k] : "?";
}
int pad_key_parse(const char *s) {
    for (int i = 0; i < PAD_KEY_COUNT; ++i)
        if (s && !strcmp(s, keys[i]))
            return i;
    return -1;
}
const char *pad_action_name(PadActionType a) {
    return a >= PAD_SELECT && a <= PAD_REWIND ? actions[a] : "?";
}
void pad_init(PadCore *p, PadEmit emit, void *user) {
    memset(p, 0, sizeof(*p));
    p->selected = 0;
    p->emit = emit;
    p->user = user;
}
void pad_slot(PadCore *p, int s, bool live, uint64_t gen, uint64_t req) {
    if (s >= 0 && s < PAD_SLOTS)
        p->slots[s] = (PadSlotRef){live, gen, req};
}
void pad_select(PadCore *p, int s) {
    if (s >= 0 && s < PAD_SLOTS)
        p->selected = s;
}
static bool valid(PadCore *p, const PadPress *k) {
    PadSlotRef *s = &p->slots[k->slot];
    return k->occupied && s->occupied && s->generation == k->generation;
}
static void emit(PadCore *p, PadPress *k, PadActionType a) {
    PadAction e = {a, k->slot, k->generation, k->request};
    if (p->emit)
        p->emit(p->user, &e);
}
void pad_tick(PadCore *p, uint64_t now) {
    if (now < p->now)
        now = p->now;
    p->now = now;
    for (int i = 0; i < PAD_KEY_COUNT; ++i) {
        PadPress *k = &p->keys[i];
        if (!k->down || k->fired || k->fn || !valid(p, k))
            continue;
        if (i <= PAD_S4 && now - k->since >= PAD_SESSION_HOLD_MS) {
            k->fired = true;
            emit(p, k, PAD_AGENTS_SHOW);
        } else if (i == PAD_TALK && now - k->since >= PAD_TALK_HOLD_MS) {
            k->fired = true;
            emit(p, k, PAD_VOICE_START);
        } else if (i == PAD_ALWAYS && now - k->since >= PAD_ALWAYS_HOLD_MS) {
            /* Bind a hold to the exact request present at key-down. Never let a
             * new prompt inherit an already-held permission key. */
            k->fired = true;
            if (k->request && p->slots[k->slot].request == k->request)
                emit(p, k, PAD_ALLOW_SESSION);
        }
    }
}
void pad_edge(PadCore *p, PadKey key, bool down, uint64_t now) {
    if (key < 0 || key >= PAD_KEY_COUNT)
        return;
    pad_tick(p, now);
    PadPress *k = &p->keys[key];
    if (k->down == down)
        return; /* Repeated HID events are not additional taps. */
    if (down) {
        int s = key <= PAD_S4 ? (int)key : p->selected;
        PadSlotRef ref = p->slots[s];
        *k = (PadPress){.down = true,
                        .fn = p->keys[PAD_FN].down,
                        .slot = s,
                        .occupied = ref.occupied,
                        .since = p->now,
                        .generation = ref.generation,
                        .request = ref.request};
        if (key == PAD_STOP && !k->fn && valid(p, k)) {
            k->fired = true;
            emit(p, k, PAD_INTERRUPT);
        }
        return;
    }
    k->down = false;
    if (key == PAD_FN)
        return;
    if (key <= PAD_S4) {
        if (k->fired) {
            emit(p, k, PAD_AGENTS_HIDE);
            return;
        }
        PadSlotRef s = p->slots[k->slot];
        if (s.generation != k->generation || s.occupied != k->occupied)
            return;
        if (k->fn)
            emit(p, k, s.occupied ? PAD_CLOSE : PAD_CREATE);
        else if (s.occupied)
            emit(p, k, PAD_SELECT);
        return;
    }
    if (key == PAD_TALK && !k->fn && k->occupied) {
        if (k->fired)
            emit(p, k, PAD_VOICE_END);
        else if (valid(p, k))
            emit(p, k, PAD_SEND);
        return;
    }
    if (!valid(p, k))
        return;
    if (k->fn) {
        if (key == PAD_MODE)
            emit(p, k, PAD_PLAN);
        if (key == PAD_STOP)
            emit(p, k, PAD_REWIND);
        return;
    }
    if (key == PAD_APPROVE && k->request && p->slots[k->slot].request == k->request)
        emit(p, k, PAD_ALLOW_ONCE);
    if (key == PAD_DENY && k->request && p->slots[k->slot].request == k->request)
        emit(p, k, PAD_REJECT);
    if (key == PAD_MODE)
        emit(p, k, PAD_CYCLE_MODE);
}
void pad_cancel(PadCore *p, uint64_t now) {
    /* Cancellation is deliberately not a synthetic key-up: it must never send
     * a draft, close a session, or approve a request on disconnect/shutdown. */
    if (now > p->now)
        p->now = now;
    for (int i = 0; i < PAD_KEY_COUNT; ++i) {
        PadPress *k = &p->keys[i];
        if (k->down && i == PAD_TALK && k->fired && !k->fn && k->occupied)
            emit(p, k, PAD_VOICE_END);
        if (k->down && i <= PAD_S4 && k->fired)
            emit(p, k, PAD_AGENTS_HIDE);
    }
    memset(p->keys, 0, sizeof(p->keys));
}
