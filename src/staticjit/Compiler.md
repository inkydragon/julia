The invoke pipeline:
    1. The function expr is transformed into a jl_apply_generic call, which requires a invoke pointer (`jl_force_jit`)
    2. To get the invoke pointer, we need to compile the method instance (`getInvokePointer`)
    3. We look up the method instance in the `CacheGraph`
        3.1 We convert method instance to its name
        3.2 We look up the name in CacheGraph
        3.3 If there is a matching, then it must be the case that the MI's Object File is added into JIT, look up jlinvoke symbol in the JITDylib (see `The lookup pipeline`). 
    4. Otherwise we trigger compilation.

The compilation pipeline:

    

The lookup pipeline
    1. The symbol is looked up and all external symbols are looked up recursively
    2. All reference function definitions must exist in the JITDylic now.
    3. External symbols must be registered to JuliaRuntimeGenerator

The cache loading pipeline
    1. We unpack every object file and the config file, every object file should be depicted by exactly one item of the config file
       1. For an invalidated object file, we generate a patched object file.
       2. For a cached object file, we look up the Cache graph to ensure it's indeed a cached one, if not, raise error
       3. Otherwise, it's a normal object file.
    2. Every we add a object file to the Cache Graph, we need to avoid multiple definition. Necessary metadata be registered into CacheGraph.