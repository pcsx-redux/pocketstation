/* Case runner shared by the driver and the psemu shim. */
#ifndef POKKET_HARNESS_H
#define POKKET_HARNESS_H
#ifdef __cplusplus
extern "C" {
#endif
/* Called by a psemu_* entry point that this core cannot provide. Unwinds the current case and
   records it as not portable, naming the entry point. */
void pokket_not_ported(const char *what);
#ifdef __cplusplus
}
#endif
#endif
