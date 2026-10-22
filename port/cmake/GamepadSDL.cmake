# Native HID transport for Switch-compatible pads, with Xbox positional buttons.
# SDL is static and limited to the input source's compile interface.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(port_sdl2
    # Official release-2.32.10.
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/5d249570393f7a37e037abf22cd6012a4cc56a71
    URL_HASH SHA256=10f1194f8d2e4a73ca1c7c553b3189d0b68ab9ef3544e8d2a268e4429456b373
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(port_sdl2)
