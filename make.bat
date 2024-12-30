if exist "tools\cmake-windows-x86_64\bin\cmake.exe" (
    set CMAKE_LOCAL=tools\cmake-windows-x86_64\bin\cmake.exe 
) else (
    set CMAKE_LOCAL=cmake
)

%CMAKE_LOCAL% %* -S . -B _build