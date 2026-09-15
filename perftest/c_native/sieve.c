/* Native baseline for cases/sieve.js. */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

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

static double sieve(double n)
{
    double *flags = calloc((size_t)n + 1, sizeof(double));
    double count = 0, i, p, m, k;

    for (i = 2; i <= n; i++)
        flags[(size_t)i] = 1;
    for (p = 2; p * p <= n; p++) {
        if (flags[(size_t)p]) {
            for (m = p * p; m <= n; m += p)
                flags[(size_t)m] = 0;
        }
    }
    for (k = 2; k <= n; k++)
        if (flags[(size_t)k])
            count++;
    free(flags);
    return count;
}

int main(void)
{
    clock_t t0, t1;
    double check = 0;
    int r;

    sieve(1000);

    t0 = clock();
    for (r = 0; r < 20; r++)
        check = sieve(300000);
    t1 = clock();

    report("sieve", (int)((t1 - t0) * 1000 / CLOCKS_PER_SEC), check, 1);
    return 0;
}