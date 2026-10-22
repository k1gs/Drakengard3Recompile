# A pinned SDL supplies native HID support for DualSense, Switch Pro / paired
# Joy-Cons (including compatible Switch-mode pads), plus Xbox through XInput.
# Static linking keeps the executable self-contained; bootstrap/offline builds
# can set FETCHCONTENT_SOURCE_DIR_PORT_SDL2 to a local SDL checkout.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(port_sdl2
    # release-2.32.8, immutable source revision and verified archive digest.
    URL https://github.com/libsdl-org/SDL/archive/98d1f3a45aae568ccd6ed5fec179330f47d4d356.tar.gz
    URL_HASH SHA256=be55f7015a5599faa869b20078de83df7e83c35585ee1c3ee203a0e58eefebbd
)
FetchContent_MakeAvailable(port_sdl2)
