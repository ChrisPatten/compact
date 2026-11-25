idf_component_register(
    SRCS main.c lv_mem_psram.c power.c overlay.c decision.c earlier.c dayview.c setscreen.c prefs.c display.c fonts.c dial.c now_layout.c settings.c ota.c logbuf.c intercom_ws.c audio_in.c audio_out.c ptt.c thread.c agents.c pill_list.c ble_link.c adpcm.c panicdiag.c capture.c memdiag.c displaywd.c screenshot.c cue.c ring_px.c panel_img.c rim12.c mom_logic.c restart.c reply_logic.c touch_logic.c gap_text.c pomo_logic.c pomodoro.c cmd_logic.c commands.c
    INCLUDE_DIRS .
    LDFRAGMENTS linker.lf
    EMBED_FILES ../fonts/lexend-200.ttf ../fonts/lexend-600.ttf ../fonts/lexend-700.ttf
                ../fonts/hanken-500.ttf ../fonts/hanken-600.ttf)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
endif()
# LVGL calls these (LV_USE_CUSTOM_MALLOC); nothing in main does, so force them into the link.
foreach(sym lv_mem_init lv_mem_deinit lv_mem_add_pool lv_mem_remove_pool lv_malloc_core
            lv_realloc_core lv_free_core lv_mem_monitor_core lv_mem_test_core)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ${sym}")
endforeach()
# fw_version.h: the git version, regenerated on every build (tools/fw_version.cmake), for /info and the boot log.
set(FWV_H ${CMAKE_CURRENT_BINARY_DIR}/fw_version.h)
add_custom_target(fw_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_LIST_DIR}/.. -DOUT=${FWV_H} -P ${CMAKE_CURRENT_LIST_DIR}/../tools/fw_version.cmake
    BYPRODUCTS ${FWV_H} VERBATIM)
add_dependencies(${COMPONENT_LIB} fw_version)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# panicdiag.c records each panic (PC, backtrace, task) in RTC memory for GET /info.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
# Panel ownership: only display.c may talk to the panel (tools/check_panel_owner.sh); a violation stops the configure step.
execute_process(COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/check_panel_owner.sh RESULT_VARIABLE PANEL_OWNER_RC
                OUTPUT_QUIET ERROR_VARIABLE PANEL_OWNER_ERR)
if(NOT PANEL_OWNER_RC EQUAL 0)
    message(FATAL_ERROR "${PANEL_OWNER_ERR}")
endif()
