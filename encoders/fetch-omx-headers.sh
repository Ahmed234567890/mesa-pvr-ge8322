#!/bin/bash
# Fetch the standard OpenMAX IL headers needed to build omx-enc.
#
# The vendor blob implements the OpenMAX IL 1.1.x API.  These are the plain
# AOSP headers (Apache-2.0), not vendor code; android-7.1.2 matches the
# component's ABI.  They are only needed at build time.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-"$HERE/.omx-headers"}
TAG=${TAG:-android-7.1.2_r39}
BASE="https://android.googlesource.com/platform/frameworks/native/+/refs/tags/$TAG/include/media/openmax"

mkdir -p "$DEST"
for h in OMX_AsString.h OMX_Audio.h OMX_AudioExt.h OMX_Component.h \
         OMX_ContentPipe.h OMX_Core.h OMX_IVCommon.h OMX_Image.h \
         OMX_Index.h OMX_IndexExt.h OMX_Other.h OMX_Types.h \
         OMX_Video.h OMX_VideoExt.h; do
    echo "fetching $h"
    curl -fsSL "$BASE/$h?format=TEXT" | base64 -d > "$DEST/$h"
done
echo "headers in $DEST"
