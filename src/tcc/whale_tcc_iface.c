/* ------------------------------------------------------------------------
 * WhaleJS: TinyCC internal interface
 *
 * Compiled as part of the TinyCC translation unit (libtcc.c includes it at the
 * very end), so it can reach TinyCC's internals directly. First step of the
 * route in notes <16>: emit TinyCC IR / machine code straight from the JIT
 * generator instead of handing TinyCC C source to parse. That would
 *   - skip C parsing (faster JIT compiles),
 *   - remove the need to include quickjs.h at all (no header path dependency),
 *   - give us the IR to optimize on instead of guessing TinyCC's lowering.
 *
 * For now: the plumbing (exported wrappers) plus a probe proving the linkage
 * works and internal state is reachable.
 * ------------------------------------------------------------------------ */

/* Probe: 1 when TinyCC's current state is reachable, 0 otherwise. tcc_state and
   the vstack/vtop pair are TinyCC internals declared in tcc.h, so reaching them
   proves the interface file really is compiled inside TinyCC's translation unit
   and that the JIT generator can drive TinyCC directly from here. */
int JS_WhaleTccProbe(void)
{
    /* __vstack is TinyCC's value stack (tcc.h). Reading its capacity proves we
       are compiled inside TinyCC's translation unit AND that the value is the
       real one: VSTACK_SIZE is 256 in tcc.h. */
    return (int)(sizeof(__vstack) / sizeof(__vstack[0])) - 1;
}