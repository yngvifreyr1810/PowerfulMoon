// Continuous process-local SDL events must not starve work outside the event drain.
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include "ui/events.h"

static SDL_AtomicInt done;
static SDL_Semaphore *ready;
static Uint32 event_type;
static int SDLCALL produce(void *unused) {
    (void)unused;
    Uint64 end = SDL_GetTicksNS() + 180000000;
    bool first = true;
    do {
        SDL_Event e = {.type = event_type};
        assert(SDL_PushEvent(&e));
        if (first) { SDL_SignalSemaphore(ready); first = false; }
        SDL_Delay(2);
    } while (SDL_GetTicksNS() < end);
    SDL_SetAtomicInt(&done, 1);
    return 0;
}

int main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS));
    event_type = SDL_RegisterEvents(1);
    assert(event_type != (Uint32)-1);
    ready = SDL_CreateSemaphore(0);
    assert(ready);
    for (int bounded = 0; bounded < 2; bounded++) {
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDL_SetAtomicInt(&done, 0);
        SDL_Thread *thread = SDL_CreateThread(produce, "event-fixture", NULL);
        assert(thread);
        SDL_WaitSemaphore(ready);
        Uint64 start = SDL_GetTicksNS(), last = start, max_gap = 0;
        unsigned maintenance = 0;
        do {
            SDL_Event e;
            unsigned count = 0;
            while (bounded ? ui_next_event(&e, &count) : SDL_WaitEventTimeout(&e, 16)) {}
            Uint64 now = SDL_GetTicksNS(), gap = now - last;
            if (gap > max_gap) max_gap = gap;
            last = now;
            maintenance++;
        } while (!SDL_GetAtomicInt(&done));
        SDL_WaitThread(thread, NULL);
        printf("%s: %u maintenance opportunities, longest gap %.1f ms\n",
               bounded ? "bounded batch" : "old wait-per-event loop", maintenance, max_gap / 1e6);
        if (bounded) { assert(maintenance >= 20); assert(max_gap < 100000000); }
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    for (int i = 0; i < 512; i++) { SDL_Event e = {.type = event_type}; assert(SDL_PushEvent(&e)); }
    unsigned count = 0;
    SDL_Event e;
    while (ui_next_event(&e, &count)) {}
    assert(count == 128 && SDL_HasEvent(event_type));
    SDL_DestroySemaphore(ready);
    SDL_Quit();
    puts("PASS: continuous input and queued floods leave maintenance responsive");
    return 0;
}
