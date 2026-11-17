# Applies ADSB_SANITIZE globally so that the library, tests, and third-party
# test frameworks are all instrumented consistently (mixing instrumented and
# uninstrumented code produces false positives, especially under TSan).

if(ADSB_SANITIZE)
  set(_adsb_san_list ${ADSB_SANITIZE})
  if("thread" IN_LIST _adsb_san_list AND "address" IN_LIST _adsb_san_list)
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
  endif()

  list(JOIN _adsb_san_list "," _adsb_san_csv)
  set(_adsb_san_flags -fsanitize=${_adsb_san_csv} -fno-omit-frame-pointer -g)
  if("undefined" IN_LIST _adsb_san_list)
    # Make UBSan findings fatal so CI fails instead of just printing.
    list(APPEND _adsb_san_flags -fno-sanitize-recover=undefined)
  endif()

  add_compile_options(${_adsb_san_flags})
  add_link_options(-fsanitize=${_adsb_san_csv})
  message(STATUS "adsb: sanitizers enabled: ${_adsb_san_csv}")
endif()
