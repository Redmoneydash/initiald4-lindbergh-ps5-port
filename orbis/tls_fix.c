/* The prospero build uses emulated TLS; Mesa's glapi references the weak TLS
 * init thunk _ZTH23_mesa_glapi_tls_Context, which stays undefined in a payload
 * link. The native converter requires every dynamic symbol to resolve, so
 * give it a hidden no-op body — emutls variables initialise lazily anyway. */
__attribute__((visibility("hidden")))
void _ZTH23_mesa_glapi_tls_Context(void) {}
