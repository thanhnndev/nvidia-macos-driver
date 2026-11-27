/*
 * FreeBSD kernel time on xnu. Uptime is mach_continuous_time (= user CLOCK_MONOTONIC); wall time is xnu's
 * calendar clock. sbintime_t is a 32.32 fixed-point count of seconds.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

volatile time_t time_second;

extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);

/*
 * User CLOCK_MONOTONIC on macOS is (calendar time - kern.boottime), which trails mach_continuous_time by the ~0.4 s
 * between mach's epoch and boottime being stamped. DRM syncobj/CS deadlines are absolute CLOCK_MONOTONIC ns, so the
 * kernel's ktime must share that origin (with mach_absolute_time every wait timed out 0.4 s early). The offset is
 * measured once and then fixed, so kernel time stays strictly monotonic and wall-clock steps are not followed.
 */
static int64_t mono_off_ns;
static int mono_off_done;

static void mono_calibrate(void)
{
    struct timeval bt = { 0, 0 };
    size_t len = sizeof(bt);
    clock_sec_t s;
    clock_nsec_t n;
    uint64_t cont;
    if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0 && bt.tv_sec != 0) {
        absolutetime_to_nanoseconds(mach_continuous_time(), &cont);
        clock_get_calendar_nanotime(&s, &n);
        int64_t mono = ((int64_t)s - bt.tv_sec) * 1000000000LL + (int64_t)n - (int64_t)bt.tv_usec * 1000LL;
        mono_off_ns = (int64_t)cont - mono;
    }
    mono_off_done = 1;
}

static uint64_t uptime_ns(void)
{
    uint64_t ns;
    if (!mono_off_done)
        mono_calibrate();
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns - (uint64_t)mono_off_ns;
}

sbintime_t sbinuptime(void)
{
    uint64_t ns = uptime_ns();
    return ((sbintime_t)(ns / 1000000000ULL) << 32) + nstosbt((int64_t)(ns % 1000000000ULL));
}

void nanouptime(struct timespec *ts)
{
    uint64_t ns = uptime_ns();
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

void microuptime(struct timeval *tv)
{
    uint64_t ns = uptime_ns();
    tv->tv_sec = (time_t)(ns / 1000000000ULL);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000);
}

void nanotime(struct timespec *ts)
{
    clock_sec_t s;
    clock_nsec_t n;
    clock_get_calendar_nanotime(&s, &n);
    ts->tv_sec = (time_t)s;
    ts->tv_nsec = (long)n;
    time_second = ts->tv_sec;
}

void nmx_microtime(struct timeval *tv)
{
    struct timespec ts;
    nanotime(&ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
}

time_t nmx_time_uptime(void)
{
    return (time_t)(uptime_ns() / 1000000000ULL);
}

/* FreeBSD ratecheck(9): true when at least 'mininterval' passed since *lasttime (then updated) */
int ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
    struct timeval now, delta;
    microuptime(&now);
    delta.tv_sec = now.tv_sec - lasttime->tv_sec;
    delta.tv_usec = now.tv_usec - lasttime->tv_usec;
    if (delta.tv_usec < 0) { delta.tv_sec--; delta.tv_usec += 1000000; }
    if (timevalcmp(&delta, mininterval, >=) || (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
        *lasttime = now;
        return 1;
    }
    return 0;
}

/* FreeBSD ppsratecheck(9): at most 'maxpps' events per second; maxpps < 0 means unlimited */
int ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
    struct timeval now;
    microuptime(&now);
    if (now.tv_sec != lasttime->tv_sec || (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
        *lasttime = now;
        *curpps = 1;
        return maxpps != 0;
    }
    (*curpps)++;
    return maxpps < 0 || *curpps <= maxpps;
}

int tvtohz(struct timeval *tv)
{
    int64_t ms = (int64_t)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    if (ms > INT32_MAX - 1)
        ms = INT32_MAX - 1;
    return (int)(ms * hz / 1000) + 1;
}
