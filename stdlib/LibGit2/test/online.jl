# This file is a part of Julia. License is MIT: https://julialang.org/license

mktempdir() do dir
    dir = realpath(dir)
    HOME = Sys.iswindows() ? "USERPROFILE" : "HOME"
    withenv(HOME => dir) do
        include("online-tests.jl")
    end
end
