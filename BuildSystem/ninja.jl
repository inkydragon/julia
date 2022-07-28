#=
struct BuildGraph
end
=#
#=
    Design of BuildGraph:
        BuildGraph is a acyclic graph, much like a data flow graph.
        View from the perspect of static analysis, BuildGraph is a huge expression.
        Modification to the graph is the modification to the expression.
        Incremental compilation is simply a decision procedure for compilation. 
=#

#=
    Expr is a operator ???
    struct BuildGraph
        graph::AcyclicGraph
        root
        cacheResults::Dict{Expr, CompileResult}
    end
    TraceResult <: CompileResult
    MergeResult <: CompileResult
    # Incremental computing function
    <Expr>::CompileResult :=
        # <Id> is depedency of library
        | <Id> = TraceFile(#filename, <Id>...)::TraceResult # tracing the object file and produce config and 
        | <Id> = Merge(<Id>...)::MergeResult # Merge of multiple object files together into a single object file
        | <Id> = Impure()::ImpureResult # Force update
    
    #  Directives
    Order(<Id>...) # Force order over builds (Explicit Depedency)
    Output(<Id>...) # Directives, can be executed multiple times

    compile() => compile the BuildGraph and save the result to the cache graph
    gc() => remove the cache that is not used in this compilation procedure
    Simple case:
        trace("int.jl") => TraceResult(input = "int.jl", )
    # how is the TraceResult rebuild ?
    Example:
        int.o : trace(int.jl);
        float.o : trace(float.jl);
        number.o : merge(int.o, float.o);
        array.o : trace(array.jl, number.o);
        output(...)
=# 
import Dates
import Serialization
import Serialization.serialize
import Serialization.deserialize
struct BuildFile
    filepath::String # always use full path
    mtime::Dates.DateTime
end

const BuildResult = Any
const LibOutputType = NamedTuple{(:lib, :config), Tuple{BuildResult, BuildResult}} 
struct TraceResult
    input::Vector{BuildFile} # input of trace files
    dependency::Vector{BuildResult} # load of dep name
    output::LibOutputType # output of tracing, should be two path of lib and config !
    parameters::Dict{String, String} # a list of parameters for julia compiler
end

function Base.:(==)(l::TraceResult, r::TraceResult)
    isEq = l.input == r.input && l.output == r.output && l.parameters == r.parameters && l.dependency == r.dependency
    if !isEq
        return false
    end
    return true
end

struct MergeResult
    input::Vector{BuildResult} # input of object files
    output::LibOutputType # output of object files, should be two path of lib and config !
    parameters::Dict{String, String} # a list of parameters for object linker
end

function Base.:(==)(l::MergeResult, r::MergeResult)
    return l.input == r.input && l.output == r.output && l.parameters == r.parameters
end

# Unsupported now
struct ImpureResult end
isImpure(r::ImpureResult) = true
function isImpure(r::TraceResult) 
    if haskey(r.parameters, "rebuild")
        if r.parameters["rebuild"] == "true"
            return true
        end
        for i in r.dependency
            if isImpure(i)
                return true
            end
        end
    end
    return false
end

function isImpure(r::MergeResult)
    for i in r.input
        if isImpure(i)
            return true
        end
    end
    return false
end

# Given a set of files with old build time, we check whether the input matches the old input, if so, we check whether the output exists
function isOutputfilesValid(files)
    for f in files
        if !isfile(f.filepath)
            return false
        end
        curTime = Dates.unix2datetime(mtime(f.filepath))
        if curTime != f.mtime
            return false
        end
    end
    return true
end

function isOutputfilesValid(files::LibOutputType)
    for fn in fieldnames(LibOutputType)
        p = getfield(files, fn)
        f = p.filepath
        t = p.mtime
        if !isfile(f)
            return false
        end
        curTime = Dates.unix2datetime(mtime(f))
        if curTime != t
            return false
        end
    end
    return true
end

struct SafeDict{K, V}
    table::Vector{Pair{K, V}}
    function SafeDict{K,V}() where {K,V}
        return new(Pair{K,V}[])
    end
end

function Base.getindex(d::SafeDict{K,V}, k::K) where {K, V}
    for (i,v) in d.table
        if i == k
            return v
        end
    end
    Base.KeyError("Key $k doesn't exist!")
end

function Base.setindex!(d::SafeDict{K,V}, v::V, k::K) where {K, V}
    for i in eachindex(d.table)
        k_, _ = d.table[i]
        if k == k_
            d.table[i] = k => v
            return
        end
    end
    push!(d.table, k => v)
end

function Base.keys(d::SafeDict)
    (k for (k, _) in d.table)
end

function Base.haskey(d::SafeDict{K, V}, k::K) where {K,V}
    for (i, _) in d.table
        if i == k
            return true
        end
    end
    return false
end

struct BuildEnv
    # where should we find the build file
    buildDir::String
    # where should we save the result
    outputDir::String
    # where the intermediate result is cached
    intermediateDir::String
    # cached result, Bool is used to indicate whether this result is used in this build
    # used in gc implementation !
    cache::SafeDict{BuildResult, Bool}
    # we currenly flush to disk every time we update out cache...
    persistantLog::String
    function BuildEnv(buildDir::String, outputDir::String, intermediateDir::String, persistantLog::String)
        cache = SafeDict{BuildResult, Bool}()
        if isfile(persistantLog)
            persistantLog = abspath(persistantLog)
            results = Serialization.deserialize(persistantLog)
            for i in results
                cache[i] = false
            end
        end   
        if !isdir(buildDir)
            error("BuildDir $buildDir is not a dir!")
        elseif !isdir(outputDir)
            error("OutputDir $outputDir is not a dir!")
        elseif !isdir(intermediateDir)
            error("IntermediateDir $intermediateDir is not a dir!")
        end
        new(buildDir, outputDir, intermediateDir, cache, persistantLog)
    end
end

@noinline function commit(env::BuildEnv)
    Serialization.serialize(env.persistantLog, collect(keys(env.cache)))
end

function gcCache!(env::BuildEnv)
    c = env.cache
    nc = Vector{BuildResult}()
    for k in keys(c)
        if c[k]
            push!(nc, k)
        end
    end
    commit(env)
end

function runBuild(r::TraceResult)::TraceResult
    io_param = copy(r.parameters)
    io_param["SJ_INPUTS"] = join(map(x->x.filepath, collect(r.input)), ';')
    io_param["SJ_LIBNAME"] = splitext(splitdir(collect(r.output)[1].filepath)[2])[1]
    libs = String[]
    configs = String[]
    for i in r.dependency
        if isa(i, TraceResult)
            o = i.output
            lib = o.lib
            config = o.config
            push!(libs, lib.filepath)
            push!(configs, config.filepath)
        elseif isa(i, MergeResult)
            error("Not implemented")
        elseif isa(i, ImpureResult)
        else
            error("Unsupported")
        end
    end
    io_param["SJ_LIBS"] = join(libs, ';')
    io_param["SJ_LIBCONFIGS"] = join(configs, ";")
    path = abspath(joinpath(@__DIR__, "../usr/bin/julia-debug"))
    prepareJIT = abspath(joinpath(@__DIR__, "prepareJIT.jl"))
    cmd = Cmd(`$path -t 1 -O 2 --image-codegen -- $prepareJIT`, env=io_param)
    cd(splitdir(@__FILE__)[1])
    cd("../") do
        run(cmd)
    end
    # we need to update the output time stamp to reflect the state of output
    newoutput::LibOutputType = (lib = makeBuildFile(r.output.lib.filepath, true),  config = makeBuildFile(r.output.config.filepath, true))
    TraceResult(r.input, r.dependency, newoutput, r.parameters)
end

function runBuild(r::MergeResult)::MergeResult
    error("Merge is not implemented")
end

function makeBuildFile(filepath::String, check::Bool = false)
    if isfile(filepath)
        BuildFile(filepath, Dates.unix2datetime(mtime(filepath)))
    else
        if check
            error("Build file $filepath doesn't exist.")
        else
            BuildFile(filepath, Dates.unix2datetime(0.0))
        end
    end
end

#trace(buildEnv, outname::String, filenames::Set{String}) = trace(buildEnv, outname, filenames, Dict{String, String}())
function trace(buildEnv, outname::String, filenames::Vector{String}, dependency::Vector{BuildResult}, parameter::Dict{String, String})::TraceResult
    if occursin('.', outname)
        error("Trace error : Output filename $outputname should not contain a '.' inside.")
    end
    libName::BuildFile = makeBuildFile(abspath(joinpath(buildEnv.outputDir, outname *".lib")))
    configName::BuildFile = makeBuildFile(abspath(joinpath(buildEnv.outputDir, outname*".config")))
    fullpaths = Vector{BuildFile}()
    for i in filenames
        push!(fullpaths, makeBuildFile(abspath(joinpath(buildEnv.buildDir, i)), true))
    end
    output::LibOutputType = LibOutputType((libName, configName))
    result = TraceResult(fullpaths, dependency, output, parameter)
    if haskey(buildEnv.cache, result) && !isImpure(result) && isOutputfilesValid(output)
        buildEnv.cache[result] = true
        return result
    end
    # output is updated, so we need to use a new update
    println(result)
    newresult = runBuild(result)
    buildEnv.cache[newresult] = true
    # commit after the result is appended in the cache
    commit(env)
    return result
end

function merge(buildEnv, outname::String, input::Vector{BuildResult}, parameters::Dict{String, String})::MergeResult
    if !endswith(outname, ".lib")
        error("Merge error : Output filename $outputname should end with \".lib\"")
    end
    libName::BuildFile = makeBuildFile(abspath(joinpath(buildEnv.outputDir, outname)))
    result = MergeResult(input, Vector{BuildFile}(BuildFile[libName]), parameters)
    if haskey(buildEnv.cache, result) && !isImpure(result) && isOutputfilesValid(output)
        buildEnv.cache[result] = true
        return result
    end
    newresult = runBuild(result)
    buildEnv.cache[newresult] = true
    return result
end

function impure()
    result = ImpureResult()
    buildEnv.cache[result] = true
    return result # actually no need to insert it into the dictionary, but for safety we insert it still 
end
#=
function addParam(i::Dict{String, String}, k::String, v::String)
    copy(i)[k] = v
end
=#
