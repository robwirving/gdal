add_gdal_driver(TARGET gdal_RIK SOURCES rikdataset.cpp PLUGIN_CAPABLE_IF "NOT GDAL_USE_ZLIB_INTERNAL" NO_DEPS)
gdal_standard_includes(gdal_RIK)

if (GDAL_USE_ZLIB_INTERNAL)
  target_include_directories(gdal_RIK PRIVATE $<TARGET_PROPERTY:libz,SOURCE_DIR>)
else ()
  gdal_target_link_libraries(gdal_RIK PRIVATE ZLIB::ZLIB)
endif ()
