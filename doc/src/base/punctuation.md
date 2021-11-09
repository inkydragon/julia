# Punctuation

Extended documentation for mathematical symbols & functions is [here](@ref math-ops).

| symbol      | meaning                                                                                     |
|:----------- |:--------------------------------------------------------------------------------------------|
| `@`         | the at-sign marks a [macro](@ref man-macros) invocation; optionally followed by an argument list |
| [`!`](@code-self-ref) | an exclamation mark is a prefix operator for logical negation ("not")                       |
| `a!`        | function names that end with an exclamation mark modify one or more of their arguments by convention |
| `#`         | the number sign (or hash or pound) character begins single line comments                    |
| `#=`        | when followed by an equals sign, it begins a multi-line comment (these are nestable)        |
| `=#`        | end a multi-line comment by immediately preceding the number sign with an equals sign       |
| `$`         | the dollar sign is used for [string](@ref string-interpolation) and [expression](@ref man-expression-interpolation) interpolation |
| [`%`](@ref rem) | the percent symbol is the remainder operator                                            |
| [`^`](@code-self-ref) | the caret is the exponentiation operator                                                    |
| [`&`](@code-self-ref) | single ampersand is bitwise and                                                             |
| [`&&`](@code-self-ref)| double ampersands is short-circuiting boolean and                                           |
| [`\|`](@code-self-ref)| single pipe character is bitwise or                                                         |
| [`\|\|`](@code-self-ref) | double pipe characters is short-circuiting boolean or                                    |
| [`⊻`](@ref xor) | the unicode xor character is bitwise exclusive or                                       |
| [`~`](@code-self-ref) | the tilde is an operator for bitwise not                                                    |
| `'`         | a trailing apostrophe is the [`adjoint`](@code-self-ref) (that is, the complex transpose) operator Aᴴ |
| [`*`](@code-self-ref) | the asterisk is used for multiplication, including matrix multiplication and [string concatenation](@ref man-concatenation) |
| [`/`](@code-self-ref) | forward slash divides the argument on its left by the one on its right                      |
| [`\`](@code-self-ref) | backslash operator divides the argument on its right by the one on its left, commonly used to solve matrix equations |
| `()`        | parentheses with no arguments constructs an empty [`Tuple`](@code-self-ref)                           |
| `(a,...)`   | parentheses with comma-separated arguments constructs a tuple containing its arguments      |
| `(a=1,...)` | parentheses with comma-separated assignments constructs a [`NamedTuple`](@code-self-ref)              |
| `(x;y)`     | parentheses can also be used to group one or more semicolon separated expressions           |
| `a[]`       | [array indexing](@ref man-array-indexing) (calling [`getindex`](@code-self-ref) or [`setindex!`](@code-self-ref)) |
| `[,]`       | [vector literal constructor](@ref man-array-literals) (calling [`vect`](@ref Base.vect))    |
| `[;]`       | [vertical concatenation](@ref man-array-concatenation) (calling [`vcat`](@code-self-ref) or [`hvcat`](@code-self-ref)) |
| `[    ]`    | with space-separated expressions, [horizontal concatenation](@ref man-concatenation) (calling [`hcat`](@code-self-ref) or [`hvcat`](@code-self-ref)) |
| `T{ }`      | curly braces following a type list that type's [parameters](@ref Parametric-Types)          |
| `{}`        | curly braces can also be used to group multiple [`where`](@code-self-ref) expressions in function declarations |
| `;`         | semicolons separate statements, begin a list of keyword arguments in function declarations or calls, or are used to separate array literals for vertical concatenation |
| `,`         | commas separate function arguments or tuple or array components                             |
| `?`         | the question mark delimits the ternary conditional operator (used like: `conditional ? if_true : if_false`) |
| `" "`       | the single double-quote character delimits [`String`](@code-self-ref) literals                        |
| `""" """`   | three double-quote characters delimits string literals that may contain `"` and ignore leading indentation |
| `' '`       | the single-quote character delimits [`Char`](@code-self-ref) (that is, character) literals            |
| ``` ` ` ``` | the backtick character delimits [external process](@ref Running-External-Programs) ([`Cmd`](@code-self-ref)) literals |
| `A...`      | triple periods are a postfix operator that "splat" their arguments' contents into many arguments of a function call or declare a varargs function that "slurps" up many arguments into a single tuple |
| `a.b`       | single periods access named fields in objects/modules (calling [`getproperty`](@ref Base.getproperty) or [`setproperty!`](@ref Base.setproperty!)) |
| `f.()`      | periods may also prefix parentheses (like `f.(...)`) or infix operators (like `.+`) to perform the function element-wise (calling [`broadcast`](@code-self-ref)) |
| `a:b`       | colons ([`:`](@code-self-ref)) used as a binary infix operator construct a range from `a` to `b` (inclusive) with fixed step size `1` |
| `a:s:b`     | colons ([`:`](@code-self-ref)) used as a ternary infix operator construct a range from `a` to `b` (inclusive) with step size `s` |
| `:`         | when used by themselves, [`Colon`](@code-self-ref)s represent all indices within a dimension, frequently combined with [indexing](@ref man-array-indexing) |
| `::`        | double-colons represent a type annotation or [`typeassert`](@code-self-ref), depending on context, frequently used when declaring function arguments |
| `:( )`      | quoted expression                                                                           |
| `:a`        | [`Symbol`](@code-self-ref) a                                                                          |
| [`<:`](@code-self-ref)| subtype operator                                                                            |
| [`>:`](@code-self-ref)| supertype operator (reverse of subtype operator)                                            |
| `=`         | single equals sign is [assignment](@ref man-variables)                                      |
| [`==`](@code-self-ref)| double equals sign is value equality comparison                                             |
| [`===`](@code-self-ref) | triple equals sign is programmatically identical equality comparison                      |
| [`=>`](@ref Pair) | right arrow using an equals sign defines a [`Pair`](@code-self-ref) typically used to populate [dictionaries](@ref Dictionaries) |
| `->`        | right arrow using a hyphen defines an [anonymous function](@ref man-anonymous-functions) on a single line |
| `\|>`        | pipe operator passes output from the left argument to input of the right argument, usually a [function](@ref Function-composition-and-piping) |
| `∘`         | function composition operator (typed with \circ{tab}) combines two functions as though they are a single larger [function](@ref Function-composition-and-piping) |
