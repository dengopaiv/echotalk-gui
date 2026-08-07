# Packaging the library as a DLL

Session 11. This was HANDOFF's "do this first" item, on the grounds that
it makes the library testable from Python, which is where the real NVDA
integration questions surface. That reasoning held up — the Python test
found nothing wrong with the library, but writing it forced decisions
about the exported surface that would otherwise have been made by
accident.

## The export surface

`src/echotalk.h` gained an `ECHOTALK_API` macro on every public
declaration:

- building the DLL defines `ECHOTALK_BUILD_DLL` → `__declspec(dllexport)`
- a C/C++ consumer linking the import library defines `ECHOTALK_DLL`
  → `__declspec(dllimport)`
- **neither defined → expands to nothing**, which is the case that has
  to keep working, because `tools/say.c` compiles the sources straight
  into itself with no defines at all.

On non-Windows, `ECHOTALK_BUILD_DLL` maps to
`__attribute__((visibility("default")))`, so the `so` target can build
with `-fvisibility=hidden` and still export the right 21 symbols.

Everything is plain cdecl. On x86-64 there is only one convention; on
32-bit MinGW the exported names come out **undecorated** — no leading
underscore, no `@n` suffix — so `ctypes.CDLL` finds them by plain C name
on both. Verified by dumping the export table on both builds; the two
lists are identical.

## Runtime dependencies

The point of a DLL a screen reader loads is that it must not drag in a
runtime the host has not got. `-static` inside the shared-library link
pulls libgcc and the MinGW support DLLs in, leaving:

```
win64:  KERNEL32.dll, api-ms-win-crt-{heap,private,runtime,stdio,string}-l1-1-0.dll
win32:  KERNEL32.dll, msvcrt.dll
```

That is exactly right — UCRT for 64-bit, MSVCRT for 32-bit, and nothing
else. Both ship with Windows. Re-check with
`objdump -p echotalk.dll | grep 'DLL Name'` after touching the link
line, because the failure mode is a DLL that loads fine here and not on
a user's machine.

## An ABI version

`echotalk_abi_version()` returns `ECHOTALK_ABI_VERSION`, currently 1.
A host that loads the library at runtime has no compile-time check
available at all, so it needs something to call before it trusts
anything else. Bump it whenever the exported surface changes in a way a
caller could notice.

## Two test programs, because one was not enough

`tools/test_dll.py` loads the DLL through `ctypes` exactly as NVDA
would, declares real `argtypes`/`restype` for every function, and
exercises the whole surface: lifecycle, every setter including its range
rejections, speak/read/available/stop, the Ctrl-D commands and their
error counter, and the single-character fix. 40 checks.

Declaring the signatures is not optional detail. Without `argtypes`,
ctypes guesses from the Python value; that happens to work for ints on
64-bit and silently breaks for doubles and for pointers above 2GB. The
test deliberately checks that `set_clock_multiplier(2.0)` *changes the
output* rather than merely returning 0 — a garbled double would still
return 0, since the range check would pass on whatever landed in the
register.

`tools/test_dll_load.c` runs the same checks from C, resolving every
export through `GetProcAddress` rather than linking. This exists because
**the 32-bit DLL cannot be reached from Python on this machine**: the
only interpreter here is 64-bit, and Windows refuses a bitness mismatch
at load time. Building the C test for both architectures verifies both
DLLs through the same dynamic path ctypes uses.

Both DLLs pass, and — a useful cross-check that fell out for free —
they produce **identical sample counts**: 7587 for "Hello there.", 3768
at a 2.0 clock multiplier, 4339 under Ctrl-D frame rate 2, 3082 for a
lone comma. 32-bit and 64-bit agree exactly.

## Verified

- 21 exports, undecorated, identical lists on both architectures.
- Dependencies as above, no MinGW runtime.
- `make test-dll`: 40 checks pass under 64-bit Python 3.11.
- `make test-dll-load`: all checks pass for both `build/win64` and
  `build/win32`.
- Second `echotalk_create()` refused while one is live, and `create`
  works again after `destroy` — the singleton constraint behaves across
  the boundary, which matters because a host changing voice will do
  exactly that sequence.
- The full HANDOFF baseline table still reproduces exactly under both
  Textalker versions, `hi_only` at −12127/+26833, `make test` passes,
  and `say` still builds with no defines.

## Not verified

`make so` compiles, but the machine this was developed on is Windows,
so what it produced here is not a Linux shared object and nobody has
loaded one. The header's visibility attribute is in place and the
sources are portable C, but treat the target as untried until someone
runs it on a Unix.

## Still to come

Streaming and index events, unchanged from before — `echotalk_speak()`
still synthesises the whole utterance before `echotalk_read()` returns
anything. Worth noting that neither will change the export surface much:
streaming is a change in *when* `echotalk_read` returns data, not in its
signature, so the ABI version probably survives it. Index events will
need new entry points and will not.
