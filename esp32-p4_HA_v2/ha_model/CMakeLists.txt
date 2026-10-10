# Общий словарь форм данных и идентификаторов (docs/RECORD_MODEL.md §9,
# docs/PROPERTY_MODEL.md). Не линкует Domain и не содержит runtime-логики:
# только формы, immutable-словари и чистые lookup-функции.
idf_component_register(
    SRCS
        "src/ha_properties.c"
        "src/ha_entity.c"
    INCLUDE_DIRS
        "include"
)
