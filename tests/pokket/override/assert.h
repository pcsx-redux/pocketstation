/* Test-harness assert(): records the failing expression and unwinds to the case runner, so one
   failing case does not stop the suites copied from mentalfoundry/Pokket. Shadows <assert.h> for
   those files only (this directory is first on their include path). */
#ifndef POKKET_HARNESS_ASSERT_H
#define POKKET_HARNESS_ASSERT_H
#ifdef __cplusplus
extern "C" {
#endif
void pokket_assert_fail(const char *expr, const char *file, int line);
#ifdef __cplusplus
}
#endif
#undef assert
#define assert(e) ((e) ? (void)0 : pokket_assert_fail(#e, __FILE__, __LINE__))
#endif
