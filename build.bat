I:
cd I:\code\lobotomy-x\UEVR
rem git submodule update --init --recursive
cmake -S . -B build ./build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Debug
cmake --build ./build --config Debug --target uevr
