add_unit_test(CRITERION TARGET test_splunk_adapter INCLUDES ${PROJECT_SOURCE_DIR} DEPENDS http-adapters)
add_unit_test(CRITERION TARGET test_splunk_hec_auth INCLUDES ${PROJECT_SOURCE_DIR} DEPENDS http-adapters)
