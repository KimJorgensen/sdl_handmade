# SDL Handmade

This is an implementation of a SDL platform layer for [Handmade Hero](http://handmadehero.org/), based on David Gow's excellent SDL port [Handmade Penguin](http://davidgow.net/handmadepenguin/).

The following notable changes has been made to Handmade Penguin:

* Updated to work with Casey's latest source code (at least for now)
* Added looped live code editing support
* Uses SDL_QueueAudio instead of a ring buffer
* Restructured `sdl_handmade` to make it easy to compare to `win32_handmade`

# Build process

You will need the Handmade Hero source code for the platform-independent game code and the game assets. Pre-order the game to get these.

Copy the Handmade Hero source files into `sdl_handmade/code/` and copy the Handmade Hero test assets into `sdl_handmade/ref/assets/`.

Compile it with `./build.sh` and run it with `./handmade.sh`.

# Things to do

* Clean-up audio code
* Correct mouse X/Y input according to image offset and stretching
* Test controller input handling
* Support 32 bit build

# Licensing/Copyright/Author

Casey Muratori is the author of Handmade Hero, and this code is built by observation of his Win32 layer. That isn’t freely distributable.

The sdl platform layer was written by David Gow and modified by Kim Jørgensen, and is freely distributable.
