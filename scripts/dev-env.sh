# Source this in the managed cloud image if CMake is not already on PATH.
if ! command -v cmake >/dev/null 2>&1 && [ -x /workspace/tools/cmake/data/bin/cmake ]; then
    export PATH="/workspace/tools/cmake/data/bin:$PATH"
fi
