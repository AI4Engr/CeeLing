if(NOT DEFINED FILE)
    message(FATAL_ERROR "FILE is required")
endif()

file(READ "${FILE}" CONTENTS)

foreach(VAR NEEDLE1 NEEDLE2 NEEDLE3 NEEDLE4)
    if(DEFINED ${VAR} AND NOT "${${VAR}}" STREQUAL "")
        string(FIND "${CONTENTS}" "${${VAR}}" POSITION)
        if(POSITION EQUAL -1)
            message(FATAL_ERROR "Missing expected text '${${VAR}}' in ${FILE}")
        endif()
    endif()
endforeach()
