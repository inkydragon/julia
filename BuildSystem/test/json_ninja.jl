include("ninja.jl")
const buildDir = abspath("/home/chenningcong/Documents/Code/StaticCompiler/julia/Parser")
const outputDir = abspath("/home/chenningcong/Documents/Code/StaticCompiler/julia/Parser/")
const intermediateDir = abspath("/home/chenningcong/Documents/Code/StaticCompiler/julia/Parser/objs/")
@static if !isdefined(Main, :basicTraceParamter)
    const basicTraceParamter = Dict{String, String}("SJ_BUILDDIR"=>buildDir, 
                                                "SJ_OUTPUTDIR" => outputDir, 
                                                "SJ_INTERMEDIATEDIR" => intermediateDir,
                                                "SJ_WORLD"=>"Base")
end
                            
env = BuildEnv(buildDir, outputDir, intermediateDir, splitext(@__FILE__)[1]*".ninja")

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
intResult = trace(env, "json", Vector{String}(["json.jl"]), Vector{BuildResult}([emptyResult]), emptyParameter)

const pkgDir = abspath("/home/chenningcong/Documents/Code/StaticCompiler/julia/stdlib/NetworkOptions-42a0b5fcb7edb8ed5b0ae699f15ca6aedc0098ca/test/")
const pkgParameters = Dict{String, String}("SJ_BUILDDIR"=>pkgDir, 
                                            "SJ_OUTPUTDIR" => outputDir, 
                                            "SJ_INTERMEDIATEDIR" => intermediateDir,
                                            "SJ_WORLD"=>"Main",
                                            "SJ_PRESERVENONRELOCATABLE" => "true")

pkgenv = BuildEnv(pkgDir, outputDir, intermediateDir, splitext(@__FILE__)[1]*".ninja")
packageResult = trace(pkgenv, "network", Vector{String}(["runtests.jl"]), Vector{BuildResult}([emptyResult]), pkgParameters)
const pkgDir1 = abspath("/home/chenningcong/Documents/Code/StaticCompiler/julia/stdlib/Tar-67f004d2af9570c7c19e679e4469bb77e918f0fc/test")
const pkgParameters1 = Dict{String, String}("SJ_BUILDDIR"=>pkgDir1, 
                                            "SJ_OUTPUTDIR" => outputDir, 
                                            "SJ_INTERMEDIATEDIR" => intermediateDir,
                                            "SJ_WORLD"=>"Main",
                                            "SJ_PRESERVENONRELOCATABLE" => "true")
pkgenv2 = BuildEnv(pkgDir1, outputDir, intermediateDir, splitext(@__FILE__)[1]*".ninja")
packageResult = trace(pkgenv2, "Tar", Vector{String}(["runtests.jl"]), Vector{BuildResult}([emptyResult]), pkgParameters1)