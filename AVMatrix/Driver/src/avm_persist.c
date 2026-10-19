// AV Matrix — guardado ordenado de ajustes. Ver avm_persist.h.

#include "avm_persist.h"

void avm_persist_init(avm_persist* p, pthread_mutex_t* state_mutex)
{
    p->state = state_mutex;
    pthread_mutex_init(&p->store, NULL);
    p->applied = 0;
    p->stored = 0;
    p->after_apply = NULL;
}

bool avm_persist_commit(avm_persist* p,
                        bool (*apply)(void* obj, const void* value),
                        void (*snapshot)(void* obj, void* out),
                        void (*write)(const void* snap, void* ctx),
                        void* obj, const void* value, void* scratch, void* ctx)
{
    pthread_mutex_lock(p->state);
    bool ok = apply(obj, value);
    if (ok) p->applied++;
    pthread_mutex_unlock(p->state);
    if (!ok) return false;
    if (p->after_apply) p->after_apply(ctx);

    pthread_mutex_lock(&p->store);
    pthread_mutex_lock(p->state);
    snapshot(obj, scratch);          // el estado vigente, no necesariamente `value`
    uint64_t gen = p->applied;
    pthread_mutex_unlock(p->state);
    if (gen > p->stored) {           // un guardado posterior ya pudo escribirlo
        write(scratch, ctx);
        p->stored = gen;
    }
    pthread_mutex_unlock(&p->store);
    return true;
}
