const StringPool = Any[];
const SymbolPool = Any[];
const BitValuePool = Any[];
const TypePool = Any[]
# prevent get into type inference...
Core.eval(Base,:(function NamedTuple{names}(nt::NamedTuple) where {names}
if @generated
    idx = Int[ fieldindex(nt, names[n]) for n in 1:length(names) ]
    types = Tuple{(fieldtype(nt, idx[n]) for n in 1:length(idx))...}
    Expr(:new, :(NamedTuple{names, $types}), Any[ :(getfield(nt, $(idx[n]))) for n in 1:length(idx) ]...)
else
    types = Tuple{(fieldtype(typeof(nt), names[n]) for n in 1:length(names))...}
    NamedTuple{names, types}(map(Fix1(getfield, nt), names))
end
end))
const MethodBlacklist = [which(Base.return_types,(Any,Any)),
which(Core.Compiler.return_type,(Any,Any,UInt64)),
which(Core.Compiler.return_type,(Core.Compiler.NativeInterpreter,Any,UInt64)),
which(Base.CoreLogging.handle_message,(Base.CoreLogging.NullLogger, Any, Any, Any, Any, Any, Any, Any)),
which(Base.Docs.doc!,(Module, Base.Docs.Binding, Base.Docs.DocStr, Any)),
which(Base.CoreLogging.env_override_minlevel,(Symbol, Module))]

# default build dir, used for repl testing
include("include.jl")

# include("collectDI.jl")
function chokeString(in::String, s::String)
    if !startswith(in, s)
        error("String $in not started with $s")
    end
    return in[length(s):end]
end

setOutput() = ccall(:jl_setoutput_dir,Cvoid,(Any,), IntermediateDir[])
macro inferred(x)
    return esc(x)
end
# Prevent type inference calling from inside
@inferred 1+1

const BuildDir = Ref{String}(abspath(joinpath(pwd(), "test")))
const IntermediateDir = Ref{String}(abspath(joinpath(BuildDir[], "objs")))
const OutputDir = Ref{String}(BuildDir[])
const FromREPL = Ref{Bool}(true)
const InputFiles = Ref{Vector{String}}(String[])
const LibName = Ref{String}("")
const InputWorlds = Ref{Vector{Symbol}}(Symbol[])
const PreserveNonRelocatable = Ref{Bool}(false)

if haskey(ENV, "SJ_BUILDDIR")
    BuildDir[] = ENV["SJ_BUILDDIR"]
    IntermediateDir[] = ENV["SJ_INTERMEDIATEDIR"]
    OutputDir[] = ENV["SJ_OUTPUTDIR"]
    InputFiles[] = split(ENV["SJ_INPUTS"], ';')
    LibName[] = ENV["SJ_LIBNAME"]
    InputWorlds[] = map(Symbol, split(ENV["SJ_WORLD"]))
    FromREPL[] = false
end

if haskey(ENV, "SJ_PRESERVENONRELOCATABLE")
    PreserveNonRelocatable[] = true
end

#=
import CompilerSupportLibraries_jll
import dSFMT_jll
import GMP_jll
import libblastrampoline_jll
import LibCURL_jll
import LibGit2_jll
import libLLVM_jll
import LibSSH2_jll
import LibUnwind_jll
import LibUV_jll
import LLVMLibUnwind_jllf
import MbedTLS_jll
import MozillaCACerts_jll
import MPFR_jll
import nghttp2_jll
import OpenBLAS_jll
import OpenLibm_jll
import p7zip_jll
import PCRE2_jll
import SuiteSparse_jll
import Zlib_jll
=#

const DebugStream = joinpath(IntermediateDir[], "debug.csv")
ccall(:jl_set_register_module_handle, Cvoid, ())
ccall(:jl_init_staticjit,Cvoid,(Any,), IntermediateDir[])

import Artifacts
import Base64
import CRC32c
import Dates
import DelimitedFiles
import Distributed
import Downloads
import FileWatching
import Future
import InteractiveUtils
import JLLWrappers
import LazyArtifacts
import LibCURL
import LibGit2
import Libdl
import LinearAlgebra
import Logging
import Markdown
import Mmap
import Printf
import Profile
import REPL
import Random
import Random.shuffle
import RelocatableFolders
import SHA
import Scratch
import Serialization
import SharedArrays
import Sockets
import SparseArrays
import TOML
import UUIDs
import Unicode
import Tar
import ArgTools
import Downloads
import LibCURL
import NetworkOptions
import p7zip_jll
using Test
if !FromREPL[]
    local includeLibs = ENV["SJ_LIBS"]
    local includeConfigs = ENV["SJ_LIBCONFIGS"]
    if length(includeLibs) > 0
        libpaths = split(includeLibs, ';')
        libconfigs = split(includeConfigs, ';')
        for i in 1:length(libpaths)
            ln = String(libpaths[i])
            cn = String(libconfigs[i])
            if length(ln) > 0
                loadLib(ln, cn)
            end
        end
    end
end

ccall(:jl_staticjit_set_cache_geter,Cvoid,(Ptr{Nothing},),cglobal(:jl_simple_multijit))
ccall(:jl_set_get_cfunction_ptr,Cvoid,(Ptr{Nothing},),cglobal(:jl_get_spec_ptr))

ccall(:jl_set_debug_stream,Cvoid,(Any,), DebugStream)
for i in 1:1
    if !FromREPL[]
        traceFiles(BuildDir[], OutputDir[], IntermediateDir[], InputFiles[], LibName[], InputWorlds[], PreserveNonRelocatable[])
    end
end

