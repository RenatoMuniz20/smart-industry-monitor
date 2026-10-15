#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int __real_nanosleep(const struct timespec *request, struct timespec *remaining);

/* Test-only linker wrapper: observe exact waits without timing upper bounds,
 * and inject deterministic EINTR/error paths. Never linked to production CLI.
 */
int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining)
{
    static int interrupted;
    const char *mode = getenv("SIM_TEST_SLEEP");
    fprintf(stderr, "TEST nanosleep %ld %ld\n", (long)request->tv_sec, request->tv_nsec);
    if (mode != NULL && strcmp(mode, "fail") == 0) {
        errno = EINVAL;
        return -1;
    }
    if (mode != NULL && strcmp(mode, "eintr") == 0 && !interrupted) {
        *remaining = *request;
        interrupted = 1;
        errno = EINTR;
        return -1;
    }
    return __real_nanosleep(request, remaining);
}
