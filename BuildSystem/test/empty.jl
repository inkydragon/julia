import Test
if !isdefined(Main, :__empty__included)
    global __empty__included = 1
end
for i in 1:1
    global __empty__included += 1
println(Test.@testset "trigger" begin
    println(Test.@test 1+1==2)
    println(Test.@test_throws ErrorException error())
end)
try 
    display(Test.@test 1+1)
catch e
    display(e)
end
try 
    display(Test.@test error())
catch e
    display(e)
end
    if __empty__included < 3
        include("empty.jl")
    end
end