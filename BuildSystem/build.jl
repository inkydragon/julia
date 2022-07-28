function dumpGraph()
    x = []
    ccall(:jl_dump_graph, Cvoid, (Any,), x)
    return x
end

function disableOutput()
    ccall(:jl_disable_output,Cvoid,())
end

import Pkg
#=
function collectDependencies()
    dep = Pkg.dependencies()
    flattenDeps = Vector{Tuple{Symbol, Symbol}}()
    for (_, info) in dep
        for (name, _) in info.dependencies
            push!(flattenDeps, (Symbol(info.name), Symbol(name)))
        end
    end
    return flattenDeps
end

function collectDependenciesDict()
    dep = Pkg.dependencies()
    flattenDeps = Dict{Symbol, Vector{Symbol}}()
    for (_, info) in dep
        arr = Symbol[]
        for (name, _) in info.dependencies
            push!(arr, Symbol(name))
        end
        flattenDeps[Symbol(info.name)] = arr
    end
    return flattenDeps
end
=#

function getCurrentDependency()
    dep = Pkg.dependencies()
end

function freezeDependency()
    dep = getCurrentDependency()
    flattenDeps = Vector{Tuple{Symbol, Symbol}}()
    for (_, info) in dep
        for (name, _) in info.dependencies
            push!(flattenDeps, (Symbol(info.name), Symbol(name)))
        end
    end
    ccall(:jl_freeze_dependency, Cvoid, (Any,), flattenDeps)
end

hex2byte(x::UInt8) = x < UInt8('A') ? UInt8(x) - UInt8('0') : UInt8(x) - UInt8('A') + UInt8(10)
byte2hex(x::UInt8) = x < 10 ? x + UInt8('0') : x - 10 + UInt8('A')
function encodeString(s::String)::String
    ccall(:jl_encode_string, Any, (Any,), s)::String
end

function decodeString(s::String)::String
    ccall(:jl_decode_string, Any, (Any,), s)::String
end

function lookUpMethodInstance(tt, world)
    mi_matches = Core.Compiler._methods_by_ftype(tt, -1, world)
    if Core.Compiler.:(>=)(Core.Compiler.length(mi_matches),1)
        instance = Core.Compiler.specialize_method(Core.Compiler.getindex(mi_matches,1)#=MethodMatch=#)
        return instance
    else
        error("Not a unique match for $f with input type $t")
    end
end
#=
format
the meaning is different when we load them into cache
when we load a JITMethodInstance, it will become a CachedMethodInstance!
    JITMethodInstance 
        node type 0
        miName (encoded)
        isRelocatable
        if !isRelocatable
            function type (encoded)
            input type (encoded)
        objectFilePath
        symbolTable
    CachedMethodInstance
        node type 1
        miName (encoded)
        libName (encoded)
        objName (encoded)
    PluginMethodInstance
        node type 2
        miName (encoded)
        pluginName (encoded)
                
=#
struct WriteConfigOp <: Function
    config::Set{Any}
end
function (op::WriteConfigOp)(f)
    config = op.config
    for miNode in config
        if miNode isa JITMethodInstance
            println(f, '0')
            println(f, encodeString(miNode.miName))
            println(f, miNode.isRelocatable ? "1" : "0")
            mi = miNode.mi
            if !miNode.isRelocatable
                println(f, ccall(:jl_encodeJuliaValue, String, (Any,), mi.specTypes))
            end
            println(f, encodeString(splitdir(miNode.objectFilePath)[2]))
            println(f, string(length(miNode.symbolTable)))
            for (s, l) in miNode.symbolTable
                println(f, encodeString(s), '\t', l)
            end
        elseif miNode isa CachedMethodInstance
            println(f, '1')
            println(f, encodeString(miNode.miName))
            println(f, encodeString(miNode.libName))
            println(f, encodeString(miNode.objName))
        elseif miNode isa PluginMethodInstance
            println(f, '2')
            println(f, encodeString(miNode.miName))
            println(f, encodeString(miNode.pluginName))
        end
    end
end

function writeConfig(path::String, config::Set{Any})
    open(WriteConfigOp(config), path; write=true)
end
#=
function openConfig(path::String)
    config = Vector{Any}()
    open(path) do f
        while !eof(f)
            miName = decodeString(readline(f))
            isCached = Bool(parse(Int, readline(f)))
            if !isCached
                mString = readline(f)
            else
                mString = nothing
            end
            isInvalidated = Bool(parse(Int, readline(f)))
            if isInvalidated
                specTypes = ccall(:jl_decodeJuliaValue, Any, (Any, Any), Main, readline(f))
                mi = lookUpMethodInstance(specTypes, Base.get_world_counter())
                println(mi)
            end
            objectFileName = decodeString(readline(f))
            pairNum = parse(Int, readline(f))
            dep = Vector{Any}()
            for i in 1:pairNum
                onePair = split(readline(f), '\t')
                push!(dep, (decodeString(String(onePair[1])), String(onePair[2])))
            end
            if isInvalidated
                push!(config, (miName, Any[mString, isInvalidated, isCached, objectFileName, dep, mi]))
            else
                push!(config, (miName, Any[mString, isInvalidated, isCached, objectFileName, dep]))
            end
        end
    end
    return config
end
=#
struct CdFunction <: Function
    files::Vector{String}
end
# used to prevent unnamed function
function (cdf::CdFunction)()
    for f in cdf.files
        include(f)
    end
end

function traceFiles(buildDir::String, outputDir::String, intermediateDir::String, files::Vector, libName::String, UpAge::Vector{Symbol}, preserveNonRelocatable::Bool)
    libPath = abspath(joinpath(outputDir, libName * ".lib"))
    println("Compiling $(libPath)")
    func = CdFunction(files)
    cd(func, buildDir)
    info = Set{Any}(dumpGraph())
    if !preserveNonRelocatable
        removeNonRelocatale!(info)
    end
    freezeDependency()
    ages = Set{Vector{Symbol}}()
    for i in info
        if i isa JITMethodInstance
            push!(ages, calculateWorld(i.mi))
        end
    end
    worlds = String[]
    for i in ages
        ss = String[]
        for j in i
            push!(ss, string(j))
        end
        push!(worlds, '[' * join(ss, ',') * ']')
    end
    println(worlds)
    legals::Set{Any} = filterUpperWorld(info, UpAge)
    writeConfig(abspath(joinpath(outputDir, libName * ".config")), legals)
    objPaths = String[]
    for i in info
        if i isa JITMethodInstance
            push!(objPaths, i.objectFilePath)
        end
    end
    command = `ar rcs $libPath $objPaths`;
    run(command);
    nothing
end

const LoadedLibs = Set{String}([])

function loadLib(libPath::String, configPath::String)
    if !(libPath in LoadedLibs)
        println("Loading $(libPath)")
        ccall(:jl_add_static_libs, Cvoid, (Any,), [(libPath, configPath)])
        push!(LoadedLibs, libPath)
        println("Finish loading!")
    else
        println("$(libPath) is already loaded and cached!")
    end
end

function loadLibAndTest(folder::String, traceFile::String, ::String, configPath::String)
    loadLib(libPath, configPath)
    if traceFile !== nothing
        func = CdFunction(String[traceFile])
        cd(func, folder)
    end
end

function findPackage(name::String)
    d = Pkg.dependencies()
    for (k, v) in d
        if v.name == name
            return Base.PkgId(k, name)
        end
    end
end

function bfs!(node::JITMethodInstance, result::Set{JITMethodInstance}, g::Dict{JITMethodInstance, Set{JITMethodInstance}}, color::Dict{JITMethodInstance, Bool})
    if !(node in result)
        push!(result, node)
        for i in g[node]
            if i isa JITMethodInstance
                bfs!(i, result, g, color)
            end
        end
    end
end

function removeIfClosure!(f::Function, info)
    g = Dict{JITMethodInstance, Set{JITMethodInstance}}()
    for miNode in info
        if miNode isa JITMethodInstance
            g[miNode] = Set{JITMethodInstance}()
        end
    end
    color = Dict{JITMethodInstance, Bool}()
    for miNode in info
        if miNode isa JITMethodInstance
            color[miNode] = f(miNode)::Bool
            for ch in miNode.dependencies
                if ch isa JITMethodInstance
                    push!(g[ch], miNode)
                end
            end
        end
    end
    result = Set{JITMethodInstance}()
    for (k, b) in color
        if b
            bfs!(k, result, g, color)
        end
    end
    return setdiff!(info, result)
end

function isNonReloatable(x::JITMethodInstance)
    !x.isRelocatable
end

function removeNonRelocatale!(x)
    return removeIfClosure!(isNonReloatable, x)
end
function isWorldIn(m::JITMethodInstance, world)
    age = calculateWorld(m.mi)::Vector{Symbol}
    for w in world
        for a in age
            if cmpWorld(a, w) in ("Smaller", "Eq")
                return true
            end
        end
    end
    return false
end

function isWorldNotIn(m::JITMethodInstance, world)
    !isWorldIn(m , world)
end

struct FilterUpperWorldOp <: Function
    upper::Vector{Symbol}
end

function (op::FilterUpperWorldOp)(@nospecialize(m::Any))::Bool
    if m isa JITMethodInstance
        return isWorldNotIn(m, op.upper)
    end
    return false
end

function filterUpperWorld(info, upper::Vector{Symbol})
    removeIfClosure!(FilterUpperWorldOp(upper), info)
end

#=
load_object(objname::String) = ccall(:jl_compile_objects,Cvoid,(Any,),String[objname])
function load_object(objnames::Vector)
    retypearray = String[]
    for i in objnames
        if isa(i,String)
            push!(retypearray,i)
        else
            error("Not a string.")
        end
    end
    ccall(:jl_compile_objects,Cvoid,(Any,),retypearray)
end
=#