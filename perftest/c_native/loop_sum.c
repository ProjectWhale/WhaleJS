/* Native baseline for cases/loop_sum.js - the same arithmetic in C.
   JS numbers are doubles, so `i % 3` is fmod(i,3) and `s` is a double; the
   comparison `=== 0` is then an exact compare. */
#include <stdio.h>
#include <time.h>
#include <math.h>

/* Print a result line the way the JS cases do. JS prints a non-finite double as
   `Infinity` / `-Infinity` / `NaN`, and the check has to be textually identical or
   the correctness gate sees a mismatch that is only a formatting difference. */
static void report(const char *name, int ms, double check, int is_int)
{
    const char *s;

    if (check != check)
        s = "NaN";
    else if (check > 1.7976931348623157e308)
        s = "Infinity";
    else if (check < -1.7976931348623157e308)
        s = "-Infinity";
    else {
        if (is_int)
            printf("WHALE_BENCH %s ms=%d check=%.0f\n", name, ms, check);
        else
            printf("WHALE_BENCH %s ms=%d check=%.17g\n", name, ms, check);
        return;
    }
    printf("WHALE_BENCH %s ms=%d check=%s\n", name, ms, s);
}

static double loop_sum(double n)
{
    double s = 0, i;

    for (i = 0; i < n; i++) {
        if (fmod(i, 3) == 0)
            s += i;
        else if (fmod(i, 7) == 0)
            s -= i;
        else
            s += 1;
    }
    return s;
}

int main(void)
{
    clock_t t0, t1;
    double check;

    loop_sum(1000);                       /* warm-up, as the JS case does */

    t0 = clock();
    check = loop_sum(5000000);
    t1 = clock();

    report("loop_sum", (int)((t1 - t0) * 1000 / CLOCKS_PER_SEC), check, 1);
    return 0;
}