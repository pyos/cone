# cone

Stackful coroutines in the least moral language ever.

### On supported platforms

Probably all UNIX-like OS on x86-64. Tested on Linux and macOS.

Supporting other architectures is a simple matter of adding relevant assembler code
to `cone_switch` and `cone_body`, as well as stack setup code to `cone_spawn_on`.
In theory.

### Building libraries for dummies

```
# The pure C version:
make CFLAGS=-O3 obj/libcone.a
# The version that requires linking with libc++abi or similar, but supports cone-local exception state:
make CFLAGS=-O3 obj/libcxxcone.a
```

Tests:

```
# The fast ones:
make CFLAGS=-O3 tests/cone
# The slow ones, which measure the time spent doing some pointless operations:
make CFLAGS=-O3 tests/perf
```

### An overview of available functions

See `cone.h`. Error handling is in `mun.h`. There are coroutine-blocking versions of
some standard library functions in `cold.h`. Optionally, these versions override the
dynamically linked ones from libc by default.

Some options (`CFLAGS="... -DOPTION=VALUE"`):

  * **CONE_EV_{SELECT,EPOLL,KQUEUE}**: (0 or 1 each) default is epoll on Linux, kqueue on macOS
    and FreeBSD (got lazy with macros for other BSDs there), select everywhere else.

  * **CONE_CXX**: (0 or 1) whether to save exception state to the stack before switching.
    This requires a C++ ABI library. Enabled for `libcxxcone.a`, disabled for `libcone.a`.

  * **CONE_DEFAULT_STACK**: (bytes; default = 64k) the stack size for coroutines created via the
    `cone(f, arg)` macro (as opposed to `cone_spawn(stksz, cone_bind(f, arg))`).

  * **COLD_OVERRIDE**: (0 or 1; default = 0) whether to provide custom implementations of
    libc functions, e.g. replace `read` with `cold_read`. Disabled by default due to some
    differences in behavior (e.g. `cold_recvmsg` with `MSG_DONTWAIT` will still block).

### In which various details are documented

  * **Address sanitizer**: supported.

  * **Shadow call stacks**: will break everything. Don't use them.

  * **Parallelism**: the scheduler is N:1. Each event loop is bound to a thread,
    and coroutines spawned on that loop will stay on it. `cone_event` can be used to
    synchronize coroutines even on different threads. `cone_wait` and `cone_wake` have
    semantics similar to Linux's `FUTEX_WAIT` and `FUTEX_WAKE`, except the storage for
    "implementation artifacts", as the kernel calls them, is provided by the user of
    the library (for performance and simplicity of the implementation), and also `cone_wait`
    accepts an arbitrary expression that is evaluated atomically instead of only checking
    for equality.

    (Note that the locks are kind of bad, though. My advice is to only use events
    in non performance critical or low contention places. Or even better, don't
    share events between threads.)

  * **Thread-safety**: `cone_drop` is atomic. The coroutine will be freed
    either by the calling thread, or by the thread to which it is pinned. `cone_cowait`,
    aka `cone_join`, is implemented in terms of `cone_event` and therefore allows
    a coroutine on one thread to wait for the completion of a coroutine on another.
    `cone_cancel` is atomic and ordered w.r.t. all coroutine-blocking calls within
    its target. `cone_deadline` is NOT thread-safe; it can only be used on coroutines
    within the same loop.
