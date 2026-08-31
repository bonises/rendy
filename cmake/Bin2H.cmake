# Script mode: convert a SPIR-V binary to a C++ header with a uint32_t array.
# Args: -DIN=<file.spv> -DOUT=<file.h> -DNAME=<identifier>

file(READ "${IN}" hex HEX)
# SPIR-V is little-endian 32-bit words: reverse each 4-byte group.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," words "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN} — do not edit.
#pragma once
#include <cstdint>
#include <cstddef>
inline constexpr uint32_t ${NAME}[] = {${words}};
inline constexpr size_t ${NAME}_words = sizeof(${NAME}) / sizeof(uint32_t);
")
