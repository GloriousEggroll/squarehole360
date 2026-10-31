// Routes this module's operator new/delete to the shared mimalloc.dll (see
// cmake/host_allocator.cmake). Linked into gow3.exe, rexruntime.dll and every
// other rexruntime consumer, so memory allocated in one module may be freed in
// another. Test fixtures that replace new/delete themselves (allocation-failure
// gates) define GOW3_OWN_NEW_DELETE and call mimalloc directly.
#if !GOW3_OWN_NEW_DELETE
#include <mimalloc-new-delete.h>
#endif
