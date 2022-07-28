include("../ninja.jl")
const buildDir = abspath(".")
const outputDir = abspath(".")
const intermediateDir = abspath("objs")
@static if !isdefined(Main, :basicTraceParamter)
    const basicTraceParamter = Dict{String, String}("SJ_BUILDDIR"=>buildDir, 
                                                "SJ_OUTPUTDIR" => outputDir, 
                                                "SJ_INTERMEDIATEDIR" => intermediateDir,
                                                "SJ_WORLD"=>"Base")
end
                            
env = BuildEnv(buildDir, outputDir, intermediateDir, splitext(buildDir*"/")[1]*".ninja")

const startParamter = Dict{String, String}("SJ_BUILDDIR"=>buildDir, 
                                                "SJ_OUTPUTDIR" => outputDir, 
                                                "SJ_INTERMEDIATEDIR" => intermediateDir,
                                                "SJ_WORLD"=>"Base;SHA;Random")
# special case, to reduce Test compiling time and Pkg compiling time
emptyParameter = copy(startParamter)
emptyParameter["SJ_PRESERVENONRELOCATABLE"] = "true"
emptyParameter["SJ_WORLD"] = "Main"
# problem of Core.Compiler
# maybe we should compile Core.Compiler before we actually do some compiling
emptyResult = trace(env, "empty", Vector{String}(["empty.jl"]), Vector{BuildResult}(), emptyParameter)
intResult = trace(env, "int", Vector{String}(["int.jl"]), Vector{BuildResult}([emptyResult]), basicTraceParamter)
floatResult = trace(env, "float", Vector{String}(["floatfuncs.jl"]), Vector{BuildResult}([intResult, emptyResult]), basicTraceParamter)
numberResult = trace(env, "numbers", Vector{String}(["testhelpers/Furlongs.jl", "numbers.jl"]), Vector{BuildResult}([emptyResult, intResult, floatResult]), basicTraceParamter)
arrayResult = trace(env, "array", Vector{String}(["abstractarray.jl"]), Vector{BuildResult}([emptyResult, intResult, floatResult]), basicTraceParamter)
dictResult  = trace(env, "dict", Vector{String}(["dict.jl"]), Vector{BuildResult}([emptyResult, intResult, floatResult, arrayResult]), basicTraceParamter)
bitResult  = trace(env, "bit", Vector{String}(["bitarray.jl", "bitset.jl"]), Vector{BuildResult}([emptyResult, intResult, floatResult, arrayResult]), basicTraceParamter)
gcCache!(env)
