# stb's pinned resize implementation packs float coefficients through uint64
# pointers that can be only four-byte aligned. Keep its filtering and SIMD paths,
# but make the scalar wide copies alignment- and overlap-safe and keep lookup
# pointers within their table. Upstream:
# https://github.com/nothings/stb/blob/31c1ad37456438565541f4919958214b6e762fb4/stb_image_resize2.h
function(luminumbra_prepare_stb_resize SOURCE_DIR OUTPUT_DIR)
  file(READ "${SOURCE_DIR}/stb_image_resize2.h" _resize_header)
  string(REPLACE "\r\n" "\n" _resize_header "${_resize_header}")
  string(SHA256 _resize_hash "${_resize_header}")
  if(NOT _resize_hash STREQUAL "173e654634f6ccaad98f603e686ea212eec1fe8ea6d2a5e5e8056efa10ae3880")
    message(FATAL_ERROR "The pinned stb resize header changed; review its memory-access patch")
  endif()

  set(_copy_two "#define STBIR_MOVE_2( dest, src ) { STBIR_NO_UNROLL(dest); ((stbir_uint64*)(dest))[0] = ((stbir_uint64*)(src))[0]; }")
  set(_copy_four "#define STBIR_MOVE_4( dest, src ) { STBIR_NO_UNROLL(dest); ((stbir_uint64*)(dest))[0] = ((stbir_uint64*)(src))[0]; ((stbir_uint64*)(dest))[1] = ((stbir_uint64*)(src))[1]; }")
  foreach(_copy IN ITEMS _copy_two _copy_four)
    string(FIND "${_resize_header}" "${${_copy}}" _copy_offset)
    if(_copy_offset EQUAL -1)
      message(FATAL_ERROR "The pinned stb coefficient-copy patch no longer applies")
    endif()
  endforeach()
  string(REPLACE "${_copy_two}"
    "#define STBIR_MOVE_2( dest, src ) { STBIR_NO_UNROLL(dest); memmove((dest), (src), sizeof(stbir_uint64)); }"
    _resize_header "${_resize_header}")
  string(REPLACE "${_copy_four}"
    "#define STBIR_MOVE_4( dest, src ) { STBIR_NO_UNROLL(dest); memmove((dest), (src), 2 * sizeof(stbir_uint64)); }"
    _resize_header "${_resize_header}")

  # The SIMD lookup macros receive exponent-biased lane indices. Subtract that
  # bias from each index instead of forming a pointer before the lookup array.
  set(_table_index [=[table\[(temp[0-3]\.m128i_i32\[[0-3]\])\]]=])
  string(REGEX MATCHALL "${_table_index}" _table_indices "${_resize_header}")
  list(LENGTH _table_indices _table_index_count)
  if(NOT _table_index_count EQUAL 36)
    message(FATAL_ERROR "The pinned stb SIMD table-index patch no longer applies")
  endif()
  string(REGEX REPLACE "${_table_index}" "table[\\1 - (127-13)*8]"
    _resize_header "${_resize_header}")
  string(REPLACE "( fp32_to_srgb8_tab4 - (127-13)*8 )" "fp32_to_srgb8_tab4"
    _resize_header "${_resize_header}")

  # Never patch a fetched source tree, which may be shared by other builds.
  # Avoid touching the generated header when a no-op configure repeats.
  file(MAKE_DIRECTORY "${OUTPUT_DIR}")
  set(_resize_output "${OUTPUT_DIR}/stb_image_resize2.h")
  if(EXISTS "${_resize_output}")
    file(READ "${_resize_output}" _resize_existing)
    if(_resize_existing STREQUAL _resize_header)
      return()
    endif()
  endif()
  file(WRITE "${_resize_output}" "${_resize_header}")
endfunction()
