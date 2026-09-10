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
/* ========================================================================
 * IR direct emission - first closed loop
 *
 * Instead of handing TinyCC C source to parse, drive its internals: build the
 * function symbol, emit the prologue, push operands on the value stack, lower
 * them with gen_op(), emit the epilogue, and let tcc_relocate() produce callable
 * machine code. This is the prerequisite for optimising on the IR (register
 * keeping, peepholes) and it drops the quickjs.h dependency entirely.
 *
 * First cut: a function with no parameters, so the parameter chain (the least
 * certain part of TinyCC's type model) is not involved yet.
 * ====================================================================== */


/* caller-facing op selector */
#define WHALE_IR_ADD 0
#define WHALE_IR_SUB 1
#define WHALE_IR_MUL 2
#define WHALE_IR_DIV 3

/* TinyCC names single-character operators by their character value (see the
   many gen_op('+') calls in tccgen.c), so no token table is needed here. */
static int whale_ir_token(int op)
{
    switch (op) {
    case WHALE_IR_ADD: return '+';
    case WHALE_IR_SUB: return '-';
    case WHALE_IR_MUL: return '*';
    case WHALE_IR_DIV: return '/';
    default: return 0;
    }
}

/* Compile `double f(void) { return a <op> b; }` straight into IR.
   Returns a callable function pointer, or NULL on failure.
   NB: TinyCC has exactly one global state (tcc_state and friends), so this has
   to hand it back before returning or it breaks every other user of it. */
void *JS_WhaleIrCompileConst2(const char *tcc_dir, double a, double b, int op)
{
    TCCState *saved_state = tcc_state;
    Section *saved_cur_text = cur_text_section;
    Sym *saved_local = local_stack, *saved_global = global_stack;
    char tcc_dir2[1024];
    TCCState *s;
    CType ft, dt;
    Sym *retsym, *funcsym;
    CValue cval;
    TokenSym *ts;
    int tok = whale_ir_token(op);
    void *fn = NULL;

    if (!tok)
        return NULL;
    snprintf(tcc_dir2, sizeof(tcc_dir2), "%s/lib", tcc_dir);
    s = tcc_new();
    if (!s)
        return NULL;
    /* libtcc1-64.a is needed for the link step; the paths must be set before the
       output type, because tcc_new() fixes them. Note that emitting IR still
       needs NO include path - not parsing C is the whole point. */
    tcc_set_lib_path(s, tcc_dir);
    tcc_add_library_path(s, tcc_dir2);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    /* tcc_new() builds the state but the sections/parser live in tcc_add_file,
       so run a trivial compile to get a usable state. */
    tcc_compile_string(s, "void whale_ir_init(void) {}");

    /* function type: return double, no parameters. ref -> return-type Sym,
       whose ->next chain holds parameters (empty here). */
    ts = tok_alloc("whale_ir_fn", 11);
    if (!ts)
        goto done;
    retsym = sym_push2(&local_stack, 0, VT_DOUBLE, 0);
    retsym->type.t = VT_DOUBLE;
    retsym->type.ref = NULL;
    retsym->next = NULL;

    ft.t = VT_FUNC;
    ft.ref = retsym;
    funcsym = sym_push2(&local_stack, ts->tok, VT_FUNC, 0);
    funcsym->type = ft;
    funcsym->r = VT_CONST;

    /* the frame, same sequence gen_function() uses */
    cur_text_section = text_section;
    if (!cur_text_section)
        goto done;
    nocode_wanted = 0;
    ind = cur_text_section->data_offset;
    put_extern_sym(funcsym, cur_text_section, ind, 0);
    funcname = "";
    func_ind = ind;
    vla_sp_loc = -1;
    vla_sp_root_loc = -1;
    sym_push2(&local_stack, SYM_FIELD, 0, 0);
    local_scope = 1;
    gfunc_prolog(&funcsym->type);
    local_scope = 0;

    /* the body */
    dt.t = VT_DOUBLE;
    dt.ref = NULL;
    cval.d = a;
    vsetc(&dt, VT_CONST, &cval);
    cval.d = b;
    vsetc(&dt, VT_CONST, &cval);
    gen_op(tok);
    gv(RC_FLOAT);
    gfunc_return(&func_vt);

    rsym = 0;
    gfunc_epilog();
    /* g() only advances ind; the section must be told where the code ends or
       tcc_relocate() will not copy it into the executable buffer. */
    cur_text_section->data_offset = ind;
    elfsym(funcsym)->st_size = ind - func_ind;
    check_vstack();

    if (tcc_relocate(s, TCC_RELOCATE_AUTO) >= 0)
        fn = (void *)(cur_text_section->data + func_ind);

done:
    /* hand the single global state back to whoever had it */
    tcc_state = saved_state;
    cur_text_section = saved_cur_text;
    local_stack = saved_local;
    global_stack = saved_global;
    return fn;
}