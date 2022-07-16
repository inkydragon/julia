# Gen code coverage report for Julia's C/C++ code

## deps
- [`lcov`](http://ltp.sourceforge.net/coverage/lcov.php)

docs:
- http://ltp.sourceforge.net/coverage/lcov/lcov.1.php
- https://linux.die.net/man/1/lcov
- https://wiki.documentfoundation.org/Development/Lcov


## Run code coverage

**build julia-debug**
```sh
pwd
# ~/julia

# clean
make clean
rm -rf julia-lcov/

# copy `Make.user.gcov` to `julia/Make.user`
cp contrib/gcov/Make.user.gcov Make.user

# make debug version
make -j `nproc`  debug
```


### gen report for build process

```sh
# in root dir: `julia`

mkdir julia-lcov/
lcov --no-external --capture --rc lcov_branch_coverage=1  \
    --directory src/ --directory src/support/ --directory src/flisp/ --directory cli/  \
    --output-file julia-lcov/julia_build.info
genhtml --branch-coverage --demangle-cpp --ignore-errors source --legend  \
    --title "[build process] commit `git rev-parse HEAD`"  \
    --output-directory=julia-lcov/lcov-html-build julia-lcov/julia_build.info
```

HTML report is located in `./julia-lcov/lcov-html-build/`.


### gen report for test only

```sh
# in root dir: `julia`

# gen test baseline
lcov --no-external --capture --initial --rc lcov_branch_coverage=1  \
    --directory src/ --directory src/support/ --directory src/flisp/ --directory cli/  \
    --output-file julia-lcov/julia_test_baseline.info
# run julia's test
make -j `nproc` test debug
# get cov data after test
lcov --no-external --capture --rc lcov_branch_coverage=1  \
    --directory src/ --directory src/support/ --directory src/flisp/ --directory cli/  \
    --output-file julia-lcov/julia_test_end.info
# merge data
lcov --rc lcov_branch_coverage=1  \
    --add-tracefile julia-lcov/julia_test_baseline.info  --add-tracefile julia-lcov/julia_test_end.info \
    --output-file julia-lcov/julia_test.info
genhtml --branch-coverage --demangle-cpp --ignore-errors source --legend  \
    --title "[test only] commit `git rev-parse HEAD`"  \
    --output-directory=julia-lcov/lcov-html-test  julia-lcov/julia_test.info
```

HTML report is located in `./julia-lcov/lcov-html-test/`.


## memo notes

- rm all `*.gcda`
```sh
# only rm *.gcda
lcov --zerocounters  --directory src/ --directory src/support/ --directory src/flisp/ --directory cli/
```

- Compile flag `--coverage` works for both gcc and clang
    - https://gcc.gnu.org/onlinedocs/gcc-9.3.0/gcc/Instrumentation-Options.html#index-gcov
    - https://releases.llvm.org/13.0.0/tools/clang/docs/SourceBasedCodeCoverage.html#introduction

- You may want more customizable styles for HTML report.  
    There is a `lcovrc` file: http://ltp.sourceforge.net/coverage/lcov/lcovrc.5.php
