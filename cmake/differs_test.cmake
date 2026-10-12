# Two screenshots must both exist and differ (e.g. Play mode moved something).
# Usage: cmake -DA=<png> -DB=<png> -P differs_test.cmake
foreach(png "${A}" "${B}")
  if(NOT EXISTS "${png}")
    message(FATAL_ERROR "Missing screenshot: ${png}")
  endif()
endforeach()
file(SHA256 "${A}" hash_a)
file(SHA256 "${B}" hash_b)
if(hash_a STREQUAL hash_b)
  message(FATAL_ERROR "Screenshots are identical: ${A} and ${B}")
endif()
message(STATUS "OK: ${A} and ${B} differ")
