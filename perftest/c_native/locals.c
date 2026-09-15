/* Native baseline for cases/locals.js. */
#include <stdio.h>
#include <time.h>

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

static double rotate(double n)
{
    double a = 1, b = 2, c = 3, i;

    for (i = 0; i < n; i++) {
        double t = a + b;

        a = b + 1;
        b = c + 1;
        c = t;
    }
    return a + b + c;
}

int main(void)
{
    clock_t t0, t1;
    double check;

    rotate(1000);

    t0 = clock();
    check = rotate(20000000);
    t1 = clock();

    report("locals", (int)((t1 - t0) * 1000 / CLOCKS_PER_SEC), check, 1);
    return 0;
}