/* The node half's header: everything that exists to imitate Node.
 *
 * In libsxnnode, which is built only when SXN_ENABLE_NODE is on. A build
 * without it has the whole WinterTC surface and none of these; that is the
 * point of the split, so nothing in include/sxn_runtime.h refers back here.
 */

#ifndef SXN_NODE_H
#define SXN_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

struct JSContext;
struct JSModuleDef;

/* Installs the `node:buffer`/`node:path`/`node:events`/`node:process`
   compatibility modules (src/node.c + src/node_compat.js), following the
   same native-primitives-plus-JS-bootstrap split as sxn_install_runtime.
   exec_path becomes process.argv[0]. */
int sxn_install_node_compat(struct JSContext *context, const char *exec_path);
/* Registers one node: builtin module by its full specifier ("node:fs"), or
   returns NULL if this runtime has no such module. Called by the module
   loader, so that a program pays only for the builtins it imports. */
struct JSModuleDef *sxn_node_module_load(struct JSContext *context, const char *name);
/* Releases the atoms sxn_install_node_compat cached; call once, before
   JS_FreeContext, or the runtime reports them as leaked. */
void sxn_free_node_compat(struct JSContext *context);
/* Drops the emit memo's strong references to one emitter's `_events` object,
   event name and listener list. A cycle sweep cannot see past them, so both
   Sxn.gc() and the idle sweep release the memo first. Safe to call at any
   time: the memo is a cache and the next emit rebuilds it. */
void sxn_free_ee_memo(struct JSContext *context);

#ifdef __cplusplus
}
#endif
#endif
