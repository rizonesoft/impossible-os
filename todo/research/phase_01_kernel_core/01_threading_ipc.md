# 01 — Threading & IPC

## Overview

Multi-threading allows apps to do background work (loading files, network requests) without freezing the UI. Inter-process communication (IPC) lets processes talk to each other (Terminal ↔ Shell, apps ↔ services).

**This is the #1 priority** — Terminal, clipboard, and all real apps depend on it.

---

## Current State

| Feature | Status |
|---------|--------|
| Process scheduler | ✅ Round-robin, `task.c` |
| Process creation (`exec`) | ✅ Basic ELF loading |
| Threads within a process | ❌ Not implemented |
| Mutexes / spinlocks | ❌ Kernel spinlock only |
| Semaphores | ❌ |
| Pipes | ❌ |
| Shared memory | ❌ |
| Message passing | ❌ |
| Signals | ❌ |

---

## Part 1: Kernel Threads

```c
/* thread.h */
typedef struct thread {
    uint64_t      id;
    uint64_t      stack_ptr;       /* saved RSP */
    uint64_t      stack_base;      /* stack allocation base */
    uint32_t      stack_size;
    uint8_t       state;           /* RUNNING, READY, BLOCKED, DEAD */
    struct task  *parent_task;     /* owning process */
    struct thread *next;           /* linked list within task */
} thread_t;

thread_t *thread_create(void (*entry)(void *), void *arg, uint32_t stack_size);
void thread_exit(int status);
void thread_join(thread_t *t);
void thread_yield(void);
```

## Part 2: Synchronization Primitives

```c
/* mutex.h */
typedef struct { volatile uint32_t locked; thread_t *owner; } mutex_t;
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* semaphore.h */
typedef struct { volatile int32_t count; } semaphore_t;
void sem_wait(semaphore_t *s);
void sem_signal(semaphore_t *s);
```

## Part 3: Pipes

```c
/* pipe.h — unidirectional byte stream */
typedef struct {
    uint8_t  buf[4096];
    uint32_t read_pos, write_pos;
    uint32_t count;
    mutex_t  lock;
    semaphore_t can_read, can_write;
} pipe_t;

int pipe_create(int fds[2]);   /* fds[0] = read end, fds[1] = write end */
int pipe_write(pipe_t *p, const void *data, uint32_t len);
int pipe_read(pipe_t *p, void *buf, uint32_t len);
```

## Part 4: Signals

```c
#define SIGKILL   9
#define SIGTERM  15
#define SIGINT    2
#define SIGCHLD  17

int signal_send(uint64_t pid, int sig);
void signal_handler(int sig, void (*handler)(int));
```

## Part 5: Shared Memory

```c
int shmem_create(const char *name, uint32_t size);  /* returns shmem id */
void *shmem_map(int id);                             /* map into address space */
void shmem_unmap(int id);
```

---

## Syscalls

| Syscall | Number | Purpose |
|---------|--------|---------|
| SYS_THREAD_CREATE | 30 | Create new thread |
| SYS_THREAD_EXIT | 31 | Exit current thread |
| SYS_THREAD_JOIN | 32 | Wait for thread |
| SYS_PIPE | 33 | Create pipe pair |
| SYS_SIGNAL | 34 | Send signal to process |
| SYS_SHMEM_CREATE | 35 | Create shared memory |
| SYS_SHMEM_MAP | 36 | Map shared memory |

---

## Implementation: ~800-1200 lines, 2-3 weeks
