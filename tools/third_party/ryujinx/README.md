# Ryujinx standalone shader compiler

Vendored subset: Ryujinx.Graphics.Shader, Ryujinx.Common and Spv.Generator
from Ryubing 1.3.2 through D2R Mobile. These are offline build tools; no
emulator runtime is included in the Android application.

Local modifications declare the storage-image-write capability, preserve phi
predecessor edges, resolve proven fixed bindless handles, and lower PIXLD
MY_INDEX to SampleId with its required capability. See the synthetic shader
regression tests and the root THIRD_PARTY_NOTICES.md.

License: MIT, see LICENSE.txt. Upstream: https://github.com/Ryubing .
