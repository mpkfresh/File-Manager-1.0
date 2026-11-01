# Called by each Windows POST_BUILD rule. All executables share one output
# directory, so parallel windeployqt copies must take the same lock.
foreach(required IN ITEMS DEPLOY_TOOL DEPLOY_OPTIONS DEPLOY_LOCK DEPLOY_TARGET)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing Qt deployment input: ${required}")
    endif()
endforeach()
if(NOT EXISTS "${DEPLOY_TOOL}" OR NOT EXISTS "${DEPLOY_TARGET}")
    message(FATAL_ERROR "Qt deployment tool or target is missing")
endif()

file(LOCK "${DEPLOY_LOCK}" GUARD PROCESS TIMEOUT 600 RESULT_VARIABLE lock_result)
if(NOT "${lock_result}" STREQUAL "0")
    message(FATAL_ERROR "Could not serialize Qt deployment: ${lock_result}")
endif()

string(REPLACE "+" ";" deploy_options "${DEPLOY_OPTIONS}")
execute_process(
    COMMAND "${DEPLOY_TOOL}" ${deploy_options} "${DEPLOY_TARGET}"
    RESULT_VARIABLE deploy_result)
if(NOT "${deploy_result}" STREQUAL "0")
    get_filename_component(target_name "${DEPLOY_TARGET}" NAME)
    message(FATAL_ERROR "Qt deployment failed for ${target_name}: ${deploy_result}")
endif()
