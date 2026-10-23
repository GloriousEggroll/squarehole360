// Default-OFF Native CPU experiment. The source-auth build guard and startup
// instruction/alias checks must succeed before selecting this exact body.
#include "getdata_inline_leaf.h"
#include "generated/default/gow3_init.h"

namespace {
#if defined(_MSC_VER)
#define GOW3_GETDATA_HELPER_LINKAGE static __forceinline
#else
#define GOW3_GETDATA_HELPER_LINKAGE static inline __attribute__((always_inline))
#endif
#define GOW3_GETDATA_SAVE_NAME Gow3InlineGetDataSave26
#define GOW3_GETDATA_RESTORE_NAME Gow3InlineGetDataRestore26
#include "generated/native-getdata/getdata_register_helpers.inc"
#undef GOW3_GETDATA_RESTORE_NAME
#undef GOW3_GETDATA_SAVE_NAME
#undef GOW3_GETDATA_HELPER_LINKAGE
}

// The original strong kick and memset thunks remain unchanged. This body reads
// the current volatile completion/report words on EVERY call, without memoizing.
#define __savegprlr_26 Gow3InlineGetDataSave26
#define __restgprlr_26 Gow3InlineGetDataRestore26
#define GOW3_GETDATA_LEAF_NAME Gow3GetDataExactInlineLeaf
#include "generated/native-getdata/getdata_leaf_body.inc"
#undef GOW3_GETDATA_LEAF_NAME
#undef __restgprlr_26
#undef __savegprlr_26

// Selected only for the authenticated blocking caller in the optional native
// result experiment. All report reads, sentinel handling and ABI effects remain
// the original body; only a proven completed query can bypass its serial gate.
#define __savegprlr_26 Gow3InlineGetDataSave26
#define __restgprlr_26 Gow3InlineGetDataRestore26
#define GOW3_GETDATA_LEAF_NAME Gow3GetDataPublishedQueryLeaf
#include "generated/native-getdata/getdata_published_body.inc"
#undef GOW3_GETDATA_LEAF_NAME
#undef __restgprlr_26
#undef __savegprlr_26

bool Gow3GetDataExactInlineLeafAvailable() noexcept {
    // Trace/profiler macro scope/names are observable diagnostic behavior. Keep
    // their original function boundaries instead of claiming trace equivalence.
#if defined(REXGLUE_ENABLE_GUEST_FUNCTION_TRACE) || defined(REXGLUE_ENABLE_PROFILING)
    return false;
#else
    // A future strong override of either shared register helper must keep the
    // original route. The generated weak aliases currently name these imports.
    // Weak aliases are bound after the compiler sees distinct declarations.
    // Read the final function-pointer values at runtime so that comparison is
    // not folded to false before the linker gives each pair the same address.
    auto* volatile save=&__savegprlr_26;
    auto* volatile imported_save=&__imp____savegprlr_26;
    auto* volatile restore=&__restgprlr_26;
    auto* volatile imported_restore=&__imp____restgprlr_26;
    return save==imported_save&&restore==imported_restore;
#endif
}
