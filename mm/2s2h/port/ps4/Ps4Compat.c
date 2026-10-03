// PS4: runtime bits the OpenOrbis C library doesn't provide.

#ifdef __PS4__

#include <pthread.h>
#include <stdlib.h>

// libc++abi forwards the registration of thread_local destructors to the C library.
// This is the usual implementation on top of a pthread key: destructors run when the thread
// exits, and from an atexit handler for the thread that calls exit().

typedef void (*Ps4ThreadDtor)(void*);

typedef struct Ps4ThreadDtorEntry {
    Ps4ThreadDtor destructor;
    void* object;
    struct Ps4ThreadDtorEntry* next;
} Ps4ThreadDtorEntry;

static pthread_key_t sDtorKey;
static pthread_once_t sDtorKeyOnce = PTHREAD_ONCE_INIT;

static void Ps4RunThreadDtors(void* list) {
    Ps4ThreadDtorEntry* entry = (Ps4ThreadDtorEntry*)list;
    while (entry != NULL) {
        Ps4ThreadDtorEntry* next = entry->next;
        entry->destructor(entry->object);
        free(entry);
        entry = next;
    }
}

static void Ps4RunExitingThreadDtors(void) {
    // A destructor may register new ones, keep going until the list stays empty.
    for (int i = 0; i < 8; i++) {
        void* list = pthread_getspecific(sDtorKey);
        if (list == NULL) {
            break;
        }
        pthread_setspecific(sDtorKey, NULL);
        Ps4RunThreadDtors(list);
    }
}

static void Ps4CreateDtorKey(void) {
    pthread_key_create(&sDtorKey, Ps4RunThreadDtors);
    atexit(Ps4RunExitingThreadDtors);
}

int __cxa_thread_atexit_impl(Ps4ThreadDtor destructor, void* object, void* dsoHandle) {
    (void)dsoHandle;

    pthread_once(&sDtorKeyOnce, Ps4CreateDtorKey);

    Ps4ThreadDtorEntry* entry = (Ps4ThreadDtorEntry*)malloc(sizeof(Ps4ThreadDtorEntry));
    if (entry == NULL) {
        return -1;
    }
    entry->destructor = destructor;
    entry->object = object;
    entry->next = (Ps4ThreadDtorEntry*)pthread_getspecific(sDtorKey);
    pthread_setspecific(sDtorKey, entry);
    return 0;
}

#endif // __PS4__
