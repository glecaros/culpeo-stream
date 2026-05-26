#!/usr/bin/env bash
set -euo pipefail

# --- C++ toolchain (clang 20 + libc++ for C++23 support) ---
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" | sudo tee /etc/apt/sources.list.d/llvm-20.list
sudo apt-get update -qq
sudo apt-get install -y -qq clang-20 libc++-20-dev libc++abi-20-dev cmake ninja-build

# Set clang-20 as default
sudo update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-20   100
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 100
