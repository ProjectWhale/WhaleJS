/* Native baseline for cases/objfields.js. The JS object is four doubles in a
   struct; field access is a struct member, which is the ceiling the JIT is
   measured against. */
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

struct rec { double tag, key, flags, depth; };

static double run(double n)
{
    struct rec o = { 0, 0, 0, 0 };
    double i;

    for (i = 0; i < n; i++) {
        o.tag = fmod(i, 7);
        o.key = fmod(i, 13);
        o.flags += o.tag;
        o.depth = o.flags - o.key;
    }
    return o.tag + o.key * 2 + o.flags * 3 + o.depth * 5;
}

int main(void)
{
    clock_t t0, t1;
    double check = 0;
    int r;

    run(1000);

    t0 = clock();
    for (r = 0; r < 20; r++)
        check = run(200000);
    t1 = clock();

    report("objfields", (int)((t1 - t0) * 1000 / CLOCKS_PER_SEC), check, 1);
    return 0;
}