# Condition Variables

Condition variables allow a thread to **atomically release a mutex and sleep**
until another thread signals that a condition has changed. They always pair with
a `mutex_t` — the mutex protects the shared state; the condvar is the
sleep/wake mechanism.

## Files

| File | Purpose |
|---|---|
| `include/kernel/sched/condvar.h` | `condvar_t` struct, API declarations |
| `src/kernel/sched/condvar.c` | `cond_init`, `cond_wait`, `cond_signal`, `cond_broadcast` |

## API

```c
condvar_t cond = CONDVAR_INIT;

void cond_init(condvar_t *cond, const char *name);
void cond_wait(condvar_t *cond, mutex_t *mutex);   // atomic release + sleep
void cond_signal(condvar_t *cond);                 // wake one waiter (FIFO)
void cond_broadcast(condvar_t *cond);              // wake all waiters
```

## cond_wait Semantics

`cond_wait` performs these steps atomically:

1. Enqueue the calling thread in the condvar's wait queue
2. Set thread state to `THREAD_BLOCKED`
3. Call `mutex_unlock(mutex)` — releases the lock before sleeping
4. Call `yield()` — scheduler skips this thread until woken
5. On return: call `mutex_lock(mutex)` — re-acquires before returning

The caller **must hold the mutex** before calling `cond_wait`.

## cond_signal vs cond_broadcast

| Function | Wakes | Use when |
|---|---|---|
| `cond_signal` | 1 waiter (FIFO) | One resource became available |
| `cond_broadcast` | All waiters | State change relevant to all (e.g., shutdown) |

After broadcast all woken threads compete for the mutex — only one proceeds
at a time.

## Starvation & Spurious Wakeups

Always re-check the condition in a `while` loop (not `if`):

```c
while (queue_empty(&q))
    cond_wait(&q->nonempty, &q->lock);
```

This handles both spurious wakeups and the case where another thread
consumes the item between `cond_signal` and the woken thread acquiring the mutex.

## Producer-Consumer Example

```c
/* Producer */
mutex_lock(&q->lock);
enqueue(q, item);
cond_signal(&q->nonempty);
mutex_unlock(&q->lock);

/* Consumer */
mutex_lock(&q->lock);
while (queue_empty(q))
    cond_wait(&q->nonempty, &q->lock);
item = dequeue(q);
mutex_unlock(&q->lock);
```

## Real Usage in Impossible OS

| Site | Condvar | Reason |
|---|---|---|
| Compositor | `vsync_cond` | Wait for VBlank interrupt |
| Shell input | `input_cond` | Wait for keyboard event |
| Pipe | Pairs with `pipe_t` semaphores | Readers wait for data |
