/* Test-only wrapper around the EXACT verified upstream dispatcher. Never linked
 * into the filesystem, helper or CLI. Overrides can only REMOVE host features. */
#include "blake3_dispatch.c"
static uint32_t permitted_features;
uint32_t flitzfs_masked_detect(void) {
#if defined(IS_X86)
  ATOMIC_STORE(g_cpu_features, UNDEFINED);
  permitted_features = (uint32_t)get_cpu_features();
#else
  permitted_features = 0;
#endif
  return permitted_features;
}
uint32_t flitzfs_masked_limit(uint32_t requested) {
  const uint32_t selected = permitted_features & requested;
#if defined(IS_X86)
  ATOMIC_STORE(g_cpu_features, selected);
#endif
  return selected;
}
