#!/bin/bash
mkdir -p ../build
pushd ../build
CommonFlags="-DDEBUG -g -Og -Wall -Werror -Wno-write-strings -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare -Wno-unused-result -Wno-strict-aliasing -Wno-switch -std=gnu++11 -fno-rtti -fno-exceptions -DHANDMADE_INTERNAL=1 -DHANDMADE_SLOW=1 -DHANDMADE_SDL=1"

# Build asset file builder
c++ $CommonFlags ../code/test_asset_builder.cpp -o test_asset_builder

# Build a 64-bit version
c++ $CommonFlags -O2 ../code/handmade_optimized.cpp -c -fPIC -o handmade_optimized.o
c++ $CommonFlags ../code/handmade.cpp handmade_optimized.o -fPIC -shared -o handmade.so.temp
mv handmade.so.temp handmade.so
c++ $CommonFlags ../code/sdl_handmade.cpp -o handmadehero.x86_64 -ldl `../code/sdl2-64/bin/sdl2-config --cflags --libs` -Wl,-rpath,'$ORIGIN/x86_64'

# Build a 32-bit version
#c++ -m32 $CommonFlags ../code/handmade.cpp -fPIC -shared -o handmade.so
#c++ -m32 $CommonFlags ../code/sdl_handmade.cpp -o handmadehero.x86 -ldl `../code/sdl2-32/bin/sdl2-config --cflags --libs` -Wl,-rpath,'$ORIGIN/x86'

#Copy SDL into the right directory.
if [ ! -d "x86_64" ]
then
  mkdir -p x86_64
  cp ../code/sdl2-64/lib/libSDL2-2.0.so.0 x86_64/
fi
if [ ! -d "x86" ] 
then
  mkdir -p x86
  cp ../code/sdl2-32/lib/libSDL2-2.0.so.0 x86/
fi
popd

