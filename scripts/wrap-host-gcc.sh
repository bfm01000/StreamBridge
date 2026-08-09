#!/bin/bash
# 隔离包装脚本：只为 MSYS2 gcc 提供其 DLL 路径，不暴露 pkg-config 等工具
export PATH="/c/msys64/ucrt64/bin:$PATH"
exec /c/msys64/ucrt64/bin/gcc.exe "$@"
