add_gdal_driver(
  TARGET ogr_AmigoCloud
  SOURCES ogr_amigocloud.h
          ogramigoclouddatasource.cpp
          ogramigoclouddriver.cpp
          ogramigocloudlayer.cpp
          ogramigocloudresultlayer.cpp
          ogramigocloudtablelayer.cpp
          PLUGIN_CAPABLE_IF
          "NOT GDAL_USE_JSONC_INTERNAL"
          NO_DEPS)
gdal_standard_includes(ogr_AmigoCloud)
target_include_directories(ogr_AmigoCloud PRIVATE ${GDAL_VECTOR_FORMAT_SOURCE_DIR}/pgdump
                                                  ${GDAL_VECTOR_FORMAT_SOURCE_DIR}/geojson)
if (GDAL_USE_JSONC_INTERNAL)
  target_include_directories(ogr_AmigoCloud PRIVATE $<TARGET_PROPERTY:libjson,SOURCE_DIR>)
else ()
  gdal_target_link_libraries(ogr_AmigoCloud PRIVATE ${JSONC_LIBRARIES})
endif ()
