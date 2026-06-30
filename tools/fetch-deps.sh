#!/bin/bash
# fetch-deps.sh — fetch the vendored third-party libraries the engine needs.
# Run once after cloning. Re-running is safe (it overwrites/refreshes).
#
#   tools/fetch-deps.sh
#
# Fetches:
#   libs/nuklear/nuklear.h        (Immediate-Mode-UI/Nuklear)   — all UI
#   libs/cgltf/cgltf.h            (jkuhlmann/cgltf)             — GLB loading
#   libs/cgltf/stb_image.h        (nothings/stb)               — textures
#   libs/JoltPhysics/             (jrouwe/JoltPhysics)         — physics
set -e
cd "$(dirname "$0")/.."

mkdir -p libs/nuklear libs/cgltf

echo ">>> nuklear.h"
curl -fsSL https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/nuklear.h -o libs/nuklear/nuklear.h

echo ">>> cgltf.h"
curl -fsSL https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h -o libs/cgltf/cgltf.h

echo ">>> stb_image.h"
curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o libs/cgltf/stb_image.h

if [ ! -e libs/JoltPhysics/Jolt/Jolt.h ]; then
    echo ">>> cloning JoltPhysics (shallow)"
    rm -rf libs/JoltPhysics
    git clone --depth 1 https://github.com/jrouwe/JoltPhysics.git libs/JoltPhysics
else
    echo ">>> JoltPhysics already present"
fi

echo ""
echo "Done. Next:  make jolt   (builds the physics lib, cached)  then  make run"
