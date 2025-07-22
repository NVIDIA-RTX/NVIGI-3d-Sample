if exist "tools\cmake-windows-x86_64\bin\cmake.exe" (
    set CMAKE_LOCAL=tools\cmake-windows-x86_64\bin\cmake.exe 
) else (
    set CMAKE_LOCAL=cmake
)

if exist project.xml call .\tools\packman\packman.cmd pull -p windows-x86_64 project.xml

%CMAKE_LOCAL% %* -S . -B _build -DDONUT_WITH_VULKAN=ON -DVULKAN_SDK=external\\vulkanSDK -DDXC_SPIRV_PATH=external\\vulkanSDK\\bin\\dxc.exe