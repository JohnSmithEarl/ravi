mkdir asmvm
cd asmvm
cmake -DCMAKE_INSTALL_PREFIX=c:\ravi -DSTATIC_BUILD=ON -DCMAKE_BUILD_TYPE=Debug -DASM_VM=ON -G "Visual Studio 15 Win64" ..
cd ..