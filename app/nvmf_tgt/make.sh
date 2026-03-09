#!/bin/bash
# compile_simple.sh - 最简单的编译脚本

echo "=== Simple Compilation Script ==="
echo "Using your exact compilation command..."

# 执行编译命令
# 修改1: 用gcc编译.c文件，用g++编译.cpp文件，然后链接
echo "Step 1: Compiling C file with gcc..."
gcc -std=c11 -c nuvcoder_nv12_demo.c -o nuvcoder_nv12_demo.o \
    -I. \
    -I/root/nuvcoder_core/. \
    -I/root/nuvcoder_core/backend/. \
    -I/usr/include/python3.9 \
    -DNUVCODER_PLATFORM_ASCEND=1 \
    -fPIC -O2 -g

if [ $? -ne 0 ]; then
    echo "❌ C file compilation failed"
    exit 1
fi
echo "✅ C file compiled successfully"

echo ""
echo "Step 2: Compiling C++ files with g++..."
g++ -std=c++17 -c /root/nuvcoder_core/nuvcoder_codec.cpp -o nuvcoder_codec.o \
    -I. \
    -I/root/nuvcoder_core/. \
    -I/root/nuvcoder_core/backend/. \
    -I/usr/local/include \
    -I/root/.local/lib/python3.9/site-packages/pybind11/include \
    -I/usr/include/python3.9 \
    -I/usr/local/lib64/python3.9/site-packages/torch/include \
    -I/usr/local/lib64/python3.9/site-packages/torch/include/torch/csrc/api/include \
    -D_GLIBCXX_USE_CXX11_ABI=0 -DNUVCODER_PLATFORM_ASCEND=1 \
    -fPIC -O2 -g

g++ -std=c++17 -c /root/nuvcoder_core/nuvcoder_codec_thread.cpp -o nuvcoder_codec_thread.o \
    -I. \
    -I/root/nuvcoder_core/. \
    -I/root/nuvcoder_core/backend/. \
    -I/usr/local/include \
    -I/root/.local/lib/python3.9/site-packages/pybind11/include \
    -I/usr/include/python3.9 \
    -I/usr/local/lib64/python3.9/site-packages/torch/include \
    -I/usr/local/lib64/python3.9/site-packages/torch/include/torch/csrc/api/include \
    -D_GLIBCXX_USE_CXX11_ABI=0 -DNUVCODER_PLATFORM_ASCEND=1 \
    -fPIC -O2 -g

g++ -std=c++17 -c /root/nuvcoder_core/backend/nuvcoder_backend_pyimpl.cpp -o nuvcoder_backend_pyimpl.o \
    -I. \
    -I/root/nuvcoder_core/. \
    -I/root/nuvcoder_core/backend/. \
    -I/usr/local/include \
    -I/root/.local/lib/python3.9/site-packages/pybind11/include \
    -I/usr/include/python3.9 \
    -I/usr/local/lib64/python3.9/site-packages/torch/include \
    -I/usr/local/lib64/python3.9/site-packages/torch/include/torch/csrc/api/include \
    -D_GLIBCXX_USE_CXX11_ABI=0 -DNUVCODER_PLATFORM_ASCEND=1 \
    -fPIC -O2 -g

if [ $? -ne 0 ]; then
    echo "❌ C++ files compilation failed"
    exit 1
fi
echo "✅ C++ files compiled successfully"

echo ""
echo "Step 3: Linking with g++..."
g++ -std=c++17 -o nuvcoder_nv12_demo \
    nuvcoder_nv12_demo.o \
    nuvcoder_codec.o \
    nuvcoder_codec_thread.o \
    nuvcoder_backend_pyimpl.o \
    -fPIC -O2 -g -rdynamic \
    -L/usr/local/lib64/python3.9/site-packages/torch/lib \
    -L/usr/local/lib64 \
    -L/usr/lib64 \
    -Wl,--start-group \
    -lc10 \
    -ltorch_cpu \
    -ltorch \
    -ltorch_python \
    -Wl,--end-group \
    -lpython3.9 \
    -lstdc++ \
    -pthread -ldl -lrt -Wl,--export-dynamic \
    -Wl,-rpath,/usr/local/lib64/python3.9/site-packages/torch/lib \
    -Wl,-rpath,/usr/local/lib64 \
    -Wl,-rpath,/usr/lib64

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Success! Executable: ./nuvcoder_nv12_demo"
    echo ""
    echo "Run with:"
    echo "  ./nuvcoder_nv12_demo <parameters>"
    
    # 清理中间文件
    echo ""
    echo "Cleaning up object files..."
    rm -f nuvcoder_nv12_demo.o nuvcoder_codec.o nuvcoder_codec_thread.o nuvcoder_backend_pyimpl.o
else
    echo "❌ Linking failed"
    exit 1
fi