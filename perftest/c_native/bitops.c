/* Native baseline for cases/bitops.js. The JS bitwise operators are defined on
   int32, so the C form is an int32_t and the ToInt32 of each operand is the cast
   that is already there. `!` is a logical not on the int32 value. */
#include <stdio.h>
#include <time.h>
#include <stdint.h>

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

static int32_t bits(double n)
{
    int32_t acc = 0;
    double i;

    for (i = 0; i < n; i++) {
        int32_t v = (int32_t)i & 255;

        acc = acc ^ (v << 3);
        acc = acc | (v >> 1);
        acc = (acc + (v % 7)) & 0xffff;
        if (!(v & 1))
            acc = acc - (~v & 15);
    }
    return acc;
}

int main(void)
{
    clock_t t0, t1;
    int32_t check;

    bits(1000);

    t0 = clock();
    check = bits(5000000);
    t1 = clock();

    report("bitops", (int)((t1 - t0) * 1000 / CLOCKS_PER_SEC), (double)check, 1);
    return 0;
}