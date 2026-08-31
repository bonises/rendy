# Interface target carrying rendy's warning + sanitizer flags.
# Dependencies are pulled in with SYSTEM includes, so these flags only hit our code.

add_library(rendy_warnings INTERFACE)
add_library(rendy::warnings ALIAS rendy_warnings)

target_compile_options(rendy_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
)

if(RENDY_WERROR)
  target_compile_options(rendy_warnings INTERFACE -Werror)
endif()

if(RENDY_SANITIZE)
  string(REPLACE ";" "," _rendy_san "${RENDY_SANITIZE}")
  target_compile_options(rendy_warnings INTERFACE -fsanitize=${_rendy_san} -fno-omit-frame-pointer)
  target_link_options(rendy_warnings INTERFACE -fsanitize=${_rendy_san})
endif()
