#include "llvm/Support/Error.h"
enum JuliaJITInternalError {
    UnusedPlaceHolder = 0,
    // Library symbols not found
    InternalSymbolNotFound,
    ExternalSymbolNotFound,
    // Evaluation failed
    EvaluationFailed,
    // TypeMismatched
    DataTypeMismatched,
    // Regex Error
    RegexError,
    // Unhandled Symbols
    UnhandledSymbols
};
namespace std {
template<>
struct is_error_code_enum<JuliaJITInternalError> : true_type {
};
}

class JuliaJITInternalErrorCategory : public std::error_category {
    const char *name() const noexcept { return "Internal jitlink error:"; }
    std::string message(int ev) const override
    {
        switch (static_cast<JuliaJITInternalError>(ev)) {
        case JuliaJITInternalError::InternalSymbolNotFound:{
            return "Non-existent internal julia symbol";
        }
        case JuliaJITInternalError::ExternalSymbolNotFound:{
            return "Non-existent external julia symbol:";
        }
        case JuliaJITInternalError::EvaluationFailed:{
            return "Evaluation failed:";
        }
        case JuliaJITInternalError::DataTypeMismatched:{
            return "Type of evaluation mismatched:";
        }
        case JuliaJITInternalError::RegexError:{
            return "regex error:";
        }
        case JuliaJITInternalError::UnhandledSymbols:{
            return "Unhandled symbols:";
        }
        default: return "(unrecognized error)";
        }
    }
};

const JuliaJITInternalErrorCategory theJuliaJITInternalErrorCategory{};
std::error_code make_error_code(JuliaJITInternalError err)
{
    return {err, theJuliaJITInternalErrorCategory};
}
