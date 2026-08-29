# Turns a .js file into a C byte array so it can be JS_Eval'd as a startup
# bootstrap without adding a runtime dependency on locating the source file
# next to the installed binary. Invoked as:
#   cmake -DIN=<src.js> -DOUT=<generated.h> -DVAR=<c identifier> -P embed_js.cmake
file(READ "${IN}" hex_content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex_content}")
file(WRITE "${OUT}" "/* generated from ${IN}; do not edit */\nstatic const char ${VAR}[] = {${bytes}0x00};\n")
