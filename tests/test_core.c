/* Behavior tests for core.c. Build and run: make test */
#include "pad_core.h"
#include <stdio.h>
#include <string.h>

#define MAXLOG 32
static PadAction log_[MAXLOG];
static int n;
static int failures;

static void rec(void *user, const PadAction *a) {
    (void)user;
    if (n < MAXLOG)
        log_[n++] = *a;
}

static void reset(PadCore *p) {
    n = 0;
    pad_init(p, rec, NULL);
}

/* Expect exactly this sequence of action types, in order. */
static void expect(const char *name, int count, const PadActionType *want) {
    int ok = n == count;
    for (int i = 0; ok && i < count; ++i)
        ok = log_[i].type == want[i];
    printf("%s  %s", ok ? "pass" : "FAIL", name);
    if (!ok) {
        failures++;
        printf("   got:");
        for (int i = 0; i < n; ++i)
            printf(" %s", pad_action_name(log_[i].type));
    }
    printf("\n");
}
#define EXPECT(name, ...)                                                            \
    do {                                                                             \
        PadActionType w_[] = {__VA_ARGS__};                                          \
        expect(name, (int)(sizeof w_ / sizeof w_[0]), w_);                           \
    } while (0)
#define EXPECT_NONE(name) expect(name, 0, NULL)

static void tap(PadCore *p, PadKey k, uint64_t t) {
    pad_edge(p, k, true, t);
    pad_edge(p, k, false, t + 40);
}

int main(void) {
    PadCore p;

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    tap(&p, PAD_APPROVE, 0);
    EXPECT("approve answers the waiting request", PAD_ALLOW_ONCE);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    tap(&p, PAD_APPROVE, 0);
    EXPECT_NONE("approve with nothing waiting does nothing");

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    pad_edge(&p, PAD_APPROVE, true, 0);
    pad_slot(&p, 0, true, 1, 78); /* a new prompt replaces the old one mid-press */
    pad_edge(&p, PAD_APPROVE, false, 40);
    EXPECT_NONE("approve held across a new prompt approves neither");

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    tap(&p, PAD_DENY, 0);
    EXPECT("deny rejects the waiting request", PAD_REJECT);

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    pad_edge(&p, PAD_ALWAYS, true, 0);
    pad_tick(&p, 999);
    EXPECT_NONE("always does not fire before 1 s");
    pad_tick(&p, 1000);
    pad_edge(&p, PAD_ALWAYS, false, 1100);
    EXPECT("always fires once after a 1 s hold", PAD_ALLOW_SESSION);

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    tap(&p, PAD_ALWAYS, 0);
    EXPECT_NONE("a quick tap on always is swallowed");

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    pad_edge(&p, PAD_ALWAYS, true, 0);
    pad_slot(&p, 0, true, 1, 78);
    pad_tick(&p, 1200);
    EXPECT_NONE("always held into a new prompt grants nothing");

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    pad_edge(&p, PAD_STOP, true, 0);
    EXPECT("stop interrupts on press, not release", PAD_INTERRUPT);
    pad_edge(&p, PAD_STOP, false, 40);
    EXPECT("stop release adds nothing", PAD_INTERRUPT);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    pad_edge(&p, PAD_FN, true, 0);
    tap(&p, PAD_STOP, 10);
    pad_edge(&p, PAD_FN, false, 100);
    EXPECT("fn + stop rewinds instead of interrupting", PAD_REWIND);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    tap(&p, PAD_MODE, 0);
    pad_edge(&p, PAD_FN, true, 100);
    tap(&p, PAD_MODE, 110);
    pad_edge(&p, PAD_FN, false, 200);
    EXPECT("mode cycles, fn + mode enters plan", PAD_CYCLE_MODE, PAD_PLAN);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    tap(&p, PAD_TALK, 0);
    EXPECT("talk tap sends", PAD_SEND);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    pad_edge(&p, PAD_TALK, true, 0);
    pad_tick(&p, 200);
    pad_edge(&p, PAD_TALK, false, 900);
    EXPECT("talk hold dictates, release keeps the draft", PAD_VOICE_START, PAD_VOICE_END);

    reset(&p);
    pad_slot(&p, 1, true, 5, 0);
    tap(&p, PAD_S2, 0);
    EXPECT("S2 tap selects an occupied slot", PAD_SELECT);

    reset(&p);
    tap(&p, PAD_S3, 0);
    EXPECT_NONE("S3 tap on an empty slot does nothing");

    reset(&p);
    pad_edge(&p, PAD_FN, true, 0);
    tap(&p, PAD_S3, 10);
    EXPECT("fn + empty slot creates a session", PAD_CREATE);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    pad_edge(&p, PAD_S1, true, 0);
    pad_tick(&p, 500);
    pad_edge(&p, PAD_S1, false, 800);
    EXPECT("S1 hold peeks at agents, release hides", PAD_AGENTS_SHOW, PAD_AGENTS_HIDE);

    reset(&p);
    pad_slot(&p, 0, true, 1, 77);
    pad_edge(&p, PAD_TALK, true, 0);
    pad_tick(&p, 300);
    pad_edge(&p, PAD_APPROVE, true, 310);
    pad_cancel(&p, 320);
    EXPECT("disconnect ends dictation and approves nothing", PAD_VOICE_START, PAD_VOICE_END);

    reset(&p);
    pad_slot(&p, 0, true, 1, 0);
    pad_edge(&p, PAD_MODE, true, 0);
    pad_edge(&p, PAD_MODE, true, 10); /* repeated down edge */
    pad_edge(&p, PAD_MODE, false, 40);
    EXPECT("a repeated press edge is still one tap", PAD_CYCLE_MODE);

    printf("\n%s (%d failed)\n", failures ? "FAILURES" : "all passed", failures);
    return failures != 0;
}
