# Node-API headers

`js_native_api.h`, `js_native_api_types.h`, `node_api.h` and `node_api_types.h`
are copied verbatim from Node.js (MIT licence, see the Node repository) so that
a `.node` addon compiled against Node sees exactly the declarations it was
built with. They are the ABI contract; `src/napi.c` is this runtime's
implementation of it on QuickJS.

Copied from Node v25.2.1. Node-API is versioned and additive, so a newer Node
adds declarations rather than changing existing ones -- refresh by copying the
four files again.
