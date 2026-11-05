# Guest DirectX contract — experimental v6 (interfaces unchanged from v4)

The bridge is `wince::DirectX`, in `runtime/src/wince/directx.cpp`. All pointers, structure fields
and vtable entries are 32-bit guest addresses. Integer arguments use SH-4 R4–R7, then the caller's
stack at SP+16; results use R0. The first 16 stack bytes are reserved register home slots.
This includes COM's first `this` argument. Interface GUIDs, layouts
and method positions follow the public legacy DirectX declarations in Microsoft's Windows SDK
(`ddraw.h`, `d3d.h`, `dsound.h`), not the host compiler's pointer size or host COM objects.

Factories and methods are process-local interpreter traps. Objects share a COM identity and
reference count across supported interfaces; related surfaces/devices/buffers retain their backing
objects. Unsupported QueryInterface requests return E_NOINTERFACE and clear the output pointer.
Other unimplemented methods return E_NOTIMPL, rather than a fake success. Invalid pointers,
descriptors, flags and indices return failing HRESULTs. `diagnostic()` records the latest failure.

V4 fixes v3's stack-argument offset in the bridge and fixture generators. DirectSound Lock and
the other methods with more than four total parameters are tested with poisoned home slots.
The Windows convention is documented in Microsoft's [SuperH calling-convention article](https://devblogs.microsoft.com/oldnewthing/20190820-00/?p=102792)
and was cross-checked against the locally available SH-4 module's parameter-spill layout.
Regenerate old synthetic executables with the v4 generators before running them on this bridge.

## DirectDraw

`DirectDrawCreate(NULL, out, NULL)` creates IDirectDraw (version 1). `DirectDrawCreateEx` accepts
IID_IDirectDraw4 only. QueryInterface supports Draw1, Draw4 and Direct3D3 on the same underlying
object. Adapter enumeration, explicit device GUIDs and COM aggregation are unavailable.

| Interface | Implemented calls | Boundaries |
|---|---|---|
| IDirectDraw / IDirectDraw4 | CreateSurface, SetCooperativeLevel, SetDisplayMode, RestoreDisplayMode | Virtual display only; 1–2048 pixels per dimension; 16/32-bit RGB; normal or exclusive-fullscreen cooperative level; refresh 0/60 and zero mode flags |
| IDirectDrawSurface / Surface4 | Blt, BltFast, Flip, GetAttachedSurface, GetCaps, GetPixelFormat, GetSurfaceDesc, IsLost, Lock, Unlock | RGB565 and XRGB8888 only; bounded copies without stretching, color key or blending; one back buffer; valid surfaces are never device-lost |

DDSURFACEDESC is 108 bytes; DDSURFACEDESC2 is 124 bytes. Descriptors and pixel masks are
validated. Surface memory is guest-readable/writable and remains stable through a lock. Pitch
is four-byte aligned. Lock supports a rectangle, WAIT, READONLY or WRITEONLY, with no event handle.
READONLY keeps a snapshot; writes through that lock are discarded on unlock. Surface1 Unlock uses
the returned pixel pointer (or NULL); Surface4 uses a matching rectangle (or NULL). Operations
that touch a locked surface fail with DDERR_SURFACEBUSY.

Blt supports WAIT and COLORFILL (native RGB565/XRGB fill values from DDBLTFX). BltFast accepts WAIT
only. Flip swaps the primary/back pixel contents; locked surfaces cannot flip. A primary surface's
successful Blt, BltFast, Unlock or Flip updates `frame()` with 0xFFRRGGBB pixels and increments
`presents()`. This is a capture/presentation hook, without a live host window or vertical blank timing.

## Direct3D

QueryInterface on DirectDraw obtains IDirect3D3. CreateDevice accepts the public RGB or HAL GUID
and a 3D-capable surface, but both use the CPU rasterizer. IDirect3DDevice3 supports BeginScene,
EndScene, GetDirect3D, GetRenderTarget, SetRenderState, DrawPrimitive, DrawIndexedPrimitive and
SetTexture with stage 0 and a NULL texture.

Drawing requires an active scene, D3DFVF_TLVERTEX (0x1C4, 32-byte pretransformed vertices), finite
screen coordinates, depth in [0,1] and zero draw flags. Triangle lists, strips and fans are supported;
indices are unsigned 16-bit and fully checked before modifying pixels. Colors are opaque Gouraud
diffuse RGB. Culling accepts NONE/CW/CCW, defaulting to CCW in screen coordinates.

ZENABLE=FALSE, solid fill, Gouraud shading and disabled alpha blending/fog/specular are accepted;
other state values fail. The standalone rasterizer has a depth buffer, but guest Direct3D depth
surface semantics are not implemented, so the COM device keeps depth testing disabled. Textures,
transforms, lighting, blending, viewport COM objects and other Direct3D interface versions remain
unavailable. Drawing to a back buffer becomes visible on Flip; drawing to the primary can be
captured on Unlock.

## DirectSound

`DirectSoundCreate(NULL, out, NULL)` supplies IDirectSound. CreateSoundBuffer and SetCooperativeLevel
(normal, priority, exclusive) are implemented. Cooperative level is virtual, with no desktop ownership.

Secondary buffers accept PCM WAVEFORMATEX (tag 1, cbSize 0), mono/stereo, 8/16 bits, 8–192 kHz,
consistent block alignment/rate and a bounded nonempty buffer. Capabilities include software/static,
volume/frequency controls, focus flags and GETCURRENTPOSITION2. Hardware, 3D and notifications fail.
Primary-buffer creation is available, but only SetFormat to the fixed stereo 16-bit 44.1 kHz mixer
format is implemented on that primary object.

Secondary-buffer methods: GetCurrentPosition, GetVolume, GetFrequency, GetStatus, Lock, Play,
SetCurrentPosition, SetVolume, SetFrequency, Stop and Unlock. Lock returns up to two **guest** spans
for a wrapping range. It accepts ENTIREBUFFER; FROMWRITECURSOR is not yet supported. Unlock checks
the returned pointers and lengths before transferring PCM into the mixer. Play supports LOOPING;
the reserved/priority arguments must be zero. Volume uses hundredths of a decibel [-10000,0].
Frequency 0 restores the original rate; explicitly set frequencies range from 100 to 200000 Hz.

The mixer uses nearest-neighbor rate conversion, stereo 44.1 kHz output and saturating signed
16-bit samples. The guest play cursor advances when the embedder calls `mix()`. The write cursor
equals the play cursor because there is no queued hardware latency. Playback, capture, Stop,
position and looping are tested, but this is not a streaming scheduler or a live audio device.

## Reproducible validation and references

`tools/wince/make_directx_fixture.py` generates an original SH-4 PE with DirectDrawCreateEx and
DirectSoundCreate imports. Guest instructions create the objects, call their vtables, draw a triangle,
copy a generated 440 Hz tone through the locked sound buffer and start looping playback.
`tools/wince/test_directx_fixture.py` checks the PPM and every stereo sample at the preferred and
relocated addresses. The C++ suite also tests malformed calls, reference lifetime and unsupported states.

The public references include Microsoft's [DirectDraw surface Lock](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-lock),
[DirectSound buffer Lock](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418073(v=vs.85))
and the installed SDK's legacy interface declarations. DLL APIs follow the CE-specific
[LoadLibrary](https://learn.microsoft.com/en-us/previous-versions/ms911520(v=msdn.10)) and
[GetProcAddress](https://learn.microsoft.com/en-us/previous-versions/ms908453(v=msdn.10)) signatures;
event waiting follows [WaitForSingleObject](https://learn.microsoft.com/en-us/previous-versions/ms915517(v=msdn.10)).
