/* The runtime half's public header: the engine plus the WinterTC surface.
 *
 * This is what an embedder includes. Everything declared here is in libsxnrt
 * and none of it knows what Node is; see include/sxn_node.h for the other
 * half and include/sxn_loop.h for supplying your own event loop.
 *
 * Unlike sxfe.h, this includes quickjs.h: these functions take and return
 * real JSValues, so a forward declaration of JSContext would not be enough.
 */

#ifndef SXN_RUNTIME_H
#define SXN_RUNTIME_H

#include <stddef.h>

#include <quickjs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the WinterTC surface on an existing context: TextEncoder, URL,
   URLPattern, Headers/Request/Response, fetch, the Streams, structuredClone,
   crypto, the timers, and the Sxn namespace. Returns 0, or -1 with the
   exception left on the context.

   The host makes the runtime and context itself, so it keeps its own memory
   limits, class ids and module loader. Install a loop backend first if you
   have one (sxn_set_loop_ops); install your own console first too, and the
   runtime's console.info/debug alias yours rather than replacing it. */
int sxn_install_runtime(JSContext *context);

/* One step of the runtime: drain the job queue, report unhandled rejections,
   maybe sweep cycles, then let the loop wait. Returns nonzero while there is
   still work that could settle a promise.

   `block` false does the sweep without waiting -- what a host with its own
   frame pump wants, calling this once a frame. `blocked_hint_ns` is how long
   the host itself waited; the idle sweep is driven by that number, so a host
   that does its own waiting should pass it rather than 0, or the sweep never
   fires and Sxn.gc() becomes the only way to reclaim a cycle. */
int sxn_runtime_tick(JSContext *context, int block, uint64_t blocked_hint_ns);

/* sxn_runtime_tick until it says there is nothing left. Returns nonzero if a
   job threw. A host driving its own loop does not call this. */
int sxn_run_event_loop(JSContext *context);

/* Settles one promise, driving the loop rather than only the job queue --
   what top-level await needs, since an await on a timer or a fetch cannot
   resume from the job queue alone. Returns the resolved value, or an
   exception. Takes ownership of `promise`. */
JSValue sxn_await_with_loop(JSContext *context, JSValue promise);

/* Idle cycle sweeping. `enabled` false turns it off entirely; `floor_bytes`
   is the tracked-allocation size below which the loop will not spend a
   collection, so a small program keeps its pause profile. Call before the
   loop starts; the defaults apply if it is never called. */
void sxn_configure_idle_gc(int enabled, size_t floor_bytes);

#ifdef __cplusplus
}
#endif
#endif
