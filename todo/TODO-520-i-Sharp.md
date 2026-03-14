# P0106 — I# (I-Sharp) Programming Language

> **Goal:** Create a custom programming language for Impossible OS with C#-like
> syntax, a native compiler that produces Impossible OS executables, and a
> standard library that wraps the IxUI toolkit and kernel syscalls. I# is
> the first-class language for writing Impossible OS applications.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!IMPORTANT]
> **Prerequisite:** Native ABI (P0105 §2) and C library (P0105 §3) must be
> working. The I# compiler (`isc`) initially runs as a cross-compiler on the
> build host, then self-hosts on Impossible OS once the OS is mature enough.

**Design Philosophy:**
- **C# syntax** — familiar to millions of developers (classes, properties, `using`, `async/await`, LINQ-style queries)
- **Compiled to native x86-64** — no VM, no garbage collector (initially), direct syscalls
- **Memory model:** manual with RAII-like `using` blocks (like C# `IDisposable` but enforced at compile time, similar to Rust's ownership)
- **No .NET dependency** — this is a fresh language that borrows C# *syntax*, not the runtime
- **File extension:** `.is` (I-Sharp)
- **Compiler:** `isc` (I-Sharp Compiler)

---

## 1. Language Design

### 1.1 Language Specification

**Prompt:** Write the I# language specification. Core syntax mirrors C#: `class`, `struct`, `enum`, `interface`, `namespace`, `using`, `public`/`private`/`protected`, `static`, properties with `get`/`set`, `for`/`foreach`/`while`/`if`/`switch`, `try`/`catch`/`finally`, string interpolation (`$"Hello {name}"`), null-conditional (`?.`), pattern matching (`is`, `switch` expressions). Key differences from C#: no garbage collector (use `using` blocks for deterministic cleanup), no reflection, no JIT — compiled directly to native code. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# specification"`.


- [ ] Document core types: `int`, `long`, `float`, `double`, `bool`, `char`, `string`, `byte`
- [ ] Document classes: `class Foo { }`, constructors, destructors (`~Foo()`), inheritance
- [ ] Document structs: value types, stack-allocated
- [ ] Document interfaces: `interface IDrawable { void Draw(); }`
- [ ] Document enums: `enum Color { Red, Green, Blue }`
- [ ] Document namespaces and `using` directives
- [ ] Document access modifiers: `public`, `private`, `protected`, `internal`
- [ ] Document properties: `public int X { get; set; }`
- [ ] Document generics: `List<T>`, `Dictionary<K,V>`, generic constraints
- [ ] Document null safety: nullable types (`int?`), null-conditional (`?.`), null-coalescing (`??`)
- [ ] Document string interpolation: `$"Hello {name}, you are {age} years old"`
- [ ] Document pattern matching: `if (obj is string s)`, `switch` expressions
- [ ] Document `using` blocks for deterministic resource cleanup
- [ ] Document `async`/`await` (maps to kernel task scheduling)
- [ ] Document operator overloading
- [ ] Create `docs/architecture/isharp-spec.md`
- [ ] Commit: `"lang: I# specification"`

---

## 2. Compiler — Lexer and Parser

### 2.1 Lexer (Tokenizer)

**Prompt:** Implement the I# lexer in C. The lexer reads source `.is` files and produces a stream of tokens: keywords (`class`, `if`, `return`, `using`, `namespace`, `public`, `private`, `static`, `void`, `int`, `string`, `bool`, `new`, `null`, `true`, `false`, `async`, `await`), identifiers, integer/float literals, string literals (including interpolated strings), operators (`+`, `-`, `*`, `/`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `!`, `?.`, `??`, `=>`, `++`, `--`), punctuation (`{`, `}`, `(`, `)`, `[`, `]`, `;`, `,`, `.`, `:`), and comments (`//` line, `/* */` block). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# lexer"`.


- [ ] Create `tools/isc/lexer.c` and `tools/isc/lexer.h`
- [ ] Token types: keywords, identifiers, literals, operators, punctuation
- [ ] C# keyword set (~80 keywords)
- [ ] String literal parsing (regular, interpolated `$"..."`, verbatim `@"..."`)
- [ ] Number literal parsing (int, float, hex `0x`, binary `0b`)
- [ ] Comment handling: `//` line, `/* */` block, `///` doc comments
- [ ] Track source location (file, line, column) for error messages
- [ ] Test: tokenize a sample `.is` file → correct token stream
- [ ] Commit: `"lang: I# lexer"`

### 2.2 Parser (AST Generation)

**Prompt:** Implement the I# parser. Takes the token stream from the lexer and produces an Abstract Syntax Tree (AST). Use recursive descent parsing. AST node types: `CompilationUnit`, `NamespaceDecl`, `ClassDecl`, `StructDecl`, `EnumDecl`, `InterfaceDecl`, `MethodDecl`, `PropertyDecl`, `FieldDecl`, `ConstructorDecl`, `ParameterList`, `Block`, `IfStmt`, `ForStmt`, `WhileStmt`, `ReturnStmt`, `ExprStmt`, `VarDecl`, `BinaryExpr`, `UnaryExpr`, `CallExpr`, `MemberAccessExpr`, `NewExpr`, `CastExpr`, `AssignExpr`, `LiteralExpr`, `IdentifierExpr`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# parser"`.


- [ ] Create `tools/isc/parser.c` and `tools/isc/parser.h`
- [ ] Create `tools/isc/ast.h` — AST node type definitions
- [ ] Parse top-level: `using`, `namespace`, class/struct/enum/interface declarations
- [ ] Parse class members: methods, properties, fields, constructors
- [ ] Parse statements: `if/else`, `for`, `foreach`, `while`, `switch`, `return`, `using`, `try/catch`
- [ ] Parse expressions: binary ops (precedence climbing), unary, ternary, lambda (`=>`)
- [ ] Parse generics: `<T>`, `<K,V>`, constraints (`where T : IComparable`)
- [ ] Error recovery: skip to next `;` or `}` on parse error, continue parsing
- [ ] Test: parse `HelloWorld.is` → valid AST
- [ ] Commit: `"lang: I# parser"`

---

## 3. Compiler — Semantic Analysis

### 3.1 Type Checker and Name Resolution

**Prompt:** Implement semantic analysis for I#. Build a symbol table, resolve all names (classes, methods, variables, types), perform type checking, and enforce access modifiers. Check: method return types match declarations, binary operations have compatible types, assignments are type-compatible, constructors exist, interface implementations are complete. Report clear error messages with file/line/column. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# type checker"`.


- [ ] Create `tools/isc/sema.c` and `tools/isc/sema.h`
- [ ] Build symbol table: scopes, namespaces, classes, methods, locals
- [ ] Name resolution: resolve identifiers to declarations
- [ ] Type checking: verify all expressions have valid types
- [ ] Implicit conversions: `int` → `long`, `int` → `double`
- [ ] Method overload resolution
- [ ] Access modifier enforcement (`private` members not visible outside class)
- [ ] Interface implementation validation
- [ ] Generic type instantiation
- [ ] Error messages with source location: `Program.is(12,5): error: type 'int' has no method 'Foo'`
- [ ] Commit: `"lang: I# type checker"`

---

## 4. Compiler — Code Generation

### 4.1 x86-64 Code Generator

**Prompt:** Implement the x86-64 code generator for I#. Takes the type-checked AST and emits x86-64 assembly (NASM syntax) or machine code directly. Use the Impossible OS native ABI (P0105 §2) for syscalls. Calling convention: System V AMD64 (rdi, rsi, rdx, rcx, r8, r9, return in rax). Generate: function prologues/epilogues, stack frame management, register allocation (linear scan or simple graph coloring), arithmetic operations, comparisons and branches, function calls, string literals in `.rodata`, class vtables for virtual methods. Output: ELF binary or flat binary that runs on Impossible OS. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# x86-64 code generator"`.


- [ ] Create `tools/isc/codegen.c` and `tools/isc/codegen.h`
- [ ] Function prologue/epilogue (stack frame, callee-saved registers)
- [ ] Register allocator (linear scan)
- [ ] Arithmetic: `+`, `-`, `*`, `/`, `%` → x86 instructions
- [ ] Comparisons: `==`, `!=`, `<`, `>` → `cmp` + conditional jumps
- [ ] Control flow: `if/else` → conditional branches, `for/while` → loops
- [ ] Function calls: System V AMD64 calling convention
- [ ] String literals → `.rodata` section
- [ ] Class layout: field offsets, vtable pointer
- [ ] Virtual method dispatch via vtable
- [ ] Object allocation: `new Foo()` → allocate memory + call constructor
- [ ] Output ELF64 binary for Impossible OS
- [ ] Commit: `"lang: I# x86-64 code generator"`

### 4.2 Linker Integration

**Prompt:** Link I#-compiled object files with the I# standard library and system libraries. Support multi-file compilation: `isc Program.is Utils.is -o app.exe`. Resolve cross-file references. Link against `libis-std.a` (standard library) and `libixui.a` (GUI toolkit). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# linker integration"`.


- [ ] Multi-file compilation: compile each `.is` to `.o`, link together
- [ ] Assembly references: `using MyLib;` → find and link `MyLib.o`
- [ ] Auto-link standard library (`libis-std.a`)
- [ ] Auto-link IxUI (`libixui.a`) when GUI types are used
- [ ] Output: native Impossible OS executable
- [ ] Commit: `"lang: I# linker integration"`

---

## 5. Standard Library (I# Standard)

### 5.1 Core Types

**Prompt:** Implement the I# standard library. Core types: `System.String` (UTF-8, length-prefixed, immutable), `System.Console` (Read, Write, WriteLine → syscalls), `System.Math` (Abs, Min, Max, Sqrt, Sin, Cos), `System.IO.File` (ReadAllText, WriteAllText, Exists, Delete → VFS syscalls), `System.IO.Path` (Combine, GetExtension, GetFileName). Collections: `System.Collections.List<T>` (dynamic array), `System.Collections.Dictionary<K,V>` (hash map). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# standard library"`.


- [ ] `System.String` — UTF-8, length-prefixed, concat, substring, contains, split, trim
- [ ] `System.Console` — `Write()`, `WriteLine()`, `ReadLine()`, `ReadKey()`
- [ ] `System.Convert` — `ToInt32()`, `ToString()`, `ToDouble()`
- [ ] `System.Math` — `Abs`, `Min`, `Max`, `Sqrt`, `Sin`, `Cos`, `Floor`, `Ceiling`
- [ ] `System.IO.File` — `ReadAllText`, `WriteAllText`, `Exists`, `Delete`, `Copy`
- [ ] `System.IO.Path` — `Combine`, `GetExtension`, `GetFileName`, `GetDirectory`
- [ ] `System.Collections.List<T>` — dynamic array with `Add`, `Remove`, `Count`, indexer
- [ ] `System.Collections.Dictionary<K,V>` — hash map with `Add`, `Remove`, `ContainsKey`, indexer
- [ ] Commit: `"lang: I# standard library"`

### 5.2 GUI Bindings

**Prompt:** Create I# wrappers for the IxUI toolkit so GUI apps can be written in I# with C#-like syntax. Example usage: `var window = new Window("My App", 800, 600); window.OnPaint += (g) => { g.DrawText("Hello!", 10, 10, 24); }; Application.Run(window);`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# GUI bindings"`.


- [ ] `IxUI.Window` — wraps `ix_create_window`, properties for title/size/position
- [ ] `IxUI.Graphics` — `DrawText`, `DrawRect`, `DrawImage`, `FillRect`
- [ ] `IxUI.Button` — wraps button control, `Click` event
- [ ] `IxUI.TextBox` — wraps text input control
- [ ] `IxUI.Label` — wraps label control
- [ ] `IxUI.MessageBox` — wraps `MessageBox()` (P0104)
- [ ] `IxUI.Application` — `Run(window)` starts event loop
- [ ] Event system: `onclick += handler` syntax
- [ ] Test: I# GUI app → window with button, click shows MessageBox
- [ ] Commit: `"lang: I# GUI bindings"`

---

## 6. Compiler Driver and Toolchain

### 6.1 `isc` Compiler Driver

**Prompt:** Create the `isc` (I-Sharp Compiler) driver that orchestrates the full compilation pipeline. Usage: `isc Program.is -o app.exe` (compile and link), `isc -c Lib.is` (compile to object file), `isc -run Script.is` (compile and run immediately), `isc --emit-asm Program.is` (output assembly for inspection). Install to `C:\Impossible\Bin\isc.exe`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: isc compiler driver"`.


- [ ] Create `tools/isc/main.c` — command-line driver
- [ ] `isc hello.is -o hello.exe` — compile, link, output executable
- [ ] `isc -c lib.is` — compile to `.o` object file
- [ ] `isc -run script.is` — compile to temp, execute, delete
- [ ] `isc --emit-asm prog.is` — output `.asm` for debugging
- [ ] `isc --check prog.is` — type check only, no codegen
- [ ] Error output: colored, with source context (like C# compiler)
- [ ] Install to `C:\Impossible\Bin\isc.exe`
- [ ] Commit: `"lang: isc compiler driver"`

### 6.2 Self-Hosting *(Long-term)*

**Prompt:** Rewrite the I# compiler in I# itself (bootstrapping). The compiler is initially written in C, cross-compiled. Once I# is expressive enough, rewrite the lexer, parser, type checker, and code generator in I#. The self-hosted compiler must produce identical output to the C compiler for a test suite. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: isc self-hosting"`.


- [ ] Rewrite lexer in I#
- [ ] Rewrite parser in I#
- [ ] Rewrite type checker in I#
- [ ] Rewrite code generator in I#
- [ ] Bootstrap test: `isc` (C) compiles `isc.is` → `isc2.exe`, `isc2.exe` compiles `isc.is` → `isc3.exe`, `isc2 == isc3`
- [ ] Commit: `"lang: isc self-hosting"`

---

## 7. Testing and Documentation

### 7.1 Test Suite

**Prompt:** Create a comprehensive test suite for I#: syntax tests, type checking tests (valid and invalid programs), codegen tests (expected output), and integration tests (compile → run → verify output). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"lang: I# test suite"`.


- [ ] Syntax tests: tokenize and parse all language features
- [ ] Type checking tests: valid programs pass, invalid programs produce correct errors
- [ ] Codegen tests: compile → run → verify stdout output
- [ ] Test runner: `isc --test tests/` runs all tests, reports pass/fail
- [ ] Commit: `"lang: I# test suite"`

### 7.2 Documentation

**Prompt:** Document I#: language tutorial, API reference, compiler usage, and sample programs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"docs: I# programming guide"`.


- [ ] Create `docs/architecture/isharp.md` — language overview
- [ ] Create `docs/guides/isharp-tutorial.md` — getting started tutorial
- [ ] Sample: `HelloWorld.is` — console hello world
- [ ] Sample: `Calculator.is` — console app with math
- [ ] Sample: `GuiApp.is` — windowed app with IxUI
- [ ] Update `README.md`
- [ ] Commit: `"docs: I# programming guide"`

---

## Priority Order

1. **§1.1** Language specification (define the language first)
2. **§2.1** Lexer (tokenizer)
3. **§2.2** Parser (AST generation)
4. **§3.1** Type checker and name resolution
5. **§4.1** x86-64 code generator
6. **§4.2** Linker integration
7. **§5.1** Standard library (core types)
8. **§6.1** Compiler driver (`isc`)
9. **§5.2** GUI bindings
10. **§7.1** Test suite
11. **§7.2** Documentation
12. **§6.2** Self-hosting *(long-term)*

---

## Example I# Program

```csharp
using System;
using IxUI;

namespace HelloApp
{
    class Program
    {
        static void Main()
        {
            var window = new Window("Hello I#", 400, 300);

            var button = new Button("Click Me!", 150, 130, 100, 32);
            button.Click += (sender, e) =>
            {
                MessageBox.Show("Hello!", "You clicked the button!", MessageBoxType.Info);
            };

            window.Add(button);
            Application.Run(window);
        }
    }
}
```

---

## Key Files

| File                               | Purpose                                 |
|------------------------------------|-----------------------------------------|
| `tools/isc/main.c`                 | [NEW] Compiler driver                   |
| `tools/isc/lexer.c`                | [NEW] Tokenizer                         |
| `tools/isc/parser.c`               | [NEW] Recursive descent parser          |
| `tools/isc/ast.h`                  | [NEW] AST node definitions              |
| `tools/isc/sema.c`                 | [NEW] Semantic analysis / type checking |
| `tools/isc/codegen.c`              | [NEW] x86-64 code generator             |
| `src/userland/libis-std/`          | [NEW] I# standard library               |
| `docs/architecture/isharp-spec.md` | [NEW] Language specification            |
| `docs/guides/isharp-tutorial.md`   | [NEW] Getting started guide             |

---

## Effort Estimates

| Component        | Effort          | Dependencies                   |
|------------------|-----------------|--------------------------------|
| Language spec    | Weeks           | Design decisions               |
| Lexer            | Days–Weeks      | Spec done                      |
| Parser           | Weeks           | Lexer                          |
| Type checker     | Weeks–Months    | Parser, symbol table           |
| Code generator   | Months          | Type checker, x86-64 knowledge |
| Standard library | Weeks           | Codegen working                |
| GUI bindings     | Weeks           | IxUI (P0105 §5), std lib       |
| Compiler driver  | Days            | All compiler stages            |
| Self-hosting     | Months          | Mature language + compiler     |
| **Total**        | **~6–9 months** |                                |
