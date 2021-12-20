# This file is a helper to compile and invoke compiled code from staticjit.cpp
# I decide not to modify Julia's current codegen pipeline
# So we manually convert every function call to a ccall with the compiled function pointer (by function replace_call)
# This is fine since currently we only use this compiler to test object codegen. 
# To test the compiler, I simply reuse the test file from the package and convert the `@test` to compile function

# Global constant pool, refered in staticjit.cpp
# used to root constant value
const StringPool = Any[];
const SymbolPool = Any[];
const BitValuePool = Any[];
const SharedString = "shared!";

#
# Helper to get method instance for a function call
# 
import Core.Compiler: SimpleVector,normalize_typevars,get_compileable_sig,svec
import Base.getindex
function get_method_instances(@nospecialize(f), @nospecialize(t), world::UInt = Core.Compiler.typemax(UInt))
    if isa(f,Function)
        tt = Tuple{Core.typeof(f),t...}
    else
        tt = Tuple{Type{f},t...}
    end
    #results = Core.MethodInstance[]
    # _methods_by_ftype(fsign,method_num_limit,world)
    # return a vector of function that matches the function call
    # _methods_by_ftype also `jl_matching_methods` defined in `gf.c` 
    mi_matches = Core.Compiler._methods_by_ftype(tt, -1, world)
    if Core.Compiler.:(>=)(Core.Compiler.length(mi_matches),1)
        instance = Core.Compiler.specialize_method(Core.Compiler.getindex(mi_matches,1)#=MethodMatch=#)
        return instance
    else
        error("Not a unique match for $f with input type $t")
    end
end

function get_all_method_instances(@nospecialize(f), @nospecialize(t), world::UInt = Core.Compiler.typemax(UInt))
    if isa(f,Function)
        tt = Tuple{Core.typeof(f),t...}
    else
        tt = Tuple{Type{f},t...}
    end
    mi_matches = Core.Compiler._methods_by_ftype(tt, -1, world)
    if Core.Compiler.:(>=)(Core.Compiler.length(mi_matches),1)
        return [Core.Compiler.specialize_method(mi_matches[i]) for i in 1:length(mi_matches)]
    else
        error("Not a unique match for $f with input type $t")
    end
end

#
#   A simple code lower procedure to lower function call to sinvoke
#
struct Failed end
# blacklist to filter out some functions we can't handle now
blacklist = [rand,isa,(===),typeof]

mutable struct LowerCtx
    id::Int
    result::Vector{Any}
    status::Bool
end
LowerCtx() = LowerCtx(0,[],true)
function simple_lower(expr)
    ctx = LowerCtx()
    simple_lower!(ctx,  expr)
    if ctx.status
        Expr(:block,ctx.result...)
    else
        expr
    end
end
function simple_lower!(ctx::LowerCtx, expr)
    if !ctx.status
        return :(nothing),:(nothing)
    end
    newname = gensym(string("v",ctx.id))
    newtname = gensym(string("tv",ctx.id))
    ctx.id += 1
    local newexpr::Any
    if isa(expr, Expr)
        if expr.head == :call
            varnames = similar(expr.args,Symbol)
            typnames = similar(expr.args,Symbol)
            for i in 1:length(expr.args)
                subexpr = expr.args[i]
                if isa(subexpr,Expr) && subexpr.head == :kw
                    ctx.status = false
                    return :(nothing),:(nothing)
                else
                    varnames[i],typnames[i] = simple_lower!(ctx, subexpr)
                end
            end
            for i in eachindex(varnames)
                v = varnames[i]
                tv = typnames[i]
                push!(ctx.result,:($tv = ($v isa DataType ? Type{$v} : typeof($v))))
            end
            body = quote 
                let
                    if $(varnames[1]) in Main.blacklist
                        $(Expr(:(=), newname, Expr(:call, varnames...)))
                    else
                        mi = get_method_instances($(varnames[1]),tuple($(typnames[2:end]...))) 
                        world = Base.get_world_counter()
                        fptr = ccall(:jl_compile_one_methodinst,Ptr{Nothing},(Any, UInt64), mi, world)
                        #sinvoke(ptr,$(map(x->x.args[2],vs)...))
                        arr = Any[$(varnames[2:end]...)]
                        ptr = Base.unsafe_convert(Ptr{UInt8},arr)
                        ccall(fptr,Any,(Ptr{Cvoid},Ptr{Cvoid},Int32),C_NULL,ptr,length(arr))
                    end
                end
            end
            callexpr = Expr(:(=), newname, body)
            push!(ctx.result, callexpr)
        elseif expr.head == :(=)
            varnames = similar(expr.args,Symbol)
            for i in 2:length(expr.args)
                varnames[i] = simple_lower!(ctx, expr.args[i])[1]
            end
            assignexpr = Expr(:(=), expr.args[1], varnames...)
            push!(ctx.result, assignexpr)
            # assign shouldn't have value...
        else
            newexpr = Expr(:(=), newname, expr)
            push!(ctx.result, newexpr)
        end
    else
        newexpr = Expr(:(=), newname, expr)
        push!(ctx.result, newexpr)
    end
    return newname,newtname
end

macro test(expr)
    return :(if !($(esc(simple_lower(expr))))
        print("Failed: ")
        println($(string(expr)))
    else
        print("Success: ")
        println($(string(expr)))
    end)
end
blacklist_set = String[]
macro testset(name::String,expr)
    if name in blacklist_set
        return expr
    end
    :(let 
        println($name)
        $(expr)
    end)
end

macro testset(expr)
    return esc(expr)
end

macro test_eval(expr)
    return :($(esc(simple_lower(expr))))
end

# currently we don't test error, since stack unwind is not implemented.
macro test_throws(expr...)
end

# sinvoke is a helper to invoke code by jl_invoke(*{}, **{}, int) api 
#=
function sinvoke(f,args...)
    arr = Vector{Any}(Any[args...])
    argptr = ccall(:jl_array_ptr,Ptr{Cvoid},(Any,),arr)
    Base.GC.@preserve argptr begin
        ccall(f,Any,(Ptr{Cvoid},Ptr{Cvoid},Int32),C_NULL,argptr,length(arr))
    end
end
=#



