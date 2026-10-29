#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <nopemd.h>

static const struct test {
    const char *name;
    double start_time;
    double start;
    double end;
    double end_time;
} tests[] = {
    {"finite end",           0.0,  0.0,          DBL_MAX,  -1.0},
    {"infinite end",         0.0,  0.0,          INFINITY, -1.0},
    {"negative bounds",      0.0, -INFINITY,    -1.0,      -1.0},
    {"large start",          0.0,  8e12,         INFINITY, -1.0},
    {"empty clipped range",  0.0,  DBL_MAX / 2,  DBL_MAX,  -1.0},
    {"start overflow",       8e12, 2e12,         3e12,     -1.0},
    {"end overflow",         8e12, 0.0,          2e12,     -1.0},
    {"clipped media time",   8e12, 0.0,          INFINITY,  1.0},
};

/*
 * Exercise conversion to microseconds and addition of the start_time offset,
 * both during configuration and after it. UBSan checks the integer additions;
 * FE_INVALID catches out-of-range llrint() calls, which UBSan does not detect.
 */
static int run_test(const char *filename, int use_pkt_duration, const struct test *test, int configured)
{
    struct nmd_ctx *s = nmd_create(filename);
    if (!s)
        return -1;

    int ret = -1;
    struct nmd_frame *frame = NULL;
    struct nmd_info info;
    if (nmd_set_option(s, "auto_hwaccel", 0) < 0 ||
        nmd_set_option(s, "use_pkt_duration", use_pkt_duration) < 0 ||
        nmd_set_option(s, "start_time", test->start_time) < 0 ||
        nmd_set_option(s, "end_time", test->end_time) < 0)
        goto end;

    if (configured && nmd_get_info(s, &info) < 0)
        goto end;

    feclearexcept(FE_INVALID);
    if (nmd_set_loop_range(s, test->start, test->end) < 0 ||
        nmd_get_info(s, &info) < 0)
        goto end;

    if (fetestexcept(FE_INVALID)) {
        fprintf(stderr, "%s: invalid floating-point conversion\n", test->name);
        goto end;
    }

    if (test->start_time == 0.0 && nmd_get_frame(s, 0.0, &frame) != NMD_RET_NEWFRAME)
        goto end;
    ret = 0;

end:
    printf("%s (%s configuration): %s\n", test->name, configured ? "after" : "before", ret < 0 ? "FAIL" : "OK");
    nmd_frame_releasep(&frame);
    nmd_freep(&s);
    return ret;
}

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;
    int ret = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); i++) {
        for (int configured = 0; configured < 2; configured++) {
            if (run_test(av[1], use_pkt_duration, &tests[i], configured) < 0)
                ret = -1;
        }
    }
    return ret;
}
