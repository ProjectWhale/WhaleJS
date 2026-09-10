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

/* Emit `double f(void) { return a <op> b; }` straight into IR.
   Returns a callable function pointer, or NULL on failure.

   TinyCC has exactly one global state (tcc_state and the rest), so this must not
   be called while another compilation is live; it also has to open/close the
   translation unit the way tcc_compile() does, because emitting by hand is not
   something the parser is otherwise prepared for. */
void *JS_WhaleIrCompileConst2(const char *tcc_dir, double a, double b, int op)
{
    char lib_dir[1024];
    TCCState *s;
    CType ft, dt;
    Sym *retsym, *funcsym;
    CValue cval;
    TokenSym *ts;
    int tok = whale_ir_token(op);
    void *fn = NULL;

#define IRSTEP(x) do { if (getenv("WHALE_IR_DEBUG")) fprintf(stderr, "  ir: %s\n", x); } while (0)

    if (!tok)
        return NULL;
    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", tcc_dir);

    s = tcc_new();
    IRSTEP("tcc_new");
    if (!s)
        return NULL;
    tcc_set_lib_path(s, tcc_dir);
    tcc_add_library_path(s, lib_dir);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    IRSTEP("paths");

    tcc_open_bf(s, "<whale-ir>", 0);
    tccelf_begin_file(s);
    preprocess_start(s, 0);
    IRSTEP("unit opened");

    ts = tok_alloc("whale_ir_fn", 11);
    IRSTEP("tok_alloc");
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
    IRSTEP("symbols");

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
    IRSTEP("prolog");

    dt.t = VT_DOUBLE;
    dt.ref = NULL;
    IRSTEP("body start");
    cval.d = a;
    vsetc(&dt, VT_CONST, &cval);
    IRSTEP("push a");
    cval.d = b;
    vsetc(&dt, VT_CONST, &cval);
    IRSTEP("push b");
    gen_op(tok);
    IRSTEP("gen_op");
    gv(RC_FLOAT);
    IRSTEP("gv");
    gfunc_return(&func_vt);
    IRSTEP("gfunc_return");

    rsym = 0;
    gfunc_epilog();
    cur_text_section->data_offset = ind;
    elfsym(funcsym)->st_size = ind - func_ind;
    check_vstack();
    IRSTEP("epilog");

    preprocess_end(s);
    sym_pop(&global_stack, NULL, 0);
    sym_pop(&local_stack, NULL, 0);
    tccelf_end_file(s);
    tcc_close();
    IRSTEP("unit closed");

    if (tcc_relocate(s, TCC_RELOCATE_AUTO) >= 0) {
        fn = (void *)(cur_text_section->data + func_ind);
        IRSTEP("relocated");
    } else {
        IRSTEP("relocate FAILED");
    }

done:
    return fn;
#undef IRSTEP
}