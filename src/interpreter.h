typedef struct{
    int fast; // whether we need to use llvm to compile function we met
    int expanded; // whether the expression needed to be expanded
    int jit; // whether we should use static jit
} jl_interp_ctx;
