# Modules

ReconOS loads code at runtime so a feature can arrive without the core being
rebuilt. There are two kinds, and the extension says which:

| Extension | What it is | Loaded | In the Apps menu |
| --- | --- | --- | --- |
| `.rts` | **Recon Tower System** — a subsystem, driver or service | at startup | no |
| `.rex` | **Recon Executable** — an application | at startup | yes |

Same format, same loader, same interface. The difference is when a thing loads
and whether a person is meant to launch it — the split Windows draws between
`.sys` and `.exe`, and macOS between `.kext` and `.app`.

The extension names *the ReconOS contract*, not the machine code inside it.
That is deliberate. A native build and, later, a wrapped foreign binary can
both present this interface, and ReconOS should not have to care which it got.
The descriptor's `payload` field says what the code actually is; today only
`RECON_PAYLOAD_NATIVE` is understood, and anything else is refused with a
message rather than mistaken for a native object.

## Where they live

```
/System/Modules/*.rts    system modules
/Apps/*.rex              applications
```

Modules a build ships with are copied into the filesystem on first run, once.
Nothing is overwritten: a file already there belongs to the user, and an
install that quietly replaces what is present is an install that undoes
people's choices. Delete one and it stays deleted.

## Writing one

A module includes exactly one ReconOS header.

```c
#include "recon_module.h"

static struct recon_appwin *create(struct recon_server *server,
        struct recon_font *font) {
    /* build and return your window */
}

static bool load(void) {
    static const struct recon_app_registration APP = {
        .name = "Calculator",
        .icon = "calculator",
        .create = create,
        .in_menu = true,
    };
    return recon_register_app(&APP);
}

static void unload(void) {
    recon_unregister_app("Calculator");
}

RECON_MODULE(
    .name = "Calculator",
    .version = "1.0",
    .description = "Arithmetic by mouse or keyboard",
    .load = load,
    .unload = unload,
);
```

`create` is called the first time somebody opens the application, not when the
module loads. An application nobody touches costs an entry in a list rather
than a window's worth of pixels — which is most of the argument for modules
over compiling everything in.

`load` returning false means the module declines: a missing dependency, a
machine it cannot work on. It is then unloaded cleanly and reported, rather
than left half-present.

### Adding a command

```c
static bool run(void *session, int argc, char **argv) {
    recon_command_print(session, "hello from a module\n");
    return true;
}

recon_register_command(&(struct recon_command_registration){
    .name = "hello",
    .usage = "hello",
    .summary = "Say hello",
    .run = run,
});
```

It then works in the terminal and over the control socket, and appears under
`help` in its own section, so it is clear what came with the system and what
arrived with something installed.

## Building one

`modules/CMakeLists.txt` has a helper:

```cmake
recon_add_module(Calculator ".rex" calculator/recon_calc.c)
```

Modules are **not** linked against ReconOS. They resolve its symbols at load
time from the executable, which is why the main target sets `ENABLE_EXPORTS`.
Undefined symbols at link time are expected; the loader uses `RTLD_NOW`, so a
symbol ReconOS does not provide is a refusal at load rather than a crash the
first time that code path is reached.

## The ABI

`RECON_MODULE_ABI` is the interface version, filled in for you by the
`RECON_MODULE` macro. A module built against a different number is **refused**,
not attempted — calling through a descriptor whose shape has changed does not
fail cleanly, it reads the wrong field as a function pointer.

Being straight about the cost of the design: because modules call ReconOS
directly rather than through a table of function pointers, a module is tied to
the build it was compiled against. The ABI number is what turns that from a
crash into a message. Rebuild your modules when you rebuild the core.

## Managing them

```
modules                       list what loaded, and what was refused and why
modules load /Apps/Thing.rex  load one now
modules unload Thing          unload one
apps                          what is installed, and which module contributed it
```

A module that fails is listed with the reason. A module that fails invisibly is
a module nobody fixes.

Unloading is refused while an application the module registered still has a
window, because unloading the code underneath a live window is a crash with
extra steps.

## What is not here yet

- **Dependencies between modules.** Load order is the only ordering, and
  unloading walks it in reverse.
- **Any sandbox.** A module runs in ReconOS's own process with its full
  privileges. It can crash the desktop. Treat installing one as you would
  installing a driver.
- **Signing.** Nothing checks where a module came from.
- **Foreign payloads.** `RECON_PAYLOAD_PE` is reserved and refused.
