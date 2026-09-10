#include "libtcc.c"

/* ========================================================================
 * WhaleJS: TinyCC internal interface
 *
 * The whole TinyCC is included above, so this file is one translation unit that
 * has both the compiler and our interface in scope. libtcc.c does not include
 * this file the other way round: xmake cannot see #include dependencies, so the
 * add_files() entry is the only file whose changes trigger a rebuild - and both
 * files change often, so the entry has to be this one.
 *
 * This is the route from notes <16>: drive TinyCC's internals directly instead
 * of handing it C source to parse. It skips C parsing and needs no include path
 * at all (the quickjs.h header-path problem disappears), and it gives us the IR
 * to optimise on later.
 * ======================================================================== */

/* Probe: proves we really are compiled inside TinyCC's translation unit. */
int JS_WhaleTccProbe(void)
{
    return (int)(sizeof(__vstack) / sizeof(__vstack[0])) - 1;
}

/* ------------------------------------------------------------------------
 * One translation unit, opened and closed the way tcc_compile() does
 * (libtcc.c:623): tcc_open_bf, tccelf_begin_file, preprocess_start, [emit],
 * preprocess_end, tccelf_end_file, tcc_close, tcc_relocate.
 *
 * Two things tcc_compile() does that must be reproduced:
 *  - it installs s->error_jmp_buf. tcc_error() longjmps, so without a setjmp
 *    target any reported error is an instant crash instead of a clean failure.
 *  - it runs the prologue of tccgen_compile() (tccgen.c:235), which sets the
 *    parser flags and the canonical global types (int_type, size_type,
 *    ptrdiff_type, func_old_type). gen_op()/gv() consult those internally.
 * ---------------------------------------------------------------------- */

/* The framing shared by the emitters below. Returns 0 when the unit is open. */
#define WHALE_IR_SETUP(s, tcc_dir, tag)                                     \
    do {                                                                    \
        char whale_lib[1024];                                               \
        snprintf(whale_lib, sizeof(whale_lib), "%s/lib", (tcc_dir));        \
        (s) = tcc_new();                                                    \
        if (!(s))                                                           \
            return NULL;                                                    \
        tcc_set_lib_path((s), (tcc_dir));                                   \
        tcc_add_library_path((s), whale_lib);                               \
        tcc_set_output_type((s), TCC_OUTPUT_MEMORY);                        \
        if (setjmp((s)->error_jmp_buf) != 0) {                              \
            (s)->error_set_jmp_enabled = 0;                                 \
            goto done;                                                      \
        }                                                                   \
        (s)->nb_errors = 0;                                                 \
        (s)->error_set_jmp_enabled = 1;                                     \
        tcc_open_bf((s), (tag), 0);                                         \
        tccelf_begin_file((s));                                             \
        preprocess_start((s), 0);                                           \
        cur_text_section = NULL;                                            \
        funcname = "";                                                      \
        anon_sym = SYM_FIRST_ANOM;                                          \
        section_sym = 0;                                                    \
        const_wanted = 0;                                                   \
        nocode_wanted = 0x80000000;                                         \
        int_type.t = VT_INT;                                                \
        char_pointer_type.t = VT_BYTE;                                      \
        mk_pointer(&char_pointer_type);                                     \
        size_type.t = VT_LLONG | VT_UNSIGNED;                               \
        ptrdiff_type.t = VT_LLONG;                                          \
        func_old_type.t = VT_FUNC;                                          \
        func_old_type.ref = sym_push(SYM_FIELD, &int_type, 0, 0);           \
        func_old_type.ref->f.func_call = FUNC_CDECL;                        \
        func_old_type.ref->f.func_type = FUNC_OLD;                          \
        tcc_debug_start((s));                                               \
    } while (0)

/* Close it and turn the emitted code into a callable pointer. */
#define WHALE_IR_FINISH(s, name, out)                                       \
    do {                                                                    \
        preprocess_end((s));                                                \
        tccelf_end_file((s));                                               \
        tcc_close();                                                        \
        if (tcc_relocate((s), TCC_RELOCATE_AUTO) >= 0) {                    \
            (out) = tcc_get_symbol((s), (name));                            \
            if ((out)) {                                                    \
                DWORD whale_old;                                            \
                VirtualProtect((out), 4096, PAGE_EXECUTE_READWRITE, &whale_old); \
            }                                                               \
        }                                                                   \
        (s)->error_set_jmp_enabled = 0;                                     \
    } while (0)

/* Emit `double whale_ir_add(double a, double b) { return a + b; }`.
   The entry point of the whole route: with parameters there is no constant
   pool involved, so this isolates "does IR emission produce callable code". */
void *JS_WhaleIrCompileAdd2(const char *tcc_dir)
{
    TCCState *s = NULL;
    CType ft, dt;
    Sym *retsym, *funcsym, *pa, *pb;
    CValue cval;
    TokenSym *ta, *tb, *tf;
    void *volatile fn = NULL;

    WHALE_IR_SETUP(s, tcc_dir, "<whale-ir>");

    tf = tok_alloc("whale_ir_add", 12);
    ta = tok_alloc("whale_a", 7);
    tb = tok_alloc("whale_b", 7);
    if (!tf || !ta || !tb)
        goto done;

    /* the parameter chain hangs off the return-type symbol: retsym -> a -> b */
    pb = sym_push2(&local_stack, tb->tok, VT_DOUBLE, 0);
    pb->type.t = VT_DOUBLE;
    pb->type.ref = NULL;
    pb->next = NULL;

    pa = sym_push2(&local_stack, ta->tok, VT_DOUBLE, 0);
    pa->type.t = VT_DOUBLE;
    pa->type.ref = NULL;
    pa->next = pb;

    retsym = sym_push2(&local_stack, 0, VT_DOUBLE, 0);
    retsym->type.t = VT_DOUBLE;
    retsym->type.ref = NULL;
    retsym->next = pa;

    ft.t = VT_FUNC;
    ft.ref = retsym;
    funcsym = sym_push2(&local_stack, tf->tok, VT_FUNC, 0);
    funcsym->type = ft;
    funcsym->r = VT_CONST;

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
    if (getenv("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: before prolog: func.ref=%p retsym=%p retsym->next=%p pa=%p\n",
                (void *)funcsym->type.ref, (void *)retsym, (void *)retsym->next, (void *)pa);
    sym_push2(&local_stack, SYM_FIELD, 0, 0);
    local_scope = 1;
    gfunc_prolog(&funcsym->type);
    local_scope = 0;
    if (getenv("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: after prolog: loc=%d pa(r=%x c=%d) pb(r=%x c=%d)\n",
                loc, pa->r, (int)pa->c, pb->r, (int)pb->c);

    /* a and b are locals now; gfunc_prolog put their stack offsets in ->c */
    if (getenv("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: pa(r=%x c=%d) pb(r=%x c=%d) loc=%d\n",
                pa->r, (int)pa->c, pb->r, (int)pb->c, loc);
    dt.t = VT_DOUBLE;
    dt.ref = NULL;
    /* gfunc_prolog resolved the parameters by pushing fresh locals keyed on
       their token - our own parameter nodes are untouched - so look them up by
       token to get the stack offsets it assigned (PE/x86-64: positive offsets). */
    pa = sym_find(ta->tok);
    pb = sym_find(tb->tok);
    if (getenv("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: a(r=%x c=%d) b(r=%x c=%d) loc=%d\n",
                pa ? pa->r : 0, pa ? (int)pa->c : -1,
                pb ? pb->r : 0, pb ? (int)pb->c : -1, loc);
    if (!pa || !pb)
        goto done;
    cval.i = pa->c;
    vsetc(&dt, VT_LOCAL | VT_LVAL, &cval);
    cval.i = pb->c;
    vsetc(&dt, VT_LOCAL | VT_LVAL, &cval);
    gen_op('+');
    /* no explicit gv(): gfunc_return() fetches the value with the right class */
    gfunc_return(&func_vt);

    rsym = 0;
    gfunc_epilog();
    cur_text_section->data_offset = ind;
    elfsym(funcsym)->st_size = ind - func_ind;
    check_vstack();
    if (getenv("WHALE_IR_DEBUG")) {
        int i, n = ind - func_ind;
        fprintf(stderr, "  ir2: code [%d,%d) %d bytes:", func_ind, ind, n);
        for (i = func_ind; i < ind && i < func_ind + 56; i++)
            fprintf(stderr, " %02x", text_section->data[i]);
        fprintf(stderr, "\n");
    }

    WHALE_IR_FINISH(s, "whale_ir_add", fn);

done:
    return fn;
}