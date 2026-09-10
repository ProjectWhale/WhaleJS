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

/* define_stack snapshot for free_defines() at teardown (see tcc_compile()). */
static Sym *whale_define_start;

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
        whale_define_start = define_stack;                                 \
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
        free_inline_functions((s));                                         \
        free_defines(whale_define_start);                                   \
        sym_pop(&global_stack, NULL, 0);                                    \
        sym_pop(&local_stack, NULL, 0);                                     \
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
    tcc_debug_funcstart(tcc_state, funcsym);
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
    gsym(rsym);
    gfunc_epilog();
    cur_text_section->data_offset = ind;
    label_pop(&global_label_stack, NULL, 0);
    elfsym(funcsym)->st_size = ind - func_ind;
    tcc_debug_funcend(tcc_state, ind - func_ind);
    func_vt.t = VT_VOID;
    func_var = 0;
    nocode_wanted = 0x80000000;
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
/* ------------------------------------------------------------------------
 * Control experiment: the SAME function, same name and signature, produced by
 * the working C-source path. Diffing its bytes against the IR version is the
 * fastest way to find what IR emission is missing - and calling both tells us
 * whether the fault is in the code or in the state.
 * ---------------------------------------------------------------------- */
void *JS_WhaleIrAdd2C(const char *tcc_dir)
{
    char lib_dir[1024];
    TCCState *s;
    void *fn = NULL;
    const char *src = "double whale_ir_add(double a, double b) { return a + b; }";

    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", tcc_dir);
    s = tcc_new();
    if (!s)
        return NULL;
    tcc_set_lib_path(s, tcc_dir);
    tcc_add_library_path(s, lib_dir);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    if (tcc_compile_string(s, src) < 0)
        return NULL;

    if (getenv("WHALE_IR_DEBUG")) {
        int i;
        Section *tsec = NULL;
        /* find the text section by name without relying on the global */
        for (i = 0; i < s->nb_sections; i++) {
            if (s->sections[i]->sh_type == 1 /* SHT_PROGBITS */ &&
                (s->sections[i]->sh_flags & 0x4) /* SHF_EXECINSTR */) {
                tsec = s->sections[i];
                break;
            }
        }
        if (tsec) {
            int n = tsec->data_offset > 80 ? 80 : (int)tsec->data_offset;
            fprintf(stderr, "  irC: c-source code %d bytes:", n);
            for (i = 0; i < n; i++)
                fprintf(stderr, " %02x", tsec->data[i]);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  irC: no text section found\n");
        }
    }

    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0)
        return NULL;
    fn = tcc_get_symbol(s, "whale_ir_add");
    if (getenv("WHALE_IR_DEBUG"))
        fprintf(stderr, "  irC: entry=%p\n", fn);
    return fn;
}
/* Dump the first bytes of the text section - only the interface file can see
   Section/TCCState internals, so whale_jit.c asks through here. */
void JS_WhaleIrDumpText(TCCState *s, const char *tag)
{
    Section *ts = NULL;
    int k, n;

    if (!s)
        return;
    for (k = 0; k < s->nb_sections; k++) {
        Section *sec = s->sections[k];
        if (sec->sh_type == 1 /* SHT_PROGBITS */ && (sec->sh_flags & 0x4) /* SHF_EXECINSTR */) {
            ts = sec;
            break;
        }
    }
    if (!ts) {
        fprintf(stderr, "  irDump[%s]: no text section\n", tag);
        return;
    }
    n = ts->data_offset > 96 ? 96 : (int)ts->data_offset;
    fprintf(stderr, "  irDump[%s]: %d bytes:", tag, n);
    for (k = 0; k < n; k++)
        fprintf(stderr, " %02x", ts->data[k]);
    fprintf(stderr, "\n");
}
/* ========================================================================
 * WhaleJS: custom token source
 *
 * This is the piece that makes WhaleJS a self-contained library: instead of
 * handing TinyCC C source to lex (which would force us to include quickjs.h and
 * carry headers around), the JIT builds a TOKEN stream and TinyCC's own parser
 * consumes it. Everything after the lexer - decl(), block(), gen_function(),
 * gen_op() - runs untouched, so all the parser state is correct by construction.
 * ======================================================================== */

int whale_tok_active;
static int whale_tok_buf[16384];
static double whale_tok_num[16384];
static int whale_tok_len;
static int whale_tok_pos;

static int whale_tok_ident(const char *name)
{
    return tok_alloc(name, strlen(name))->tok;
}

static void whale_tok_push(int t)
{
    if (whale_tok_len < (int)(sizeof(whale_tok_buf) / sizeof(whale_tok_buf[0])))
        whale_tok_buf[whale_tok_len++] = t;
}

/* ---- interface for the JIT generator: it pushes tokens directly ---- */

void JS_WhaleTokReset(void)
{
    whale_tok_len = 0;
    whale_tok_pos = 0;
}

/* val is used only for TOK_CINT / TOK_CDOUBLE */
void JS_WhaleTokPush(int t, double val)
{
    if (whale_tok_len >= (int)(sizeof(whale_tok_buf) / sizeof(whale_tok_buf[0])))
        return;
    whale_tok_num[whale_tok_len] = val;
    whale_tok_buf[whale_tok_len++] = t;
}

int JS_WhaleTokIdent(const char *name)
{
    return whale_tok_ident(name);
}

/* called by next_nomacro() in place of the lexer */
void whale_next_token(void)
{
    tok = (whale_tok_pos < whale_tok_len) ? whale_tok_buf[whale_tok_pos] : TOK_EOF;
    if (tok == TOK_CINT)
        tokc.i = (int)whale_tok_num[whale_tok_pos];
    else if (tok == TOK_CDOUBLE)
        tokc.d = whale_tok_num[whale_tok_pos];
    whale_tok_pos++;
    tokc.i = 0;
    tokc.d = 0;
    tok_flags = 0;
}

/* Build `double whale_ir_add(double a, double b) { return a + b; }` as tokens.
   Nothing but built-in types appear here, which is the whole point. */
static void whale_tok_build_add2(void)
{
    whale_tok_len = 0;
    whale_tok_pos = 0;
    whale_tok_push(TOK_DOUBLE);
    whale_tok_push(whale_tok_ident("whale_ir_add"));
    whale_tok_push('(');
    whale_tok_push(TOK_DOUBLE);
    whale_tok_push(whale_tok_ident("whale_a"));
    whale_tok_push(',');
    whale_tok_push(TOK_DOUBLE);
    whale_tok_push(whale_tok_ident("whale_b"));
    whale_tok_push(')');
    whale_tok_push('{');
    whale_tok_push(TOK_RETURN);
    whale_tok_push(whale_tok_ident("whale_a"));
    whale_tok_push('+');
    whale_tok_push(whale_tok_ident("whale_b"));
    whale_tok_push(';');
    whale_tok_push('}');
}

/* Compile that function by driving TinyCC's parser with our tokens. */
void *JS_WhaleTokCompileAdd2(const char *tcc_dir)
{
    char lib_dir[1024];
    TCCState *s;
    void *fn = NULL;

    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", tcc_dir);
    s = tcc_new();
    if (!s)
        return NULL;
    tcc_set_lib_path(s, tcc_dir);
    tcc_add_library_path(s, lib_dir);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    /* Need the token table: open the unit the normal way first, then let our
       stream take over before any real parsing happens. */
    tcc_open_bf(s, "<whale-tok>", 0);
    tccelf_begin_file(s);
    preprocess_start(s, 0);
    tcc_debug_start(s);

    whale_tok_build_add2();
    whale_tok_active = 1;

    tccgen_compile(s);          /* the real parser, fed by our tokens */
    whale_tok_active = 0;

    preprocess_end(s);
    free_inline_functions(s);
    sym_pop(&global_stack, NULL, 0);
    tccelf_end_file(s);
    tcc_close();

    if (tcc_relocate(s, TCC_RELOCATE_AUTO) >= 0)
        fn = tcc_get_symbol(s, "whale_ir_add");
    return fn;
}
/* ------------------------------------------------------------------------
 * Compile by driving TinyCC's parser with the JIT's token stream.
 * No source text is parsed and no header is read: everything after
 * next_nomacro() (decl/block/gen_function/gen_op) runs untouched, so parser
 * state is correct by construction. Returns 1 on success.
 * ---------------------------------------------------------------------- */
void JS_WhaleTokDump(void);

int JS_WhaleCompileTokens(TCCState *s, void (*emit)(void *), void *emit_ctx)
{
    if (!s)
        return 0;
    if (setjmp(s->error_jmp_buf) != 0) {
        s->error_set_jmp_enabled = 0;
        return 0;
    }
    s->nb_errors = 0;
    s->error_set_jmp_enabled = 1;

    tcc_open_bf(s, "<whale-jit>", 0);
    tccelf_begin_file(s);
    preprocess_start(s, 0);
    tcc_debug_start(s);

    /* tokens can only be created now: tok_alloc needs the unit */
    JS_WhaleTokReset();
    if (emit)
        emit(emit_ctx);

    whale_tok_pos = 0;
    whale_tok_active = 1;

    if (getenv("WHALE_IR_DEBUG"))
        JS_WhaleTokDump();

    tccgen_compile(s);

    whale_tok_active = 0;
    preprocess_end(s);
    free_inline_functions(s);
    tccelf_end_file(s);
    tcc_close();

    s->error_set_jmp_enabled = 0;
    return s->nb_errors == 0;
}
void JS_WhaleTokKw(int w)
{
    static const int kwtok[] = {
        TOK_INT, TOK_CHAR, TOK_VOID, TOK_LONG, TOK_UNSIGNED, TOK_FLOAT,
        TOK_DOUBLE, TOK_STATIC, TOK_EXTERN, TOK_TYPEDEF, TOK_STRUCT, TOK_RETURN,
        TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_DO, TOK_BREAK, TOK_CONTINUE,
        TOK_GOTO, TOK_SWITCH, TOK_CASE, TOK_DEFAULT, TOK_SIZEOF, TOK_REGISTER,
        TOK_UNION, TOK_ENUM, TOK_SHORT,
    };
    if (w >= 0 && w < (int)(sizeof(kwtok) / sizeof(kwtok[0])))
        JS_WhaleTokPush(kwtok[w], 0);
}

void JS_WhaleTokIdent2(const char *name)
{
    JS_WhaleTokPush(whale_tok_ident(name), 0);
}

void JS_WhaleTokSlot(const char *prefix, int n)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%s%d", prefix, n);
    JS_WhaleTokPush(whale_tok_ident(buf), 0);
}

void JS_WhaleTokInt(int v)
{
    JS_WhaleTokPush(TOK_CINT, (double)v);
}

void JS_WhaleTokDouble(double v)
{
    JS_WhaleTokPush(TOK_CDOUBLE, v);
}

void JS_WhaleTokChar(int ch)
{
    JS_WhaleTokPush(ch, 0);
}
void JS_WhaleTokOp(int o)
{
    static const int optok[] = { '+', '-', '*', '/', TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_EQ, TOK_NE, TOK_LOR, TOK_LAND };

    if (o >= 0 && o < (int)(sizeof(optok) / sizeof(optok[0])))
        JS_WhaleTokPush(optok[o], 0);
}

/* Diagnostics: reconstruct the token stream as text, so it can be compared with
   the C it is supposed to be equivalent to. */
void JS_WhaleTokDump(void)
{
    int k;

    for (k = 0; k < whale_tok_len; k++) {
        int t = whale_tok_buf[k];
        const char *nm = get_tok_str(t, NULL);
        if (t == TOK_CINT)
            fprintf(stderr, "%d ", (int)whale_tok_num[k]);
        else if (t == TOK_CDOUBLE)
            fprintf(stderr, "%.17g ", whale_tok_num[k]);
        else if (nm && t < 256)
            fprintf(stderr, "%s ", nm);
        else if (nm)
            fprintf(stderr, "%s ", nm);
        else
            fprintf(stderr, "<?%d> ", t);
    }
    fprintf(stderr, "\n");
}
