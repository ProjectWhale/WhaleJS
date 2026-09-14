/*
 * WhaleJS JIT/AOT backend interface (implementation in whale_jit.c).
 *
 * The engine packs everything the code generator needs into JSWhaleFuncInfo, so
 * the generator never touches quickjs internals; the few internal services it
 * does need come through the JS_Whale* wrappers below (implemented by the
 * engine). Keeping the generator in its own translation unit means editing it
 * rebuilds a small file instead of all of quickjs.c.
 */
#ifndef WHALE_JIT_H
#define WHALE_JIT_H

#include "quickjs.h"

/* Observed property-access shapes for the function being compiled, or NULL. Set by
   the engine from the table it filled while the function was still interpreted.
   Opaque here: the record itself stays in quickjs.c, where JSShape is known. This is
   what replaces proving that an object's shape is stable - a call to something
   opaque may or may not change a shape and there is no list of such functions worth
   trusting, but the interpreted calls show what actually happened. */
JS_EXTERN int JS_WhaleProfShapes(const void *prof, unsigned pc,
                                 const void **out, int max);
/* The property atoms of a shape, in layout order, for laying out an unboxed struct. */
JS_EXTERN int JS_WhaleShapeFields(const void *shape, int *atoms, int max);

typedef struct JSWhaleFuncInfo {
    const uint8_t *byte_code;
    const void *prof;   /* observed access shapes, or NULL */
    int byte_code_len;
    int is_normal;      /* func_kind == JS_FUNC_NORMAL */
    int arg_count;
    int var_count;
    int stack_size;
    int var_ref_count;
    int cpool_count;
    const JSValue *cpool;
} JSWhaleFuncInfo;

/* Number of JSValue slots the engine lends to a JIT-compiled function so it has
   somewhere GC can see an object it is holding across a helper call.

   This is not an optimisation, it is what makes objects possible in JIT code at
   all. The generated function is a plain C function: it has no JSStackFrame, GC
   does not scan the C stack, and so a `void *` in a C local is invisible to the
   collector - the object would be freed underneath it. The engine therefore
   reserves this many slots immediately after the JS operand stack, walks them
   as part of the frame, and hands the address to the JIT. The JIT puts object
   number k in slot k.

   Borrowing them costs nothing at run time: the JIT keeps every intermediate
   value in C doubles and never uses the JS operand stack, which is the region
   these slots sit right behind. */
#define WHALE_OBJ_SLOTS 28
/* The slots from WHALE_CALL_BASE up are the scratch a compiled call needs: the
   arguments have to sit in JSValues the collector can see for as long as the
   callee runs, and slots are the only such place the generated code has. Object
   literals and arrays take slots from 0, so they must stay below the base - the
   generator refuses a function that allocates past it. */
#define WHALE_CALL_BASE 20
#define WHALE_CALL_SLOTS (WHALE_OBJ_SLOTS - WHALE_CALL_BASE)
/* object-property inline-cache pairs the generated code can declare */
#define WHALE_FIELD_CACHE 24

/* Unboxed struct slots. A slot whose observed shape set is settled holds its fields
   as plain doubles in the frame instead of a heap object with a shape: one load per
   field, no shape check, no tag branch, no pointer chase. The layout is the UNION of
   the observed shapes, and a field that is not present in every one of them keeps its
   slot plus a presence flag - reserving eight bytes each is cheap next to the access
   it replaces, which is the whole argument for doing this at all.

   Flat arrays rather than a generated struct type: indexing a fixed base by a
   compile-time constant is the same single load, and it keeps the frame declaration
   from having to be built dynamically. */
#define WHALE_STRUCT_SLOTS  4
#define WHALE_STRUCT_FIELDS 16

/* The unset marker for an unboxed field, and the shape of the initialisation.

   A field is two words: value first, tag second (measured - sizeof(JSValue) is 16
   and JS_UNDEFINED is value 0, tag 3). A fresh field must read as undefined and
   must NOT look like a member of the object, and the two states differ only in
   what those words hold:

       never assigned   value = WHALE_STR_UNSET_VALUE, tag = WHALE_STR_UNSET_TAG
       assigned undefined   value = 0, tag = JS_TAG_UNDEFINED
       assigned a number    value = <bits>, tag = JS_TAG_FLOAT64

   The marker is written as a macro rather than a literal at the point of use, so
   the value is stated once and the initialisation loop stays a plain loop. */
#define WHALE_STR_UNSET_TAG   JS_TAG_UNDEFINED
#define WHALE_STR_UNSET_VALUE ((int64_t)JS_TAG_UNDEFINED)
/* property names the generator can reason about (object literals only) */
#define WHALE_ATOM_MAX 32
/* direct calls the generator records, to see which loops contain one */
#define WHALE_CALL_MAX 32

/* Compile one function. Returns 1 and fills entry_out/handle_out on success;
   the caller owns both, and releases the handle with JS_WhaleJitRelease(). */
int  JS_WhaleJitCompileFunction(JSContext *ctx, const JSWhaleFuncInfo *info,
                                void **entry_out, void **handle_out);
void JS_WhaleJitRelease(void *handle);

/* services the engine provides to the generator */
int JS_WhaleVarRefIsSelf(JSContext *ctx, void **var_refs, int idx, JSValue self);
/* Fill `out` with the bytecode of the function literal in cpool[idx], so the
   generator can look inside a callee that was declared in the same body. Returns
   0 when that pool entry is not a function. */
int JS_WhaleCpoolFuncInfo(JSContext *ctx, const JSValue *cpool, int idx,
                          JSWhaleFuncInfo *out);
/* Closure slot read/write as a number. 0 to deopt: no binding, or a value that
   is not a number. This is what lets a function that captures an enclosing
   variable stay in the JIT instead of falling back to the interpreter. */
int JS_WhaleVarRefGetNum(JSContext *ctx, void *var_refs, int idx, double *out);
int JS_WhaleVarRefSetNum(JSContext *ctx, void *var_refs, int idx, double v);
/* Park whatever closure slot idx holds in object slot `slot`, whatever kind of
   value it is. Used when the slot feeds a call: the value is either this
   function (self-recursion) or an enclosing closure's function. */
int JS_WhaleVarRefGetObj(JSContext *ctx, void *var_refs, int idx, int slot);
int JS_WhaleCheckStack(JSContext *ctx);
int JS_WhaleOpSize(int op);   /* bytecode operand size, from the interpreter table */

/* ---- mixed compilation: the single-instruction fallback ----

   An instruction the generator cannot lower is handed to the engine instead of
   throwing the whole function away. The values it works on travel through the
   frame's own operand stack - the memory the interpreter uses - so only the
   values this one instruction touches are converted, not the whole frame.

   Protocol for one instruction: push its n_pop inputs into operand slots
   0..n_pop-1, run it, then read the n_push results back from slots 0..n_push-1.
   Every call may return 0, and 0 always means "assume nothing" - the generator
   then deoptimises the call. Control flow (goto, return, call, throw) is NOT
   covered and must never be handed over: the generated code is structured and
   cannot follow a branch it did not emit itself. */

/* is this opcode one the fallback can run? ask before emitting, so a function
   never compiles only to deopt at run time (which loses the entry for good) */
int JS_WhaleStepCovers(int op);
/* how many values an opcode pops / pushes; -1 when op is not an opcode */
void JS_WhaleOpArity(int op, int *n_pop, int *n_push);
/* put generator-side value i on the operand stack: a number, or a parked object */
int JS_WhaleStepPushNum(JSContext *ctx, int i, double v);
int JS_WhaleStepPushSlot(JSContext *ctx, int i, int slot);
/* run the instruction at bytecode offset pc_off; 0 = engine threw or not covered */
int JS_WhaleStepRun(JSContext *ctx, int pc_off, int n_pop, int n_push);
/* read result i back: whether it is a number, its number, or park it in a slot */
int JS_WhaleStepOutIsNum(JSContext *ctx, int i);
double JS_WhaleStepOutNum(JSContext *ctx, int i);
int JS_WhaleStepOutSlot(JSContext *ctx, int i, int slot);
/* release the first n operand slots - they are only this instruction's stage */
void JS_WhaleStepClear(JSContext *ctx, int n);

/* Object slots: the GC-visible place for the JIT to keep an object across a
   helper call (see WHALE_OBJ_SLOTS above). The engine implements these against
   the calling frame, so the generated code does not have to pass an address. */
void JS_WhaleObjSlot(JSContext *ctx, int k, JSValue v);   /* park object in slot k */
/* Park a null, or the value of an atom, in an object slot. Pushing a non-number used
   to abandon the whole function; these are the two instructions that do it most. */
void JS_WhaleObjSlotNull(JSContext *ctx, int k);
int  JS_WhaleObjSlotAtom(JSContext *ctx, int atom, int k);
/* Apply a real JS binary operator to two parked values (see quickjs.c). Returns 0 on
   exception, in which case the generated code must deopt. */
JS_EXTERN int JS_WhaleBinopVal(JSContext *ctx, int op, int a_slot, int b_slot,
                               int out_slot);
/* Move a callable from the constant pool into an object slot, so the general call
   path can use a callee that inlining could not expand. */
JS_EXTERN int JS_WhaleCpoolSlot(JSContext *ctx, int idx, int k);
JSValue JS_WhaleObjGet(JSContext *ctx, int k);            /* read it back */
/* Park a plain number in slot k, so it can be a call argument (JS_Call takes
   JSValues, and an argument has to stay visible to GC while the callee runs). */
int JS_WhaleObjSlotNum(JSContext *ctx, int slot, double v);
/* Call the value in func_slot with the arguments parked in
   arg_base..arg_base+argc-1. The result is parked in ret_slot, or dropped when
   ret_slot is negative. 0 means deopt (the call threw). Callee and arguments are
   borrowed from their slots, which is what keeps them alive across the call. */
int JS_WhaleCallValue(JSContext *ctx, int func_slot, int argc, int arg_base,
                      int ret_slot);
/* The same call, to this function itself, for arguments the double domain cannot
   carry: the self-recursion path packs its arguments into a double array, so a
   self-call that passes a parked object has to come through here instead. The
   function itself comes from the running frame - see the definition. */
int JS_WhaleCallSelfValue(JSContext *ctx, int argc, int arg_base, int ret_slot);
/* Duplicate slot src into slot dst (moving an already-parked value into the
   contiguous scratch area a call needs). */
int JS_WhaleObjSlotCopy(JSContext *ctx, int src, int dst);
/* Park argument `v` in slot k when it is an object, so a parameter used as the
   receiver of a property access can travel as the JSValue it is. 0 means "not an
   object" and deopts. */
int JS_WhaleArgGetObj(JSContext *ctx, JSValue v, int slot);
/* The pending exception as a value the generated code can return. */
JSValue JS_WhaleExceptionValue(JSContext *ctx);

/* Typed-array support. The generated code has no JSValue domain, so an array
   travels as an object slot number - a compile-time constant - and these do the
   object work. Each returns 0 to mean "deopt", and none may raise. */
int JS_WhaleGetGlobalObj(JSContext *ctx, int atom, int slot);
int JS_WhaleNewU8Array(JSContext *ctx, int ctor_slot, double n, int out_slot);
int JS_WhaleTAU8Get(JSContext *ctx, int slot, double idx, double *out);
int JS_WhaleTAU8Set(JSContext *ctx, int slot, double idx, double v);

/* Buffer base + element count for a Uint8Array in `slot`, so generated code can
   fetch both once and then index directly instead of calling an accessor per
   element. NULL means deopt. */
uint8_t *JS_WhaleTAU8Ptr(JSContext *ctx, int slot, double *len_out);

/* Plain-object fields, for the JIT's object-literal path. `JS_WhaleObjShape`
   gives an opaque key the generated code caches; `JS_WhaleObjFieldPtr` resolves
   a property against it and returns the address of its value, or NULL to deopt
   (wrong shape, missing property, or an accessor). */
int JS_WhaleObjNew(JSContext *ctx, int slot);
void *JS_WhaleObjShape(JSContext *ctx, int slot);
JSValue *JS_WhaleObjFieldPtr(JSContext *ctx, int slot, int atom, void **shape_io);
/* Define a numeric property, for an object literal's initialisers. */
int JS_WhaleObjDefineNum(JSContext *ctx, int slot, int atom, double v);
/* Define a property from a parked value instead of a double. */
JS_EXTERN int JS_WhaleObjDefineValue(JSContext *ctx, int slot, int atom, int val_slot);

/* The cold branch of an inlined property read: sets *deopt and returns 0 when
   the value is neither an int32 nor a float64. */
double JS_WhaleObjValueNum(JSContext *ctx, JSValue *v, int *deopt);
/* Packed-array access: the backing store plus element count (NULL to deopt), a
   fresh empty array, and a numeric element store that appends. */
JSValue *JS_WhaleArrPtr(JSContext *ctx, int slot, double *len_out);
int JS_WhaleArrNew(JSContext *ctx, int slot);
int JS_WhaleArrSetNum(JSContext *ctx, int slot, double idx, double v);
/* Park argument `idx` in object slot `slot` as a packed array; 0 to deopt. Lets
   a function that takes an array stay in the JIT - the generated code assumes
   the parameter is one (a wrong guess only bails) and this checks it. */
int JS_WhaleArrBind(JSContext *ctx, int slot, JSValue *args, int idx);

/* Numeric property access with a shape cache held in the caller: *shape_cache
   and *ptr_cache are two opaque words the generated code declares. Sets *deopt
   and returns 0 when the shape changed under it, the property is missing, or
   the value is neither an int32 nor a float64. */
double JS_WhaleObjFieldNum(JSContext *ctx, int slot, int atom,
                           void **shape_cache, void **ptr_cache, int *deopt);
JS_EXTERN int JS_WhaleObjStoreValue(JSContext *ctx, int slot, int atom, int val_slot,
                                    void **shape_cache, void **ptr_cache);
int JS_WhaleObjStoreNum(JSContext *ctx, int slot, int atom, double v,
                        void **shape_cache, void **ptr_cache);

/* The native form of an object - a second view of the same thing, which travels with
   the object instead of being rebuilt at every boundary. See the definitions in
   quickjs.c for the design, and notes/whale-step-repro.md for what it replaces.

   The ...Slot forms take an object slot, which is how generated code holds an object.
   Field/Put are what the field paths use when a native form is present; they refuse
   (deopt) when it is absent or the shape has moved on, and the caller falls back. */
int JS_WhaleNativeEnsureSlot(JSContext *ctx, int slot, const int *atoms, int n);
double JS_WhaleNativeSlotFieldNum(JSContext *ctx, int slot, int col, int *deopt);
int JS_WhaleNativeSlotPutNum(JSContext *ctx, int slot, int col, int atom, double v);

/* Compile-time only - never called by generated code. Does global `atom` name
   the Uint8Array constructor? Asking while emitting is what keeps each stack
   slot's held type a single compile-time fact. */
int JS_WhaleGlobalIsU8Ctor(JSContext *ctx, int atom);

/* Test hook: force a GC, then report the .length of whatever is in slot k, or -1
   if the slot does not hold an object. It exists to prove the slots survive a
   collection, which is otherwise unobservable from JS. */
int JS_WhaleObjProbe(JSContext *ctx, int k);

/* Park a copy of v in slot 0, collect, then report its .length (-1 if v is not an
   object). Combined into one call so generated code never has to reference
   js_dup, which is static to the engine. */
int JS_WhaleObjProbeFrom(JSContext *ctx, JSValue v);

/* Keyword ids for the token emitters. The generator names a keyword; the
   TinyCC-side file maps it to its own TOK_*. ORDER MUST MATCH kwtok[] in
   src/tcc/whale_tcc_iface.c. */
enum {
    JS_WK_INT = 0, JS_WK_CHAR, JS_WK_VOID, JS_WK_LONG, JS_WK_UNSIGNED, JS_WK_FLOAT,
    JS_WK_DOUBLE, JS_WK_STATIC, JS_WK_EXTERN, JS_WK_TYPEDEF, JS_WK_STRUCT,
    JS_WK_RETURN, JS_WK_IF, JS_WK_ELSE, JS_WK_WHILE, JS_WK_FOR, JS_WK_DO,
    JS_WK_BREAK, JS_WK_CONTINUE, JS_WK_GOTO, JS_WK_SWITCH, JS_WK_CASE,
    JS_WK_DEFAULT, JS_WK_SIZEOF, JS_WK_REGISTER, JS_WK_UNION, JS_WK_ENUM, JS_WK_SHORT,
};

/* operator ids for JS_WhaleTokOp (same idea as the keyword ids) */
enum {
    JS_OP_ADD = 0, JS_OP_SUB, JS_OP_MUL, JS_OP_DIV,
    JS_OP_LT, JS_OP_LE, JS_OP_GT, JS_OP_GE, JS_OP_EQ, JS_OP_NE,
    JS_OP_OR, JS_OP_AND,
    /* These are single tokens in C, not two characters: the parser never sees
       a lexer, so `wt('<'); wt('<')` would arrive as two `<` and fail with
       "')' expected (got \"<\")". */
    JS_OP_SHL, JS_OP_SAR, JS_OP_SHR,
    /* member access, for inlined property reads: -> is one token in C */
    JS_OP_ARROW, JS_OP_DOT,
};

/* provided by src/tcc/whale_tcc_iface.c */
void JS_WhaleTokKw(int w);
void JS_WhaleTokIdent2(const char *name);
void JS_WhaleTokSlot(const char *prefix, int n);
void JS_WhaleTokInt(int v);
void JS_WhaleTokDouble(double v);
void JS_WhaleTokChar(int ch);
void JS_WhaleTokOp(int o);

#endif /* WHALE_JIT_H */
