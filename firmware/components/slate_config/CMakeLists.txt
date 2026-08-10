idf_component_register(
    SRCS "slate_config.c"
    INCLUDE_DIRS "include"
    REQUIRES json slate_state slate_store slate_theme
)
