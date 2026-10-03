/* Diagnostics live behind WHALE_DEBUG (see the file): include it before libtcc.c
   so every upstream file pulled in below sees the same definition. */
#include "../whale_dbg.h"
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

/* tcc_add_file() loads an object without going through JS_WhaleCompileTokens,
   whose epilogue clears runtime_main (see its comment there). Without the
   same clearing, the next pe_output_file() takes runtime_main as "run a
   program" and pe_add_runtime() injects __runmain plus the CRT libraries -
   whose symbols nobody can resolve, failing the compile. The gcc backend
   calls this after loading its object. */
void JS_WhaleTccClearRuntimeMain(TCCState *s)
{
    if (s)
        s->runtime_main = NULL;
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
    if (whale_dbg_env("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: before prolog: func.ref=%p retsym=%p retsym->next=%p pa=%p\n",
                (void *)funcsym->type.ref, (void *)retsym, (void *)retsym->next, (void *)pa);
    tcc_debug_funcstart(tcc_state, funcsym);
    sym_push2(&local_stack, SYM_FIELD, 0, 0);
    local_scope = 1;
    gfunc_prolog(&funcsym->type);
    local_scope = 0;
    if (whale_dbg_env("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: after prolog: loc=%d pa(r=%x c=%d) pb(r=%x c=%d)\n",
                loc, pa->r, (int)pa->c, pb->r, (int)pb->c);

    /* a and b are locals now; gfunc_prolog put their stack offsets in ->c */
    if (whale_dbg_env("WHALE_IR_DEBUG"))
        fprintf(stderr, "  ir2: pa(r=%x c=%d) pb(r=%x c=%d) loc=%d\n",
                pa->r, (int)pa->c, pb->r, (int)pb->c, loc);
    dt.t = VT_DOUBLE;
    dt.ref = NULL;
    /* gfunc_prolog resolved the parameters by pushing fresh locals keyed on
       their token - our own parameter nodes are untouched - so look them up by
       token to get the stack offsets it assigned (PE/x86-64: positive offsets). */
    pa = sym_find(ta->tok);
    pb = sym_find(tb->tok);
    if (whale_dbg_env("WHALE_IR_DEBUG"))
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
    if (whale_dbg_env("WHALE_IR_DEBUG")) {
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

    if (whale_dbg_env("WHALE_IR_DEBUG")) {
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
    if (whale_dbg_env("WHALE_IR_DEBUG"))
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
/* Set by the generator callback: 0 means it bailed on an opcode it cannot lower,
   so the tokens emitted so far form a truncated function. */
int whale_gen_ok = 1;
/* Also set by the generator: 1 when the function it emitted never reads a
   global. Such a function's result depends only on its arguments, which is what
   lets the engine memoise it (see whale_memo in quickjs.c). */
int whale_gen_pure = 1;
/* Also set by the generator: how many frame object slots the emitted function
   uses. The engine only has to initialise and release those slots when this is
   non-zero, which keeps plain numeric functions (fib, arith, ...) from paying
   for machinery they never touch. */
int whale_gen_objs = 0;
/* Which reading of a closure slot that feeds a call to compile. 1 = the slot is
   this function (self-recursion); 0 = it is an enclosing closure's value. The
   bytecode is identical, so the engine compiles both and lets the entry check
   decide which one is running. */
int whale_assume_self = 1;
static int whale_tok_buf[16384];
static double whale_tok_num[16384];
static int whale_tok_len;
static int whale_tok_pos;

/* ---- generator-owned token workspace ----------------------------------
   The JIT generator folds stack-slot values into expressions before the final
   token order is known, so it needs a workspace it can append to and then
   linearise. The encoders below append there; JS_WhaleTokCommit() copies the
   finished stream into the buffer the parser reads. Without a workspace
   installed the encoders write the parser buffer directly, as before. */
static int *whale_ws_tok;
static double *whale_ws_num;
static int whale_ws_cap;
static int whale_ws_len;

void JS_WhaleTokSink(int *tok, double *num, int cap)
{
    whale_ws_tok = tok;
    whale_ws_num = num;
    whale_ws_cap = cap;
    whale_ws_len = 0;
}

int JS_WhaleTokSinkLen(void)
{
    return whale_ws_len;
}

/* Drop everything appended since position n, leaving the workspace exactly as it
   was at that point. An inline attempt can fail after having emitted tokens - the
   callee turns out to have locals, or holds an instruction the inliner does not
   handle - and the caller then has to fall back to a plain call, so the
   half-emitted expansion has to go away. */
void JS_WhaleTokSinkTruncate(int n)
{
    if (n >= 0 && n <= whale_ws_len)
        whale_ws_len = n;
}

/* Publish the first n workspace tokens as the stream the parser reads. */
void JS_WhaleTokCommit(int *tok, double *num, int n)
{
    int i, cap = (int)(sizeof(whale_tok_buf) / sizeof(whale_tok_buf[0]));

    if (n > cap)
        n = cap;
    for (i = 0; i < n; i++) {
        whale_tok_num[i] = num[i];
        whale_tok_buf[i] = tok[i];
    }
    whale_tok_len = n;
    whale_tok_pos = 0;
}

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
    if (whale_ws_tok) {
        /* The generator owns the stream while it folds expressions: it buffered
           the whole function before the parser ever runs, so the encoders append
           to its workspace and JS_WhaleTokCommit() publishes the result. */
        if (whale_ws_len >= whale_ws_cap)
            return;
        whale_ws_num[whale_ws_len] = val;
        whale_ws_tok[whale_ws_len++] = t;
        return;
    }
    if (whale_tok_len >= (int)(sizeof(whale_tok_buf) / sizeof(whale_tok_buf[0])))
        return;
    whale_tok_num[whale_tok_len] = val;
    whale_tok_buf[whale_tok_len++] = t;
}

int JS_WhaleTokIdent(const char *name)
{
    return whale_tok_ident(name);
}

/* The generated-source mirror (WHALE_JIT_SRC).

   This is where the token stream is consumed by TinyCC, in its final order and
   with its final contents, so it is the only place a faithful text rendering can
   be taken. The mirror used to be written from the emitter helpers instead
   (wt/wt_id/wt_int/...), which records an expression while it is still pending in
   the fold sink: fold_commit() moves it into the output afterwards, so the text
   came out in the wrong order, and anything pushed straight to the sink by
   fold_inline_expr() - which bypasses the helpers entirely - did not appear at
   all. Reading that file as if it were the C gave `return` with no `return`, `a b`
   where the code was `a + b`, and `= ( )` where a whole folded expression stood. */
/* ---- the C renderer ------------------------------------------------------
   The generator produces a token stream, not text. TinyCC consumes that stream
   through whale_next_token(), but anything else that wants the function as C -
   the WHALE_JIT_SRC dump, and a gcc backend whose driver only reads text - has
   to render it. The finished stream sits in the generator's go_tok/go_num in
   final order, so rendering walks that array; it never has to observe the
   stream as TinyCC pulls it, which is what tied this to TinyCC's own consume
   point before.

   The text is built in memory because a gcc backend hands it to the driver's
   stdin pipe - it is not written to a file.
   ======================================================================== */
/* The tokens whose spelling is not a plain ASCII character. get_tok_str() knows them too, but on
   this token it does not find the entry: TOK_NE came back empty and the token
   reached the file as a raw 0x95, which is how the `%` guard every case emits -
   `(v[0] != 0) && ...` - turned the generated C into something gcc will not
   parse. Matching on the token constant rather than on the spelling cannot
   drift from the compiler. */
/* The spelling of a token that is not a plain ASCII character. Matching on the
   token constant rather than on a value range is what makes this correct:
   `<=`, `==`, `!=` and `&&` sit above 128, but `<<` is 1 and `>>` is 2, and
   `<` and `>` have their own constants above 128 as well - TinyCC needs those
   apart from `<<`/`<=` and `>>`/`>=` for longest-match, so they are single
   characters that do not live in ASCII here. Writing any of them with fputc()
   put a raw byte in the file. */
static const char *whale_tok_spelling(int tok)
{
    switch (tok) {
    case TOK_LT:    return "<";
    case TOK_GT:    return ">";
    case TOK_LE:    return "<=";
    case TOK_GE:    return ">=";
    case TOK_NE:    return "!=";
    case TOK_LAND:  return "&&";
    case TOK_LOR:   return "||";
    case TOK_INC:   return "++";
    case TOK_DEC:   return "--";
    case TOK_EQ:    return "==";
    case TOK_SHL:   return "<<";
    case TOK_SAR:   return ">>";
    case TOK_A_ADD: return "+=";
    case TOK_A_SUB: return "-=";
    case TOK_A_MUL: return "*=";
    case TOK_A_DIV: return "/=";
    case TOK_A_MOD: return "%=";
    case TOK_A_AND: return "&=";
    case TOK_A_XOR: return "^=";
    case TOK_A_OR:  return "|=";
    case TOK_ARROW: return "->";
    default:        return NULL;
    }
}
typedef struct WhaleText {
    char *p;
    int   len;
    int   cap;
} WhaleText;

static void whale_text_put(WhaleText *t, const char *s, int n)
{
    if (!t->p) {
        t->cap = 1 << 16;
        t->p = (char *)tcc_malloc(t->cap);
        t->p[0] = '\0';
    }
    if (t->len + n + 1 > t->cap) {
        while (t->len + n + 1 > t->cap)
            t->cap *= 2;
        t->p = (char *)tcc_realloc(t->p, t->cap);
    }
    if (n > 0)
        memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

static void whale_text_puts(WhaleText *t, const char *s)
{
    whale_text_put(t, s, (int)strlen(s));
}

static void whale_text_putc(WhaleText *t, int c)
{
    char ch = (char)c;

    whale_text_put(t, &ch, 1);
}

/* One token as C. This is the whole of the rendering; everything above is
   buffer plumbing and everything below is a caller. */
static void whale_render_one(WhaleText *t, int tok, double num)
{
    char tmp[64];

    if (tok == TOK_EOF)
        return;                 /* the end marker is not C */
    /* Order matters: TOK_IDENT (256) and the CINT/CDOUBLE pseudo-tokens sit
       ABOVE 128, so they have to be recognised before anything reads the value
       as a character. */
    if (tok == TOK_CINT) {
        snprintf(tmp, sizeof(tmp), "%d ", (int)num);
        whale_text_puts(t, tmp);
        return;
    }
    if (tok == TOK_CDOUBLE) {
        snprintf(tmp, sizeof(tmp), "%.17g ", num);
        whale_text_puts(t, tmp);
        return;
    }
    if (tok >= TOK_IDENT) {
        const char *nm = get_tok_str(tok, NULL);

        whale_text_puts(t, nm ? nm : "?");
        whale_text_putc(t, ' ');
        return;
    }
    /* A multi-character token is NOT identified by its value: `<=`, `==` and
       `!=` live above 128, but `<<` is token 1 and `>>` is token 2, inside
       ASCII, so a value test would write those as raw bytes. The token constant
       is what actually identifies them. */
    {
        const char *ops = whale_tok_spelling(tok);

        if (ops) {
            whale_text_puts(t, ops);
            whale_text_putc(t, ' ');
            return;
        }
    }
    if (tok > 0) {
        whale_text_putc(t, tok);
        whale_text_putc(t, (tok == ';' || tok == '{' || tok == '}') ? '\n' : ' ');
    } else {
        snprintf(tmp, sizeof(tmp), "T%d ", tok);
        whale_text_puts(t, tmp);
    }
}

/* Render a whole finished stream. Returns the length and stores a malloc'd,
   NUL-terminated buffer in *out for the caller to free. */
/* Release a buffer JS_WhaleRenderStream returned: it comes from the compiler's
   own allocator, so it has to go back to that one. */
void JS_WhaleRenderFree(char *p)
{
    tcc_free(p);
}
int JS_WhaleRenderStream(const int *tok, const double *num, int n, char **out)
{
    WhaleText t;
    int i;

    memset(&t, 0, sizeof(t));
    for (i = 0; i < n; i++)
        whale_render_one(&t, tok[i], num[i]);
    *out = t.p;
    return t.len;
}
static FILE *whale_mirror_fp;
static int whale_mirror_tried;

static WhaleText whale_mirror_text;

static void whale_mirror_write(int t, double num)
{
    if (!whale_mirror_tried) {
#ifdef WHALE_DEBUG
        const char *p = getenv("WHALE_JIT_SRC");

        whale_mirror_tried = 1;
        if (p && *p)
            whale_mirror_fp = fopen(p, "w");
#else
        whale_mirror_tried = 1;
#endif
    }
    if (!whale_mirror_fp)
        return;
    whale_render_one(&whale_mirror_text, t, num);
    if (whale_mirror_text.len > 0) {
        fwrite(whale_mirror_text.p, 1, whale_mirror_text.len, whale_mirror_fp);
        whale_mirror_text.len = 0;
        whale_mirror_text.p[0] = '\0';
    }
    fflush(whale_mirror_fp);
}

/* called by next_nomacro() in place of the lexer */
void whale_next_token(void)
{
    tok = (whale_tok_pos < whale_tok_len) ? whale_tok_buf[whale_tok_pos] : TOK_EOF;
    tokc.i = 0;
    tokc.d = 0;
    if (tok == TOK_CINT)
        tokc.i = (int)whale_tok_num[whale_tok_pos];
    else if (tok == TOK_CDOUBLE)
        tokc.d = whale_tok_num[whale_tok_pos];
    whale_mirror_write(tok, whale_tok_num[whale_tok_pos]);
    whale_tok_pos++;
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

    /* Guard exactly like JS_WhaleCompileTokens: the lexer must be off before
       anything defines a builtin macro, and any error must not leave it on. */
    whale_tok_active = 0;
    if (setjmp(s->error_jmp_buf) != 0) {
        whale_tok_active = 0;
        s->error_set_jmp_enabled = 0;
        return NULL;
    }
    s->nb_errors = 0;
    s->error_set_jmp_enabled = 1;

    tcc_open_bf(s, "whale_tok.c", 1);
    file->buffer[0] = 0;   /* empty unit, not garbage */
    file->buf_ptr = file->buffer;
    tccelf_begin_file(s);
    preprocess_start(s, 0);
    tcc_debug_start(s);

    whale_tok_build_add2();      /* fills and resets the stream */
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
    /* the lexer must be off until we have tokens to give it: a previous run that
       longjmp'ed out of here could have left it on, and preprocess_start() would
       then consume our (empty) token stream */
    whale_tok_active = 0;

    /* Snapshot the macro stack before setjmp: the error path below unwinds with
       it, and it must never still hold a previous unit's value. */
    whale_define_start = define_stack;

    if (setjmp(s->error_jmp_buf) != 0) {
        /* WhaleJS: point at the token the parser was on when it failed. Without
           this the only clue is "whale_jit.c:1: error: ...", and the position in
           the token stream - the one piece of information that identifies which
           emitted construct is malformed - is gone. */
        {
            int k, from = (whale_tok_pos > 8) ? whale_tok_pos - 8 : 0;

            fprintf(stderr, "[tokerr] failed at token %d of %d:\n",
                    whale_tok_pos, whale_tok_len);
            /* Every identifier that looks like a label, with its position. A
               "duplicate label" is only diagnosable if both definitions are visible,
               and the 8-token window below shows at most one of them. */
            if (whale_dbg_env("WHALE_OP_DBG")) {
                for (k = 0; k < whale_tok_len; k++) {
                    int t = whale_tok_buf[k];
                    const char *nm;

                    if (t < TOK_IDENT)
                        continue;
                    nm = get_tok_str(t, NULL);
                    if (nm && nm[0] == 'L' && nm[1] >= '0' && nm[1] <= '9') {
                        int b;

                        fprintf(stderr, "  [L] %d: %s  ctx:", k, nm);
                        for (b = (k > 4) ? k - 4 : 0; b < k + 3 && b < whale_tok_len; b++)
                            fprintf(stderr, " %d", whale_tok_buf[b]);
                        fprintf(stderr, "\n");
                    }
                }
            }
            for (k = from; k < whale_tok_len && k < whale_tok_pos + 4; k++) {
                int t = whale_tok_buf[k];
                const char *mark = (k + 1 == whale_tok_pos) ? " <<<" : "";

                if (t == TOK_CINT)
                    fprintf(stderr, "  %d%s: INT %d\n", k, mark,
                            (int)whale_tok_num[k]);
                else if (t == TOK_CDOUBLE)
                    fprintf(stderr, "  %d%s: DBL %.17g\n", k, mark,
                            whale_tok_num[k]);
                else if (t > 0 && t < 256)
                    fprintf(stderr, "  %d%s: '%c' (%d)\n", k, mark,
                            (t >= 32 && t < 127) ? t : '?', t);
                else {
                    const char *nm = (t >= TOK_IDENT) ? get_tok_str(t, NULL) : NULL;

                    fprintf(stderr, "  %d%s: %s (%d)\n", k, mark,
                            nm ? nm : "?", t);
                }
            }
        }
        /* tcc_error() longjmps straight here, so the teardown at the end of this
           function never runs. Do the same unwinding on this path: otherwise the
           symbol/label stacks keep this unit's entries and the next compile in
           the process trips over them (label_pop() would then define labels
           through put_extern_sym() against a symbol table that no longer exists). */
        whale_tok_active = 0;
        free_defines(whale_define_start);
        sym_pop(&global_stack, NULL, 0);
        sym_pop(&local_stack, NULL, 0);
        global_label_stack = NULL;
        local_label_stack = NULL;
        s->error_set_jmp_enabled = 0;
        return 0;
    }
    s->nb_errors = 0;
    s->error_set_jmp_enabled = 1;

    /* len only sizes the char buffer; we read no characters (tokens come from the hook),
       but the buffer has to exist for preprocess_start/parser init. */
    tcc_open_bf(s, "whale_jit.c", 1);
    file->buffer[0] = 0;   /* an empty translation unit, not garbage */
    file->buf_ptr = file->buffer;
    tccelf_begin_file(s);
    preprocess_start(s, 0);
    tcc_debug_start(s);

    /* tokens can only be created now: tok_alloc needs the unit */
    JS_WhaleTokReset();
    whale_gen_ok = 1;
    whale_gen_pure = 1;         /* pessimistic until the generator says otherwise */
    whale_gen_objs = 0;
    if (emit)
        emit(emit_ctx);

    whale_tok_pos = 0;
    whale_tok_active = 1;

    if (!whale_gen_ok) {
        /* The generator bailed on an opcode it cannot lower, so what it pushed is
           a truncated function. Parsing that would only produce a bogus syntax
           error, so skip the parse and unwind as if the unit had ended. */
        whale_tok_active = 0;
        free_defines(whale_define_start);
        sym_pop(&global_stack, NULL, 0);
        sym_pop(&local_stack, NULL, 0);
        global_label_stack = NULL;
        local_label_stack = NULL;
        preprocess_end(s);
        tccelf_end_file(s);
        tcc_close();
        s->error_set_jmp_enabled = 0;
        return 0;
    }

    if (whale_dbg_env("WHALE_IR_DEBUG")) {
        fprintf(stderr, "[isa] bits=0x%x sse2=%d sse4.2=%d avx2=%d (override with WHALE_ISA)\n",
                s->isa_bits,
                !!(s->isa_bits & WHALE_ISA_SSE2),
                !!(s->isa_bits & WHALE_ISA_SSE42),
                !!(s->isa_bits & WHALE_ISA_AVX2));
        JS_WhaleTokDump();
    }

    tccgen_compile(s);

    whale_tok_active = 0;
    preprocess_end(s);
    free_inline_functions(s);
    /* Unwind this unit the way tcc_compile() does, so the next compile in this
       process starts from an empty symbol/macro stack. */
    free_defines(whale_define_start);
    sym_pop(&global_stack, NULL, 0);
    sym_pop(&local_stack, NULL, 0);
    /* Drop any labels left behind by a parse that bailed out early. label_pop()
       would try to define their symbols through put_extern_sym() against a
       symbol table that is already gone, and the next gen_function() pops
       global_label_stack wholesale - so detach the chains here instead of
       letting the stale entries crash a later compile. They are only freed with
       the state at process exit. */
    global_label_stack = NULL;
    local_label_stack = NULL;
    tccelf_end_file(s);
    tcc_close();

    /* pe_output_file() runs again on every relocate and records the PE entry
       symbol in runtime_main. pe_add_runtime() reads that field to decide
       whether to inject a start symbol, so once it is set a later relocate would
       define `main`/`__runmain` as UNDEF and pe_check_symbols() would fail the
       compile. WhaleJS never runs a program out of this state, so clear it. */
    s->runtime_main = NULL;

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
    static const int optok[] = { '+', '-', '*', '/', TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_EQ, TOK_NE, TOK_LOR, TOK_LAND,
                                 TOK_SHL, TOK_SAR, TOK_SHR,
                                 TOK_ARROW, '.' };

    if (o >= 0 && o < (int)(sizeof(optok) / sizeof(optok[0])))
        JS_WhaleTokPush(optok[o], 0);
}

/* Diagnostics: reconstruct the token stream as text, so it can be compared with
   the C it is supposed to be equivalent to. */
void JS_WhaleTokDump(void)
{
    int kk;
    fprintf(stderr, "[tokcount=%d] vals:", whale_tok_len);
    for (kk = 0; kk < whale_tok_len && kk < 18; kk++)
        fprintf(stderr, " %d", whale_tok_buf[kk]);
    fprintf(stderr, "\n");
    int k;

    for (k = 0; k < whale_tok_len; k++) {
        int t = whale_tok_buf[k];
        fprintf(stderr, "%d:", k);
        /* get_tok_str() is only valid for identifiers/keywords: for a plain
           character token it would index past/to the wrong table entry. */
        if (t == TOK_CINT)
            fprintf(stderr, "INT %d\n", (int)whale_tok_num[k]);
        else if (t == TOK_CDOUBLE)
            fprintf(stderr, "DBL %.17g\n", whale_tok_num[k]);
        else if (t > 0 && t < 256)
            fprintf(stderr, "'%c' (%d)\n", (t >= 32 && t < 127) ? t : '?', t);
        else if (t >= TOK_IDENT) {
            const char *nm = get_tok_str(t, NULL);
            fprintf(stderr, "%s (%d)\n", nm ? nm : "?", t);
        } else
            fprintf(stderr, "T%d\n", t);
    }
}