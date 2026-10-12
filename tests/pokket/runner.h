#ifndef POKKET_RUNNER_H
#define POKKET_RUNNER_H
void pokket_run(const char *name, void (*fn)(void *), void *arg);
int pokket_summary(const char *suite);
#endif
