# Мост «ZCL ↔ семантика» (docs/PROPERTY_MODEL.md, фаза 1). Знает пары (cluster, attr)
# и их кодировку; предоставляет потребителям семантику. Линкует ha_model (словарь),
# Domain не линкует.
idf_component_register(
    SRCS
        "src/semantics.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ha_model
)
