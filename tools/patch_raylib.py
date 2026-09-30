#!/usr/bin/env python3
"""Small, idempotent patches to the pinned raylib 5.5 for the OpenGL ES 3.0 / Android build.

  rlgl.h           ES 3.0 has glDrawBuffers in core; raylib calls the ES 2.0 extension name
                   glDrawBuffersEXT, which libGLESv3 does not export (link error).
  rcore_android.c  asks EGL for a 24-bit depth buffer (16 bits band at a distance) and for an
                   ES 3.0 context when raylib is built for ES 3.0 (it always asked for 2);
                   InitWindow(0, h) renders h lines with the width from the panel's shape,
                   which the system then scales up to the panel (a phone's GPU need not fill
                   2400x1080 every frame).

Run by tools/fetch_deps.sh; safe to run again.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RL = ROOT / "third_party" / "raylib" / "src"

PATCHES = [
    (RL / "rlgl.h",
     "#if defined(GRAPHICS_API_OPENGL_ES3)\n            glDrawBuffersEXT(count, buffers);",
     "#if defined(GRAPHICS_API_OPENGL_ES3)\n            glDrawBuffers(count, buffers);  // (REAL JAPAN patch: core in ES 3.0)"),
    (RL / "platforms" / "rcore_android.c",
     "        EGL_DEPTH_SIZE, 16,         // Depth buffer size (Required to use Depth testing!)",
     "        EGL_DEPTH_SIZE, 24,         // Depth buffer size (REAL JAPAN patch: 16 bits band at a distance)"),
    (RL / "platforms" / "rcore_android.c",
     "        EGL_CONTEXT_CLIENT_VERSION, 2,",
     "        EGL_CONTEXT_CLIENT_VERSION, (rlGetVersion() == RL_OPENGL_ES_30)? 3 : 2,  // (REAL JAPAN patch)"),
    (RL / "platforms" / "rcore_android.c",
     """                    CORE.Window.display.height = ANativeWindow_getHeight(platform.app->window);
""",
     """                    CORE.Window.display.height = ANativeWindow_getHeight(platform.app->window);

                    // (REAL JAPAN patch) InitWindow(0, h): h lines, the width from the panel's shape
                    if ((CORE.Window.screen.width == 0) && (CORE.Window.screen.height > 0) && (CORE.Window.display.height > 0))
                    {
                        if (CORE.Window.screen.height >= CORE.Window.display.height) CORE.Window.screen.height = 0;
                        else CORE.Window.screen.width = (int)((float)CORE.Window.display.width*CORE.Window.screen.height/CORE.Window.display.height + 0.5f);
                    }
"""),
]


def main() -> int:
    ok = True
    for path, old, new in PATCHES:
        if not path.exists():
            print(f"patch_raylib: {path} missing", file=sys.stderr)
            ok = False
            continue
        s = path.read_text(encoding="utf-8")
        if new in s:
            continue
        if s.count(old) != 1:
            print(f"patch_raylib: pattern not found once in {path.name}", file=sys.stderr)
            ok = False
            continue
        path.write_text(s.replace(old, new), encoding="utf-8")
        print(f"patch_raylib: patched {path.name}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
