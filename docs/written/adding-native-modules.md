# Adding Native Modules

This guide shows how to add new Aussom classes whose methods are written
in C and compiled into your program. It covers the pieces involved, two
complete examples, and the steps for adding a class to aussom-c's own
standard library.

aussom-c never loads code at run time. Everything described here is
compiled and linked into the program that runs the interpreter.

## 1. Building blocks

### 1.1 An extern class

A module has two halves: an Aussom declaration and a C implementation.
The declaration is an `extern class`. Its `extern` methods have no body;
their code is in C.

```
static extern class greet : com.example.text.Greeter {
    public extern hello(string Name);
}
```

The text after the colon is the class's **reference**. It splits at the
last dot into two parts:

| Part | Value | Meaning |
|---|---|---|
| Package | `com.example.text` | The registered package that holds the C code |
| Class | `Greeter` | A class inside that package |

The Aussom name (`greet`) is what scripts use. It plays no part in
finding the C code; only the reference does. The same reference strings
are used by aussom-base, where they name a Java class.

### 1.2 The C function signature

Every extern method is a C function of type `extern_fn`
(`include/aussom/extern_fn.h`):

```c
av_type *fn(av_type *self, environment *env, av_list *args);
```

- `self` is the object the method was called on. For a static class it
  is the class's single instance.
- `env` is the calling context. `environment_engine(env)` gives the
  engine.
- `args` holds the evaluated arguments, in order.
- The function returns an Aussom value, or an exception (section 4).

### 1.3 The tables

C functions are listed in plain `const` tables
(`include/aussom/extern_registry.h`). A method table maps method names
to functions, a class table maps class names to method tables, and a
package names its class table. Each table ends with a `{ NULL, NULL }`
entry.

```c
static const aussom_extern_method greeter_methods[] = {
    { "hello", greet_hello },
    { NULL, NULL }
};

static const aussom_extern_class text_classes[] = {
    { "Greeter", greeter_methods },
    { NULL, NULL }
};

const aussom_extern_package example_text_package = {
    "com.example.text", text_classes
};
```

A package can hold any number of classes. Two Aussom classes may use the
same reference, in which case they share one method table.

### 1.4 Registering a package on an engine

A package does nothing until it is registered on an engine:

```c
engine_add_extern_package(e, &example_text_package);
```

Rules:

- **Per engine.** A package is visible only to the engine it was added
  to. Add it to every engine that needs it.
- **First one wins.** If a package with that name is already registered,
  the call returns `false` and changes nothing. The built-in packages
  `com.aussom.types` and `com.aussom.stdlib` cannot be replaced.
- **Copied.** The tables are copied, so they do not need to outlive the
  call. The functions themselves must stay available.
- **Any time, any thread.** Registration is safe while scripts run.

### 1.5 Binding and calling

When the parser reads an `extern class`, it looks up the reference in
the engine's packages right away. A missing package or class is a parse
error:

```
Extern class 'com.example.text.Greeter' not found.
```

So a package must be registered **before** the source that declares the
class is parsed. When a script calls an extern method, the engine looks
the method name up in the class's table. If it is not there, it tries
the class's parents, and then throws `EXTERN_NO_SUCH_METHOD`, as Java does.

Every extern method must be declared `public`. A `private extern` line,
or one with no access word, is a parse error:

```
Extern function 'init' must be public.
```

Access words can't protect a C function. Any script can declare its own
`extern class` bound to the same C class, with its own declarations, and
call every method in the table. So the C code is the real guard of the
API: each function must be safe to call by any script, in any order, on
any object. Section 3.2 shows how a class with C state does this.

## 2. Example: a static class

This example adds a `greet` class with two methods. It is built as part
of a host program, the usual way to extend the interpreter without
changing aussom-c.

### 2.1 The Aussom declaration: `textkit.aus`

```
/**
 * Builds simple text.
 */
static extern class greet : com.example.text.Greeter {
    /**
     * Builds a greeting.
     * @p Name is a string with the name to greet.
     * @r A string with the greeting.
     */
    public extern hello(string Name);

    /**
     * Repeats a string.
     * @p Text is a string to repeat.
     * @p Times is an int with the number of copies.
     * @r A string with the copies joined together.
     */
    public extern repeat(string Text, int Times);
}
```

### 2.2 The C implementation: `textkit.c`

```c
#include <stdio.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/environment.h"
#include "aussom/extern_registry.h"

static av_type *greet_hello(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *name = av_string_get((av_string *)av_list_get(args, 0));
    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!", name);
    return &av_string_new(buf)->base.base;
}

static av_type *greet_repeat(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *text = av_string_get((av_string *)av_list_get(args, 0));
    int64_t times = av_int_get((av_int *)av_list_get(args, 1));
    if (times < 0) {
        av_exception *ex = av_exception_new("greet.repeat(): Times can't be negative.");
        av_exception_set_id(ex, "INVALID_ARGUMENT");
        return &ex->base.base;
    }

    size_t len = strlen(text);
    char *out = AUSSOM_ALLOC_ATOMIC(len * (size_t)times + 1);
    for (int64_t i = 0; i < times; i++) memcpy(out + len * (size_t)i, text, len);
    out[len * (size_t)times] = '\0';
    return &av_string_new(out)->base.base;
}

static const aussom_extern_method greeter_methods[] = {
    { "hello", greet_hello },
    { "repeat", greet_repeat },
    { NULL, NULL }
};

static const aussom_extern_class text_classes[] = {
    { "Greeter", greeter_methods },
    { NULL, NULL }
};

const aussom_extern_package example_text_package = {
    "com.example.text", text_classes
};
```

### 2.3 The host program: `host.c`

The host creates each engine through one function, so every engine gets
the package and the module. `engine_add_module` makes the declaration
available to `include textkit;` without a file on disk.

```c
#include <stdio.h>

#include "aussom/aussom_gc.h"
#include "aussom/engine.h"
#include "aussom/parser.h"

extern const aussom_extern_package example_text_package;
extern const char textkit_aus[];

static engine *new_engine(void)
{
    engine *e = engine_new();
    engine_add_extern_package(e, &example_text_package);
    engine_add_module(e, "textkit.aus", textkit_aus);
    return e;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.aus\n", argv[0]);
        return 1;
    }
    aussom_gc_init();               /* once, before any engine */
    engine *e = new_engine();
    aussom_parse_file(e, argv[1]);
    if (engine_parse_exception(e) != NULL) {
        /* An error that stopped the parse is left for the host to
         * report; a syntax error is already logged, and engine_run
         * refuses to run after one. */
        fprintf(stderr, "%s\n", engine_parse_exception_stack_trace(e));
        return 1;
    }
    return engine_run(e, argv[1]);
}
```

`textkit_aus` holds the text of `textkit.aus` as a C string. aussom-c
embeds its own `.aus` files the same way, with `xxd`
(see the `%_aus.c` rule in the `Makefile`):

```
printf 'const char textkit_aus[] = {\n' > textkit_aus.c
xxd -i < textkit.aus >> textkit_aus.c
printf ', 0x00\n};\n' >> textkit_aus.c
```

Instead of `engine_add_module`, a host can put `textkit.aus` in a
directory and call `engine_add_include_path(e, dir)`. The engine's
modules are searched before the include paths.

### 2.4 Building

Build aussom-c's library, then compile against its headers. Every file
that includes `aussom/aussom_gc.h` must be compiled with `-DGC_THREADS`.

```
make lib
CFLAGS="-std=c11 -pthread -DGC_THREADS -D_POSIX_C_SOURCE=200809L \
        -I/path/to/aussom-c/include -I/path/to/aussom-c/third_party/bdwgc/include"
cc $CFLAGS -c textkit.c textkit_aus.c host.c
cc -pthread -o myapp host.o textkit.o textkit_aus.o \
   /path/to/aussom-c/build/libaussom.a -lm -ldl
```

On macOS and Windows, leave off `-ldl`.

### 2.5 Using it

```
include textkit;

class Main {
    public main() {
        c.println(greet.hello("Ada"));
        c.println(greet.repeat("ab", 3));
        try {
            greet.repeat("ab", -1);
        } catch (ex) {
            c.println(ex.getId() + ": " + ex.getText());
        }
        return 0;
    }
}
```

Output:

```
Hello, Ada!
ababab
INVALID_ARGUMENT: greet.repeat(): Times can't be negative.
```

## 3. Example: a class with C state

An instance class often needs C data per object. Each object has an
**extern object** slot, a `void *` that the C code may set and read with
`av_object_set_extern_object` and `av_object_get_extern_object`. The
standard library's `AtomicLong` works this way.

The pattern is a constructor written in Aussom that calls an extern
method to build the state.

### 3.1 Declaration (added to `textkit.aus`)

```
/**
 * Keeps a running total.
 */
extern class Tally : com.example.text.Tally {
    /**
     * Creates a new Tally.
     * @p Start is an optional int with the starting total. Default 0.
     */
    public Tally(int Start = 0) {
        return this.init(Start);
    }
    public extern init(int Start);

    /**
     * Adds to the total.
     * @p Amount is an int with the amount to add.
     * @r This object.
     */
    public extern add(int Amount);

    /**
     * Gets the total.
     * @r An int with the total.
     */
    public extern total();
}
```

The default value lives on the Aussom constructor, not on the extern
method; see section 4.4 for why.

### 3.2 C implementation (added to `textkit.c`)

Section 1.5 explains why the C code can't trust its receiver. A script
can call `add` on an object whose `init` never ran. Until `init` runs,
the extern object slot points at the object itself, not at a
`tally_state`. Reading it as a `tally_state` would read the wrong
memory. So the state struct starts with a tag that only `tally_init`
writes, and `tally_of` returns NULL unless the tag is there. The
standard library's classes with state do the same; see `conc_state` in
`src/stdlib/concurrent_methods.c`.

```c
#define TALLY_MAGIC 0x54414c59u   /* "TALY" */

typedef struct tally_state {
    uint32_t magic;               /* first field, always TALLY_MAGIC */
    int64_t total;
} tally_state;

/* The receiver's state, or NULL when init never ran on it. */
static tally_state *tally_of(av_type *self)
{
    if (self == NULL || self->type != C_TYPE_OBJECT) return NULL;
    void *eo = av_object_get_extern_object((av_object *)self);
    if (eo == NULL || eo == (void *)self ||
        ((tally_state *)eo)->magic != TALLY_MAGIC)
        return NULL;
    return (tally_state *)eo;
}

static av_type *tally_bad_self(void)
{
    return &av_exception_new("Tally: receiver is not an initialized "
                             "Tally object.")->base.base;
}

static av_type *tally_init(av_type *self, environment *env, av_list *args)
{
    (void)env;
    tally_state *s = AUSSOM_ALLOC(sizeof(*s));
    s->magic = TALLY_MAGIC;
    s->total = av_int_get((av_int *)av_list_get(args, 0));
    av_object_set_extern_object((av_object *)self, s);
    return self;
}

static av_type *tally_add(av_type *self, environment *env, av_list *args)
{
    (void)env;
    tally_state *s = tally_of(self);
    if (s == NULL) return tally_bad_self();
    s->total += av_int_get((av_int *)av_list_get(args, 0));
    return self;
}

static av_type *tally_total(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    tally_state *s = tally_of(self);
    if (s == NULL) return tally_bad_self();
    return &av_int_new(s->total)->base.base;
}

static const aussom_extern_method tally_methods[] = {
    { "init", tally_init },
    { "add", tally_add },
    { "total", tally_total },
    { NULL, NULL }
};
```

Then add the class to the package's class table:

```c
static const aussom_extern_class text_classes[] = {
    { "Greeter", greeter_methods },
    { "Tally", tally_methods },
    { NULL, NULL }
};
```

### 3.3 Using it

```
include textkit;

class Main {
    public main() {
        t = new Tally(10);
        t.add(5).add(2);
        c.println(t.total());   // 17
        return 0;
    }
}
```

The state is allocated with `AUSSOM_ALLOC`, so the collector keeps it
alive as long as the object is reachable and frees it afterward. If one
object can be used from more than one interpreter thread, guard its
state with a lock, as `AtomicLong` does in
`src/stdlib/concurrent_methods.c`.

## 4. Working with values

### 4.1 Arguments and return values

Include `aussom/av.h`. Each argument is an `av_type *`; its `type` field
says which kind it is. Cast it to the matching type before reading it.

| Aussom type | `type` value | Create | Read |
|---|---|---|---|
| null | `C_TYPE_NULL` | `av_null_new()` | |
| bool | `C_TYPE_BOOL` | `av_bool_new(b)` | `av_bool_get` |
| int | `C_TYPE_INT` | `av_int_new(i)` | `av_int_get` (an `int64_t`) |
| double | `C_TYPE_DOUBLE` | `av_double_new(d)` | `av_double_get` |
| string | `C_TYPE_STRING` | `av_string_new(s)` | `av_string_get` |
| list | `C_TYPE_LIST` | `av_list_new()` | `av_list_get`, `av_list_size`, `av_list_add` |
| map | `C_TYPE_MAP` | `av_map_new()` | `av_map_get`, `av_map_put` |

To return a value, return its embedded base: `&av_int_new(5)->base.base`.
Return `self` to let calls chain. Return `&av_null_new()->base.base`
when there is nothing to return. Never return `NULL`.

### 4.2 Which arguments to check

A parameter with a declared type (`int Times`) is checked before the C
function runs. A call with the wrong type or the wrong number of
arguments fails with `FUNCT_NOT_FOUND`. A `null` argument matches a
`string`, `list`, `map` or `callback` parameter, but it is stopped before
the C function runs and fails with `EXTERN_INVOCATION_TARGET_EXCEPTION`,
as the cast in a Java method would. The one exception is a parameter
whose default is `null` (`int Radix = null`): there the C code receives
the `null` and must check for it. Otherwise the C code can cast typed
arguments directly. A parameter with no type (`abs(Val)`) accepts any
value, and the C code must check `type` itself. An `int` argument passed
to a `double` parameter arrives already converted to a double.

### 4.3 Errors

Return an exception instead of a value. Give it an ID, so scripts can
tell errors apart with `ex.getId()`:

```c
av_exception *ex = av_exception_new("mymod.f(): What went wrong.");
av_exception_set_id(ex, "SOME_ERROR_ID");
return &ex->base.base;
```

A script can catch it with `try`/`catch`. An uncaught one stops the
program and is reported like any other Aussom exception.

### 4.4 Current differences from aussom-base

Two argument rules differ from aussom-base today. Write C code that
works either way:

- **Default values are not passed.** If an extern method declares
  `int B = 7` and the caller leaves it out, the C function receives one
  argument fewer; aussom-base would pass 7. Put defaults on an Aussom
  wrapper, as `Tally`'s constructor does, or check `av_list_size(args)`
  and supply the default in C.
- **`...` arguments arrive flat.** For `f(int A, ...)` called as
  `f(1, 2, 3)`, the C function receives three arguments; aussom-base
  passes `A` and then one list holding the rest.

### 4.5 Memory

All Aussom values live in the garbage-collected heap, and nothing is
freed by hand.

- Create values with the `av_*` functions. Never `free` one.
- For your own data, use `AUSSOM_ALLOC` (may hold pointers),
  `AUSSOM_ALLOC_ATOMIC` (plain bytes, such as text) and `AUSSOM_STRDUP`.
- A resource owned by another library, such as a file handle or a
  socket, stays in that library's world. Free it with that library's
  calls, typically from an explicit `close` method.
- A thread that your C code creates must be created with
  `pthread_create` so the collector knows about it. See the notes at
  the top of `include/aussom/aussom_gc.h`.

## 5. Adding a class to the standard library

Use this path only for something every engine should have. aussom-c is
a port of aussom-base, so a new standard library class should exist in
aussom-base first, with the same `.aus` declaration and reference, and
then be ported here.

1. **Declaration.** Add `stdlib/aus/<name>.aus`, or add the class to an
   existing file there. The `Makefile` embeds every file in that
   directory as `aussom_<name>_aus` with no further change.
2. **Module list.** For a new file, add it to `src/stdlib_sources.c`: an
   `extern const char aussom_<name>_aus[];` line and a
   `{ "<name>.aus", aussom_<name>_aus }` row. Every engine then has it
   as a module, so scripts can `include <name>;`.
3. **C code.** Add `src/stdlib/<name>_methods.c` with the functions and
   one method table per class, named `stdlib_<stem>_methods`. The build
   finds new files under `src/` on its own.
4. **Declaration of the table.** Add
   `extern const aussom_extern_method stdlib_<stem>_methods[];` to
   `src/stdlib/stdlib_internal.h`.
5. **Package entry.** Add `{ "<Class>", stdlib_<stem>_methods }` to
   `stdlib_classes` in `src/stdlib/stdlib_packages.c`. `<Class>` is the
   last segment of the reference, such as `AUuid` for
   `com.aussom.stdlib.AUuid`. Classes in the `com.aussom.types` package
   go in `types_classes` instead.
6. **Tests.** Add Aussom tests to `tests/c-interpreter.aus` or a new
   Aussom test file, or add a C test file `tests/test_<name>.c`, which
   `make test` picks up on its own. `tests/interpreter.aus` is a copy of
   aussom-base's and is not changed here.

`src/stdlib/uuid_methods.c` and the `uuid` class in
`stdlib/aus/util.aus` are a small, complete example of this layout.

## 6. Security

A host can require that every extern class be on a list. When the
security property `aussom.extern.allowlist.enforce` is `true`, a
reference must match an entry of the list property
`aussom.extern.allowed`, either exactly or through a `package.*` entry.
The default list admits only the two built-in packages. Enforcement is
off by default. A host that turns it on adds its own packages to the
list. Policy belongs to the security manager the engine is built with
(`include/aussom/security_manager.h`), so the host sets it there before
creating the engine, the way a Java host sets `props` in its
`SecurityManagerImpl` subclass:

```c
security_manager *sm = security_manager_impl_new();
av_strmap_put(sm->props, "aussom.extern.allowlist.enforce",
              &av_bool_new(true)->base.base);
av_list *allowed = (av_list *)av_strmap_get(sm->props, "aussom.extern.allowed");
av_list_add(allowed, &av_string_new("com.example.text.*")->base.base);
engine *e = engine_new_with_security(sm);
```

A reference that is not allowed is a parse error, and the engine never
looks for its package.

## 7. Troubleshooting

| Message | Cause |
|---|---|
| `Extern class '<ref>' not found.` (parse error) | The package is not registered on this engine, it has no such class, or the reference has no package part. Register the package before parsing. |
| `Extern class '<ref>' is not permitted.` (parse error) | Allowlist enforcement is on and the reference is not on the list (section 6). |
| `Extern function '<m>' must be public.` (parse error) | An extern method is declared `private` or with no access word. Every extern method must be `public` (section 1.5). |
| `EXTERN_NO_SUCH_METHOD`: `External call, no such method 'm'.` | The class's method table has no entry for `m`. Check the spelling in the table. |
| `FUNCT_NOT_FOUND`: `Object 'X' has no overload of 'm' matching call signature 'i,s'.` | The call does not match the declaration's argument count or types. The signature lists the call's argument types, one letter each: `b` bool, `i` int, `d` double, `s` string, `l` list, `m` map, `o` object, `c` callback, `n` null. |
| `EXTERN_INVOCATION_TARGET_EXCEPTION`: `External call, invocation target exception for method 'm', the external method threw an uncaught exception: ClassCastException: AussomNull cannot be cast to AussomString.` | A script passed `null` to a typed parameter whose default is not `null` (section 4.2). |
| `INSTANTIATION_FAILURE`: `Instantiation threw exception.`, with `Cannot inherit from two external classes. First is '<a>' and second is '<b>'.` at the head of the stack trace | A class has more than one extern backing: its own reference plus an extern parent, or two extern parents. Only one is allowed. |
