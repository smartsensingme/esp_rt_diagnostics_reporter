idf_component_register(SRCS "esp_rt_diagnostics_reporter.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_rt_diagnostics esp_timer freertos log heap)
