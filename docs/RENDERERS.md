# Renderers

The core's Vulkan and OpenGL backends both run on the console, through Mesa translation layers to Direct3D 12, with a choice per game. The earlier plan for a hand-written D3D12 backend was dropped in favour of these.

| Backend | How it runs | Notes |
| --- | --- | --- |
| Vulkan | Mesa **Dozen** (Vulkan on D3D12), patched by `scripts/dozen-patches` | Default. The patches work around console limits, and one catches a crash in the console's shader compiler so the affected draws are skipped. |
| OpenGL | Mesa OpenGL on D3D12 (`onyx_gl.dll`) | Programmable blending reads from a render-target copy rather than shader images. Some games do better here, others only work on Vulkan. |

Console limits that shaped the work:

- No UAVs in pixel shaders.
- Dozen maps some scaled vertex formats to `DXGI_FORMAT_UNKNOWN`, which the console rejects. Vulkan therefore converts those attributes in the shader.
- The console compiles each new pipeline in 30 to 750 ms, so pipelines are built on a background thread by default.
- Frames are read back through a ring of three slots and handed to the UI, so the game does not wait for the previous frame.

Microsoft documents D3D12 feature level 11.0 for UWP on Series X|S; features are checked at runtime.

## Sources

- [Pinned upstream renderer build list](https://github.com/Vita3K/Vita3K/blob/fa296cc043742692ea3a51ff0a82e5683b201f72/vita3k/renderer/CMakeLists.txt)
- [Upstream renderer interface](https://github.com/Vita3K/Vita3K/blob/fa296cc043742692ea3a51ff0a82e5683b201f72/vita3k/renderer/include/renderer/state.h)
- [Microsoft: Xbox UWP resource and DirectX limits](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
- [DirectX team: UWP on Series X|S](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/)
- [Microsoft: GL-on-D3D12 for an Xbox GDK game](https://devblogs.microsoft.com/directx/opengl-on-dx12-enables-steel-assault-to-launch-on-xbox/)
