# Java Runtime Support — Research

## Overview

Java programs run on the **Java Virtual Machine (JVM)** — a bytecode interpreter/JIT compiler that provides platform independence. Supporting Java on Impossible OS means either porting a JVM or implementing a minimal one.

---

## Java Ecosystem

| Component | What it is |
|-----------|-----------|
| **JVM** | Virtual machine that executes `.class` bytecode |
| **JRE** | JVM + standard class library (rt.jar / modules) |
| **JDK** | JRE + compiler (`javac`), tools |
| **JAR** | ZIP archive of `.class` files + manifest |
| **Java bytecode** | Stack-based instruction set, ~200 opcodes |

---

## Open-Source JVM Implementations

| JVM | License | Redistributable | Size | Notes |
|-----|---------|-----------------|------|-------|
| **OpenJDK** | GPL 2.0 + Classpath Exception | ✅ Yes | Millions of lines | Full production JVM, HotSpot JIT |
| **GraalVM** | GPL 2.0 + CE | ✅ Yes (CE edition) | Very large | Polyglot, ahead-of-time compilation |
| **Eclipse OpenJ9** | EPL 2.0 + Apache 2.0 | ✅ Yes | Large | IBM's JVM, lower memory usage |
| **Avian** | ISC License | ✅ Yes | ~30K lines of C++ | Lightweight, embeddable, JIT + AOT |
| **JamVM** | GPL 2.0 | ✅ Yes | ~15K lines of C | Interpreter only, very portable |
| **MiniJVM** | Apache 2.0 | ✅ Yes | ~5K lines of C | Minimal interpreter, for embedded |

---

## Approaches for Impossible OS

### Approach A: Port a minimal JVM interpreter

**Target: JamVM or MiniJVM**

```
Java source → javac (on host) → .class bytecode → ship in initrd → JVM interprets
```

| Aspect | Details |
|--------|---------|
| **Effort** | 4-8 weeks (port JamVM's core interpreter) |
| **Prerequisites** | `mmap()`, file I/O, threading (for full Java), or single-threaded subset |
| **Coverage** | Basic Java: classes, methods, strings, arrays, exceptions |
| **Missing** | AWT/Swing (GUI), networking, reflection (advanced) |

### Approach B: Ahead-of-time compilation (GraalVM Native Image)

```
Java source → GraalVM native-image → ELF/PE binary → runs natively on Impossible OS
```

| Aspect | Details |
|--------|---------|
| **Effort** | Minimal (binary runs like any other program) |
| **Prerequisites** | Working ELF/PE loader + enough syscalls |
| **Coverage** | Full Java (minus runtime class loading) |
| **Limitation** | Compiled on host, not on Impossible OS itself |

### Approach C: Implement a mini-JVM from scratch

Write a minimal Java bytecode interpreter (educational purpose):

| Aspect | Details |
|--------|---------|
| **Effort** | 2-4 weeks for basics |
| **Coverage** | Simple programs: arithmetic, loops, strings, System.out.println |
| **Size** | ~2000-4000 lines of C |

```c
/* Minimal JVM interpreter loop */
while (pc < code_length) {
    uint8_t opcode = code[pc++];
    switch (opcode) {
        case 0x00: break;                          /* nop */
        case 0x03: stack_push(0); break;            /* iconst_0 */
        case 0x04: stack_push(1); break;            /* iconst_1 */
        case 0x60: { int b=pop(); int a=pop(); push(a+b); } break; /* iadd */
        case 0xB1: return;                          /* return */
        case 0xB2: /* getstatic */ ...              /* System.out */
        case 0xB6: /* invokevirtual */ ...          /* println */
    }
}
```

> [!TIP]
> **Recommended: Approach B first** (GraalVM native images for practical Java), **Approach C later** (mini-JVM for the cool factor of running `.class` files directly).

---

## Java Licensing

| Component | License | Redistributable | Notes |
|-----------|---------|-----------------|-------|
| Java SE specification | Free to implement | ✅ | Anyone can build a JVM |
| OpenJDK | GPL 2.0 + Classpath Exception | ✅ Yes | The Classpath Exception allows linking with proprietary code |
| JamVM | GPL 2.0 | ✅ Yes | Must provide source if distributed |
| MiniJVM | Apache 2.0 | ✅ Yes | Very permissive |
| Avian | ISC | ✅ Yes | Most permissive JVM license |
| GraalVM CE | GPL 2.0 | ✅ Yes | Community edition is fully open |

> [!NOTE]
> The **Classpath Exception** in OpenJDK's license is important — it means programs *running on* the JVM don't have to be GPL. Only modifications to the JVM itself must be open-sourced.

---

## Prerequisites for Impossible OS

| Requirement | Needed for | Status |
|-------------|-----------|--------|
| File I/O (read `.class` / `.jar`) | All approaches | ✅ Basic VFS exists |
| `mmap()` / heap management | JVM memory | ❌ Not yet |
| Threading | Full Java (`java.lang.Thread`) | ❌ Not yet |
| Networking (sockets) | `java.net.*` | ❌ Not yet |
| Console I/O | `System.out.println` | ✅ SYS_WRITE exists |
| ZIP/JAR parsing | Loading `.jar` files | ❌ Need miniz or similar |
