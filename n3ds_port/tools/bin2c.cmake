# cmake -DIN=<file> -DOUT=<file.c> -P bin2c.cmake : embeds a binary as passthrough_shbin[] (pure CMake, works in MSYS).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR size "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "const unsigned int passthrough_shbin_size = ${size};\nconst unsigned char passthrough_shbin[] __attribute__((aligned(4))) = {${arr}};\n")
