// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  utility functions used by the runtime system, generated code, and Base library
*/
#include "platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <iostream>
#include <map>
#include <vector>
#include <memory>
#include <string>  
#include <sstream> 
#if defined(_OS_WINDOWS_)
#include <malloc.h>
#else
#include <unistd.h>
#endif
#include <ctype.h>
#include "julia.h"
#include "julia_internal.h"
#include "julia_assert.h"

/*TODO:
1. handle quoted name and unquoted name correctly
2. module qualification
3. isbitstype value as type parameter
*/
template<typename ... Args>
std::string string_format( const std::string& format, Args ... args )
{
    int size_s = std::snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
    //if( size_s <= 0 ){ throw std::runtime_error( "Error during formatting." ); }
    auto size = static_cast<size_t>( size_s );
    auto buf = std::make_unique<char[]>( size );
    std::snprintf( buf.get(), size, format.c_str(), args ... );
    return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
}

extern "C" JL_DLLEXPORT int jl_id_start_char(uint32_t wc) JL_NOTSAFEPOINT;
extern "C" JL_DLLEXPORT int jl_id_char(uint32_t wc) JL_NOTSAFEPOINT;
//int c(char *str) JL_NOTSAFEPOINT;
extern "C" JL_DLLEXPORT void jl_func_sig_to_string(std::stringstream &s, jl_value_t *type) JL_NOTSAFEPOINT;
extern "C" JL_DLLEXPORT void jl_test_print(jl_value_t *type) JL_NOTSAFEPOINT;
//extern "C" JL_DLLEXPORT void name_from_method_instance(jl_value_t *li) JL_NOTSAFEPOINT;

// specTypes => (method signature => string for llvm function)
// C++ is unhappy about this function...
// JL_DLLEXPORT void jl_func_sig_to_string(std::stringstream &s, jl_value_t *type) JL_NOTSAFEPOINT;
//extern "C" const char* name_from_method_instance(jl_method_instance_t *li);
extern "C" JL_DLLEXPORT void jl_datatype_to_string(std::stringstream &out, jl_value_t *v, jl_datatype_t *vt,
                                 struct recur_list *depth) JL_NOTSAFEPOINT;
extern "C" JL_DLLEXPORT int jl_is_identifier(char *str) JL_NOTSAFEPOINT;
extern "C" JL_DLLEXPORT const char* jl_name_from_method_instance(jl_method_instance_t *li) JL_NOTSAFEPOINT;


static int jl_static_is_function_(jl_datatype_t *vt) JL_NOTSAFEPOINT {
    if (!jl_function_type) {  // Make sure there's a Function type defined.
        return 0;
    }
    int _iter_count = 0;  // To prevent infinite loops from corrupt type objects.
    while (vt != jl_any_type) {
        if (vt == NULL) {
            return 0;
        } else if (_iter_count > 10000) {
            // We are very likely stuck in a cyclic datastructure, so we assume this is
            // _not_ a Function.
            return 0;
        } else if (vt == jl_function_type) {
            return 1;
        }
        vt = vt->super;
        _iter_count += 1;
    }
    return 0;
}


struct recur_list {
    struct recur_list *prev;
    jl_tvar_t *v;
};

int is_globfunction(jl_value_t *v, jl_datatype_t *dv, jl_sym_t **globname_out)
{
    jl_sym_t *globname = dv->name->mt != NULL ? dv->name->mt->name : NULL;
    *globname_out = globname;
    int globfunc = 0;
    if (globname && !strchr(jl_symbol_name(globname), '#') &&
        !strchr(jl_symbol_name(globname), '@') && dv->name->module &&
        jl_binding_resolved_p(dv->name->module, globname)) {
        jl_binding_t *b = jl_get_module_binding(dv->name->module, globname);
        // The `||` makes this function work for both function instances and function types.
        jl_value_t* bv = jl_atomic_load_relaxed(&(b->value));
        if (b && bv && (bv == v || jl_typeof(bv) == v)) {
            globfunc = 1;
        }
    }
    return globfunc;
}

extern "C" JL_DLLEXPORT void jl_test_print(jl_value_t *type) JL_NOTSAFEPOINT{
    std::stringstream os;
    jl_func_sig_to_string(os, type);
    //jl_printf(JL_STDOUT,"Test type:");
    //std::string s = "";
    //os >> s;
    jl_printf(JL_STDOUT,os.str().c_str());
    return;
}

// convert operator to Module.:(+), convert non-identifier to Module.var""
extern "C" JL_DLLEXPORT void jl_sym_to_fully_qualified_sym(std::stringstream &out, jl_sym_t* name) JL_NOTSAFEPOINT{
    //size_t n = 0;
    char *sn = jl_symbol_name(name);
    int non_id = 0;
    int op = 0;
    if (jl_is_operator(sn)){
        op = 1;
    }
    else if (!(jl_is_identifier(sn))) {
        non_id = 1;
    }
    if (non_id){
        out << "var\"";
    }
    else if (op){
        out << ":(";
    }
    out << sn;
    if (non_id){
        out << "\"";
    }
    else if (op){
        out << ")";
    }
    return;
}

extern "C" JL_DLLEXPORT void jl_escaped_sym_to_string(std::stringstream &out, jl_sym_t* name) JL_NOTSAFEPOINT{
    //size_t n = 0;
    char *sn = jl_symbol_name(name);
    int hidden = 0;
    if (!(jl_is_identifier(sn) || jl_is_operator(sn))) {
        hidden = 1;
    }
    if (hidden){
        out << "var\"";
    }
    out << sn;
    if (hidden){
        out << "\"";
    }
    return;
}
/* 
    Convert a module to a fully qualified string. For example, Compiler is convert to Core.Compiler
*/
extern "C" JL_DLLEXPORT void jl_module_to_string(std::stringstream &s, jl_module_t* m) JL_NOTSAFEPOINT{
    int n = 0;
    jl_module_t* start = m;
    // We will omit Main module if the symbol is not defined in Main
    // So if the first module we encounter is Main, it means the symbol is defined in Main.
    if (start == jl_main_module){
        s << "Main";
        return;
    }
    while (start != NULL){
        if (start == jl_main_module){
            break;
        }
        if (start==start->parent){
            n += 1;
            break;
        }
        n += 1;
        start = start->parent;
    };
    jl_module_t** mods = (jl_module_t**)alloca(sizeof(jl_module_t*)*n);
    mods[0] = m;
    for (int i = 1;i < n;i++){
        mods[i] = mods[i-1]->parent;
    }
    for (int i = n-1;i > 0;i--){
        s << jl_symbol_name(mods[i]->name);
        s << '.';
    }
    s << jl_symbol_name(m->name);
    return;
}
/*
// Given an identifier with name `name` which is defined in `mod`, return a qualified name
// need to handle special symbol in variable name
void jl_identifier_to_string(std::stringstream &s,jl_module_t* mod, jl_sym_t *name, bool needcolon, bool needSym) JL_NOTSAFEPOINT{
    int n = 0;
    jl_module_t* start = m;
    // Case 1: We will omit Main module if the symbol is not defined in Main
    // So if the first module we encounter is Main, it means the symbol is defined in Main.
    if (start == jl_main_module){
        s << "Main.";
        s << jl_symbol_name(m->name);
        return;
    }
    // Case 2: Some builtin-in types defined in Core is widely used and exported everywhere
    // so we safely abbrev the mod, otherwise, a fully qualified name is required
    if (!(mod == jl_core_module && jl_module_exports_p(jl_core_module, sym))){
        while (start != NULL){
            // Main is not a direct parent, ignore Main module
            if (start==jl_main_module){
                break;
            } 
        if (start==jl_core_module){
            n += 1;
            break;
        }
        n += 1;
        start = start->parent;
        };
        jl_module_t** mods = (jl_module_t**)alloca(sizeof(jl_module_t*)*n);
        mods[0] = m;
        for (int i = 1;i < n;i++){
            mods[i] = mods[i-1]->parent;
        }
        for (int i = n-1;i > 0;i--){
            s << jl_symbol_name(mods[i]->name);
            s << '.';
        }
    }
    jl_unqualified_identifier_to_string(s,name,needcolon,needSym);
    return;
}

void jl_unqualified_identifier_to_string(std::stringstream &s, jl_sym_t *name,bool needcolon, bool needSym) JL_NOTSAFEPOINT{
    assert(!(needcolon&&needSym));
    if (needcolon){
        s << ":(";
        s << jl_symbol_name(name);
        s << ')';
        return;
    }
    if (needSym){
        s << "Symbol(\""
        s << jl_symbol_name(name);
        s << ')';
        return;
    }
    s << jl_symbol_name(name)
    return;
}
*/
// entry point for function signature display
extern "C" JL_DLLEXPORT void jl_func_sig_to_string(std::stringstream &s, jl_value_t *type) JL_NOTSAFEPOINT{
    //size_t n = 0;
    size_t i = 0;
    // normally this would be a function type (typeof(...))
    jl_value_t *ftype = (jl_value_t*)jl_first_argument_datatype(type);
    if (ftype==NULL){
        jl_error("This first type of this function sig is not a function!");
        jl_(ftype);
    };
    //jl_unionall_t *tvars = (jl_unionall_t*)type;
    int nvars = jl_subtype_env_size(type);
    struct recur_list *typevarlist = NULL;
    struct recur_list *depth = NULL;
    // contains type variable
    if (nvars > 0) {
        typevarlist = (struct recur_list*)alloca(sizeof(struct recur_list) * nvars);
        for (int i = 0; i < nvars; i++) {
            typevarlist[i].prev = i == 0 ? NULL : &typevarlist[i - 1];
            typevarlist[i].v = ((jl_unionall_t*)type)->var;
            type = ((jl_unionall_t*)type)->body;
        }
        depth = &(typevarlist[nvars - 1]);
    };
    // For toplevel code? 
    if (!jl_is_datatype(ftype)) {
        jl_error("Shouldn't be here");
        return;
    };
    // Type contains no type variables, or it's fully-unapplied type.
    // should we distinguish between type constructor and normal function?
    // Type constructor is represented by Type{...}, and function is represented by typeof(...)
    // TODO: omit useless Core.XXX
    if (jl_nparams(ftype) == 0 || ftype == ((jl_datatype_t*)ftype)->name->wrapper) {
        jl_module_to_string(s,((jl_datatype_t*)ftype)->name->module);
        s << '.';
        jl_sym_t *globname;
        int globfunc = is_globfunction(ftype, (jl_datatype_t*)ftype, &globname);
        jl_sym_t *sym = globfunc ? globname : ((jl_datatype_t*)ftype)->name->name;
        char *sn = jl_symbol_name(sym);
        size_t quote = 0;
        if (globfunc && !jl_id_start_char(u8_nextchar(sn, &i))){
            s << ":(";
            quote = 1;
        }
        jl_escaped_sym_to_string(s,sym);
        if (quote) {
            s << ')';
        }
    }
    else {
        // we display type constructor as (::Type{...})(...)
        // this is because we may have siguature like this:
        // (::Type{T})(x::Tuple) where {T<:Tuple} = convert(T, x)  # still use `convert` for tuples
        // for type constructor
        // TODO: maybe we can abbrev the Type{...} for some constructor
        s << "(::";
        //jl_module_t* m = ((jl_datatype_t*)ftype)->name->module;
        //jl_module_to_string(s,m);
        //s << '.';
        //if (jl_is_type_type(ftype)){
        //    jl_value_t* param = jl_tparam0(ftype);
        //    jl_datatype_to_string(s, param, (jl_datatype_t*)jl_typeof(param),depth);
        //}
        //else{
        //    jl_error("Shouldn't be here!");
        jl_datatype_to_string(s, ftype, (jl_datatype_t*)jl_typeof(ftype),depth);
        //}
        s << ')';
    }
    size_t tl = jl_nparams(type);
    s << '(';

    // display remaining argument type
    for (i = 1; i < tl; i++) {
        jl_value_t *tp = jl_tparam(type, i);
        // TODO: should we use a f(::X,::X2) or just f(X1,X2)?
        // s << "::";
        if (i != tl - 1) {
            jl_datatype_to_string(s, tp, (jl_datatype_t*)jl_typeof(tp), depth);
            s << ", ";
        }
        else {
            // last parameter needs special treatment
            // Vararg is represented differently in earlier Julia version
            // So type of Vararg{Int,N} where N is not UnionAll 
            // It's TypeofVararg!
            if (jl_vararg_kind(tp) == JL_VARARG_UNBOUND) {
                // unbounded means N is undetermined
                //tp = jl_unwrap_vararg(tp);
                //if (jl_is_unionall(tp)){
                //    jl_printf(JL_STDOUT,"Vararg shouldn't be a unionall");
                //    s << '(';
                //}
                jl_datatype_to_string(s, tp, jl_vararg_type, depth);
                //if (jl_is_unionall(tp)){
                //    s << ')';
                //}
                //n += jl_printf(s, "...");
            }
            else {
                jl_datatype_to_string(s, tp, (jl_datatype_t*)jl_typeof(tp), depth);
            }
        }
    }
    s << ')';
    if (nvars > 0) {
        //depth -= nvars - 1;
        //int first = 1;
        s << " where {";
        for (int i = 0; i < nvars; i++){
            jl_datatype_to_string(s,(jl_value_t*)typevarlist[i].v,jl_tvar_type,typevarlist[i].prev);
            if (i != (nvars-1)){
               s << ", ";
           }
       }
        s << "}";
    }
    return ;
}
extern "C" JL_DLLEXPORT void jl_value_to_string(std::stringstream &out, jl_value_t *v){
    jl_value_t* vt = jl_typeof(v);
    jl_datatype_to_string(out, v,(jl_datatype_t*)vt , NULL);
}
// since a datatype can contains many different values (for example, Val{T}), we have to deal with these cases
// depth is used to track type variables, since we need to print types inside out!
// every time we encounter a unionall, we push the type variable to the depth stack
// every time we encount a typevar, we check whether this is a definition of typevar or a use of a typevar.
// If it's a definition, we need to print bounds of typevar.
extern "C" JL_DLLEXPORT void jl_datatype_to_string(std::stringstream &out, jl_value_t *v, jl_datatype_t *vt, struct recur_list *depth) JL_NOTSAFEPOINT{
    //size_t n = 0;
    //size_t n = 0;
    if ((uintptr_t)vt < 4096U) {
        jl_error("Invalid pointer");
    }
    // These need to be special cased because they
    // exist only by pointer identity in early startup
    // we need to use `Core.XXX` for SimpleVector and other unexported name
    else if (v == (jl_value_t*)jl_simplevector_type) {
        out << "Core.SimpleVector";
    }
    else if (v == (jl_value_t*)jl_typename_type) {
        out << "Core.TypeName";
    }
    else if (v == (jl_value_t*)jl_symbol_type) {
        out << "Symbol";
    }
    else if (v == (jl_value_t*)jl_methtable_type) {
        out << "Core.MethodTable";
    }
    else if (v == (jl_value_t*)jl_any_type) {
        out << "Any";
    }
    else if (v == (jl_value_t*)jl_type_type) {
        out << "Type";
    }
    /*
    SimpleVector is not allowed in types.
    else if (vt == jl_simplevector_type) {
        n += jl_show_svec(out, (jl_svec_t*)v, "svec", "(", ")");
    }
    */
    else if (v == (jl_value_t*)jl_unionall_type) {
        // avoid printing `typeof(Type)` for `UnionAll`.
        out << "UnionAll";
    }
    else if (vt == jl_vararg_type) {
        jl_vararg_t *vm = (jl_vararg_t*)v;
        out << "Vararg";
        if (vm->T) {
            out << '{';
            jl_datatype_to_string(out, vm->T, (jl_datatype_t*)jl_typeof(vm->T), depth);
            // In newer version of Julia, If N is not set, then it means N is undetermined
            if (vm->N) {
                out << ", ";
                jl_datatype_to_string(out, vm->N, (jl_datatype_t*)jl_typeof(vm->N), depth);
            }
            out << '}';
        }
    }
    else if (vt == jl_datatype_type) {
        // typeof(v) == DataType, so v is a Type object.
        // Types are printed as a fully qualified name, with parameters, e.g.
        // `Base.Set{Int}`, and function types are printed as e.g. `typeof(Main.f)`
        // TODO: handle function correctly here!
        jl_datatype_t *dv = (jl_datatype_t*)v;
        jl_sym_t *globname;
        int globfunc = is_globfunction(v, dv, &globname);
        jl_sym_t *sym = globfunc ? globname : dv->name->name;
        char *sn = jl_symbol_name(sym);
        size_t quote = 0;
        if (globfunc) {
            out << "typeof(";
        }
        //TODO: handle local definition and quote correctly
        if (jl_core_module && (dv->name->module != jl_core_module || !jl_module_exports_p(jl_core_module, sym))) {
            // we need to use fully qualified name for modules other than core, or symbols unexported from Core
            jl_module_to_string(out,dv->name->module);
            out << '.';
        }
        size_t i = 0;
        if (globfunc && !jl_id_start_char(u8_nextchar(sn, &i))){
            out << ":(";
            quote = 1;
        }
        jl_escaped_sym_to_string(out,sym);
        if (globfunc) {
            out << ')';
            if (quote) {
                out << ')';
            }
        }
        // now we process type parameters
        if (dv->parameters && (jl_value_t*)dv != dv->name->wrapper &&
            (jl_has_free_typevars(v) ||
             (jl_value_t*)dv != (jl_value_t*)jl_tuple_type)) {
            size_t j, tlen = jl_nparams(dv);
            if (tlen > 0) {
                out << '{';
                for (j = 0; j < tlen; j++) {
                    jl_value_t *p = jl_tparam(dv,j);
                    jl_datatype_to_string(out, p, (jl_datatype_t*)jl_typeof(p),depth);
                    if (j != tlen-1)
                        out << ", ";
                }
                out << '}';
            }
            else if (dv->name == jl_tuple_typename) {
                out << "{}";
            }
        }
    }
    /*
    Can intrinsic type appear in type position?
    else if (vt == jl_intrinsic_type) {
        int f = *(uint32_t*)jl_data_ptr(v);
        n += jl_printf(out, "Core.Intri", f, jl_intrinsic_name(f));
    }
    */
   // special case for some simple plain bit type
    else if (vt == jl_int64_type) {
        out << string_format("%" PRId64,*(int64_t*)v);
    }
    else if (vt == jl_int32_type) {
        out << string_format("%" PRId32, *(int32_t*)v);
    }
    else if (vt == jl_int16_type) {
        out << string_format("%" PRId16, *(int16_t*)v);
    }
    else if (vt == jl_int8_type) {
        out << string_format("%" PRId8, *(int8_t*)v);
    }
    else if (vt == jl_uint64_type) {
        out << string_format("0x%016" PRIx64, *(uint64_t*)v);
    }
    else if (vt == jl_uint32_type) {
        out << string_format("0x%08" PRIx32, *(uint32_t*)v);
    }
    else if (vt == jl_uint16_type) {
        out << string_format("0x%04" PRIx16, *(uint16_t*)v);
    }
    else if (vt == jl_uint8_type) {
        out << string_format("0x%02" PRIx8, *(uint8_t*)v);
    }
    else if (jl_pointer_type && jl_is_cpointer_type((jl_value_t*)vt)) {
        out << "Ptr{";
        //jl_static_show_to_string_x_(out,jl_tparam0((jl_datatype_t*)vt);
        out <<"}(";
#ifdef _P64
        out << string_format("0x%016" PRIx64, *(uint64_t*)v);
#else
        out << string_format("0x%08" PRIx32, *(uint32_t*)v);
#endif
        out << ')';
    }
    else if (vt == jl_float32_type) {
        out << string_format("%gf", *(float*)v);
    }
    else if (vt == jl_float64_type) {
        out << string_format("%g", *(double*)v);
    }
    else if (vt == jl_bool_type) {
        out << (*(uint8_t*)v ? "true" : "false");
    }
    else if (v == jl_nothing || (jl_nothing && (jl_value_t*)vt == jl_typeof(jl_nothing))) {
        out << "nothing";
    }
    /*
    String is not allowed in type
    else if (vt == jl_string_type) {
        out << "\"";
        jl_uv_puts(out, jl_string_data(v), jl_string_len(v)); n += jl_string_len(v);
        out << "\"";
    }
    */
    else if (v == jl_bottom_type) {
        out << "Union{}";
    }
    else if (vt == jl_uniontype_type) {
        out << "Union{";
        while (jl_is_uniontype(v)) {
            // tail-recurse on b to flatten the printing of the Union structure in the common case
            jl_datatype_to_string(out, ((jl_uniontype_t*)v)->a, (jl_datatype_t*)jl_typeof(((jl_uniontype_t*)v)->a), depth);
            out << ", ";
            v = ((jl_uniontype_t*)v)->b;
        }
        jl_datatype_to_string(out, v, (jl_datatype_t*)jl_typeof(v), depth);
        out << '}';
    }
    else if (vt == jl_unionall_type) {
        // when we encounter a unionall type, we add the type var to the depth stack
        // TODO: print all the type variables in a bracket where {...}
        //jl_unionall_t *ua = (jl_unionall_t*)v;
        //struct recur_list * newdepth = {depth, (jl_value_t*)ua->var};
        //jl_datatype_to_string(out, (jl_value_t*)ua->var, jl_tvar_type, depth);
        int typevarnum = 0;
        jl_value_t* start = v;
        while (jl_typeof(start) == (jl_value_t*)jl_unionall_type){
            typevarnum += 1;
            start = ((jl_unionall_t*)start)->body;
        };
        // now start point to a non-unionall type
        struct recur_list* new_unionall_depth = (struct recur_list*)alloca(sizeof(struct recur_list) * typevarnum);
        jl_value_t* typevar_start = v;
        new_unionall_depth[0].prev = depth;
        new_unionall_depth[0].v = ((jl_unionall_t*)typevar_start)->var;
        for (int i=1;i < typevarnum;i++){
            typevar_start = ((jl_unionall_t*)typevar_start)->body;
            new_unionall_depth[i].prev = &(new_unionall_depth[i-1]);
            new_unionall_depth[i].v = ((jl_unionall_t*)typevar_start)->var;
        }
        jl_datatype_to_string(out, start, (jl_datatype_t*)jl_typeof(start), &(new_unionall_depth[typevarnum-1]));
        out << " where {";
        for (int i = 0;i < typevarnum; i++){
            jl_datatype_to_string(out, (jl_value_t*)(new_unionall_depth[i].v), jl_tvar_type, new_unionall_depth[i].prev);
            if (i != (typevarnum-1)){
                out << ", ";
            }
        }
        out << '}';
    }
    /*
    else if (vt == jl_typename_type) {
        n += jl_printf(out, "typename(");
        n += jl_static_show_x(out, jl_unwrap_unionall(((jl_typename_t*)v)->wrapper), depth);
        n += jl_printf(out, ")");
    }
    */
    else if (vt == jl_tvar_type) {
        // show type-var bounds only if they aren't going to be printed by UnionAll later
        jl_tvar_t *var = (jl_tvar_t*)v;
        struct recur_list *p;
        int showbounds = 1;
        // check whether this is a definition or a use
        for (p = depth; p != NULL; p = p->prev) {
            if ((jl_tvar_t*)p->v == var) {
                showbounds = 0;
                break;
            }
        }
        jl_value_t *lb = var->lb, *ub = var->ub;
        if (showbounds && lb != jl_bottom_type) {
            // show type-var lower bound if it is defined
            int ua = jl_is_unionall(lb);
            if (ua)
                out << '(';
            jl_datatype_to_string(out, lb, (jl_datatype_t*)jl_typeof(lb),depth);
            if (ua)
                out << ')';
            out << "<:";
        }
        jl_escaped_sym_to_string(out, var->name);
        if (showbounds && (ub != (jl_value_t*)jl_any_type || lb != jl_bottom_type)) {
            // show type-var upper bound if it is defined, or if we showed the lower bound
            int ua = jl_is_unionall(ub);
            out << "<:";
            if (ua)
                out << '(';
            jl_datatype_to_string(out, ub, (jl_datatype_t*)jl_typeof(ub), depth);
            if (ua)
                out << ')';
        }
    }
    /*
    else if (vt == jl_module_type) {
        jl_module_t *m = (jl_module_t*)v;
        if (m->parent != m && m->parent != jl_main_module) {
            n += jl_static_show_x(out, (jl_value_t*)m->parent, depth);
            n += jl_printf(out, ".");
        }
        n += jl_printf(out, "%s", jl_symbol_name(m->name));
    }
    */
    else if (vt == jl_symbol_type) {
        // a symbol is not a identifier, when we display a value of Symbol
        // we display it as :... or Symbol("...")
        char *sn = jl_symbol_name((jl_sym_t*)v);
        int quoted = !jl_is_identifier(sn) && jl_operator_precedence(sn) == 0;
        if (quoted)
            out << "Symbol(\"";
        else
            out << ":";
        out << sn;
        if (quoted)
            out << "\")";
    }
    /*
    else if (vt == jl_ssavalue_type) {
        n += jl_printf(out, "SSAValue(%" PRIuPTR ")",
                       (uintptr_t)((jl_ssavalue_t*)v)->id);
    }
    else if (vt == jl_globalref_type) {
        n += jl_static_show_x(out, (jl_value_t*)jl_globalref_mod(v), depth);
        char *name = jl_symbol_name(jl_globalref_name(v));
        n += jl_printf(out, jl_is_identifier(name) ? ".%s" : ".:(%s)", name);
    }
    else if (vt == jl_gotonode_type) {
        n += jl_printf(out, "goto %" PRIuPTR, jl_gotonode_label(v));
    }
    else if (vt == jl_quotenode_type) {
        jl_value_t *qv = *(jl_value_t**)v;
        if (!jl_is_symbol(qv)) {
            n += jl_printf(out, "quote ");
        }
        else {
            n += jl_printf(out, ":(");
        }
        n += jl_static_show_x(out, qv, depth);
        if (!jl_is_symbol(qv)) {
            n += jl_printf(out, " end");
        }
        else {
            n += jl_printf(out, ")");
        }
    }
    
    else if (vt == jl_newvarnode_type) {
        out << "Core.NewvarNode";
    }
    else if (vt == jl_linenumbernode_type) {
        out << "Core.LineNumberNode";
    }
    else if (vt == jl_expr_type) {
        jl_expr_t *e = (jl_expr_t*)v;
        if (e->head == jl_assign_sym && jl_array_len(e->args) == 2) {
            n += jl_static_show_x(out, jl_exprarg(e,0), depth);
            n += jl_printf(out, " = ");
            n += jl_static_show_x(out, jl_exprarg(e,1), depth);
        }
        else {
            char sep = ' ';
            n += jl_printf(out, "Expr(:%s", jl_symbol_name(e->head));
            size_t i, len = jl_array_len(e->args);
            for (i = 0; i < len; i++) {
                n += jl_printf(out, ",%c", sep);
                n += jl_static_show_x(out, jl_exprarg(e,i), depth);
            }
            n += jl_printf(out, ")");
        }
    }
    
    else if (jl_array_type && jl_is_array_type(vt)) {
        n += jl_printf(out, "Array{");
        n += jl_static_show_x(out, (jl_value_t*)jl_tparam0(vt), depth);
        n += jl_printf(out, ", (");
        size_t i, ndims = jl_array_ndims(v);
        if (ndims == 1)
            n += jl_printf(out, "%" PRIdPTR ",", jl_array_dim0(v));
        else
            for (i = 0; i < ndims; i++)
                n += jl_printf(out, (i > 0 ? ", %" PRIdPTR : "%" PRIdPTR), jl_array_dim(v, i));
        n += jl_printf(out, ")}[");
        size_t j, tlen = jl_array_len(v);
        jl_array_t *av = (jl_array_t*)v;
        jl_datatype_t *el_type = (jl_datatype_t*)jl_tparam0(vt);
        int nlsep = 0;
        if (av->flags.ptrarray) {
            // print arrays with newlines, unless the elements are probably small
            for (j = 0; j < tlen; j++) {
                jl_value_t *p = jl_array_ptr_ref(av, j);
                if (p != NULL && (uintptr_t)p >= 4096U) {
                    jl_value_t *p_ty = jl_typeof(p);
                    if ((uintptr_t)p_ty >= 4096U) {
                        if (!jl_isbits(p_ty)) {
                            nlsep = 1;
                            break;
                        }
                    }
                }
            }
        }
        if (nlsep && tlen > 1)
            n += jl_printf(out, "\n  ");
        for (j = 0; j < tlen; j++) {
            if (av->flags.ptrarray) {
                n += jl_static_show_x(out, jl_array_ptr_ref(v, j), depth);
            }
            else {
                char *ptr = ((char*)av->data) + j * av->elsize;
                n += jl_static_show_x_(out, (jl_value_t*)ptr, el_type, depth);
            }
            if (j != tlen - 1)
                n += jl_printf(out, nlsep ? ",\n  " : ", ");
        }
        n += jl_printf(out, "]");
    }
    
    else if (vt == jl_loaderror_type) {
        n += jl_printf(out, "LoadError(at ");
        n += jl_static_show_x(out, *(jl_value_t**)v, depth);
        // Access the field directly to avoid allocation
        n += jl_printf(out, " line %" PRIdPTR, ((intptr_t*)v)[1]);
        n += jl_printf(out, ": ");
        n += jl_static_show_x(out, ((jl_value_t**)v)[2], depth);
        n += jl_printf(out, ")");
    }
    else if (vt == jl_errorexception_type) {
        n += jl_printf(out, "ErrorException(");
        n += jl_static_show_x(out, *(jl_value_t**)v, depth);
        n += jl_printf(out, ")");
    }
    */
   
    else if (jl_static_is_function_(vt)) {
        // v is function instance (an instance of a Function type).
        jl_datatype_t *dv = (jl_datatype_t*)vt;
        jl_sym_t *sym = dv->name->mt->name;
        char *sn = jl_symbol_name(sym);

        jl_sym_t *globname;
        int globfunc = is_globfunction(v, dv, &globname);
        int quote = 0;
        if (jl_core_module && (dv->name->module != jl_core_module || !jl_module_exports_p(jl_core_module, sym))) {
            jl_module_to_string(out, dv->name->module);
            out << ".";
            size_t i = 0;
            if (globfunc && !jl_id_start_char(u8_nextchar(sn, &i))) {
                out << ":(";
                quote = 1;
            }
        }
        jl_escaped_sym_to_string(out, sym);
        if (globfunc) {
            if (quote) {
                out << ")";
            }
        }
    }

    else if (jl_datatype_type && jl_is_datatype(vt)) {
        // typeof(v) isa DataType, so v is an *instance of* a type that is a Datatype,
        // meaning v is e.g. an instance of a struct. These are printed as a call to a
        // type constructor, such as e.g. `Base.UnitRange{Int64}(start=1, stop=2)`
        // It can only be concrete immutable!
        if (!(jl_is_immutable(vt)&&jl_is_concrete_type((jl_value_t*)vt))){
            jl_error("A bad value in type!");
            jl_(vt);
        }
        int istuple = jl_is_tuple_type(vt);
        int isnamedtuple = jl_is_namedtuple_type(vt);
        size_t tlen = jl_datatype_nfields(vt);
        if (isnamedtuple) {
            if (tlen == 0){
                out << "NamedTuple";
            }
        }
        else if (!istuple) {
            // print the typename
            jl_datatype_to_string(out, (jl_value_t*)vt, jl_datatype_type, depth);
        }
        out << '(';
        size_t nb = jl_datatype_size(vt);
        // a primitive type with no field
        if (nb > 0 && tlen == 0) {
            uint8_t *data = (uint8_t*)v;
            out << "0x";
            for(int i = nb - 1; i >= 0; --i){
                out << string_format("%02" PRIx8, data[i]);
            }
        }
        else {
            //if (vt == jl_typemap_entry_type)
            //    i = 1;
            jl_value_t *names = isnamedtuple ? jl_tparam0(vt) : (jl_value_t*)jl_field_names(vt);
            for (size_t i = 0; i < tlen; i++) {
                if (!istuple) {
                    jl_value_t *fname = isnamedtuple ? jl_fieldref_noalloc(names, i) : jl_svecref(names, i);
                    // TODO: should we use escaped symbol here?
                    out << string_format("%s=", jl_symbol_name((jl_sym_t*)fname));
                }
                size_t offs = jl_field_offset(vt, i);
                char *fld_ptr = (char*)v + offs;
                if (jl_field_isptr(vt, i)) {
                    // the field is a pointer, so we can safely take its pointer
                    jl_value_t* ptrv = *(jl_value_t**)fld_ptr;
                    jl_datatype_to_string(out,ptrv,(jl_datatype_t*)jl_typeof(ptrv), depth);
                }
                else {
                    jl_datatype_t *ft = (jl_datatype_t*)jl_field_type_concrete(vt, i);
                    if (jl_is_uniontype(ft)) {
                        uint8_t sel = ((uint8_t*)fld_ptr)[jl_field_size(vt, i) - 1];
                        ft = (jl_datatype_t*)jl_nth_union_component((jl_value_t*)ft, sel);
                    }
                    jl_datatype_to_string(out, (jl_value_t*)fld_ptr, ft, depth);
                }
                if ((istuple || isnamedtuple) && tlen == 1)
                    out << ",";
                else if (i != tlen - 1)
                    out << ", ";
            }
        }
        out << ")";
    }
    else {
        jl_error("Bad pointer here!");
        /*
        n += jl_printf(out, "<?#%p::", (void*)v);
        n += jl_static_show_x(out, (jl_value_t*)vt, depth);
        n += jl_printf(out, ">");
        */
    }
    return;
}

// maybe we should just pass a stringbuffer in...
static std::map<jl_value_t*, std::map<jl_value_t*,std::string>> namedpool;
static std::vector<std::string> toplevelpool;
static int toplevel_id = 0;
extern "C" JL_DLLEXPORT const char* jl_name_from_method_instance(jl_method_instance_t *li) {   
    // we need to cache the string in a pool to manage memory.
    std::stringstream stm;
    if (jl_is_method(li->def.method)){
        jl_value_t* fspec = li->specTypes;
        jl_value_t* fsig = li->def.method->sig;
        // we use a pair
        jl_func_sig_to_string(stm,fspec);
        stm << " from ";
        jl_func_sig_to_string(stm,fsig);
        if (namedpool.find(fspec) == namedpool.end()){
            std::map<jl_value_t*,std::string> m;
            m[fsig] = std::move(stm.str()); 
            // TODO: we can actually get the lvalue of namedpool[fspec]
            namedpool[fspec] = std::move(m);
            return namedpool[fspec][fsig].c_str();
        }
        else{
            // the return is a reference, so we won't copy the value here...
            std::map<jl_value_t*,std::string> &defs = namedpool[fspec];
            if (defs.find(fsig) == defs.end()){
                defs[fsig] = std::move(stm.str());
                return defs[fsig].c_str();
            }
            else{
                /*
                jl_safe_printf("Regenerating method instance for:");
                jl_static_show(JL_STDOUT,fspec);
                jl_safe_printf("\n");
                jl_static_show(JL_STDOUT,fsig);
                jl_safe_printf("\n");
                */
                return defs[fsig].c_str();
            }
        }
    }
    else{
        stm << "toplevel";
        stm << toplevel_id;
        toplevel_id += 1;
        toplevelpool.push_back(stm.str());
        return toplevelpool.back().c_str();
    }
}