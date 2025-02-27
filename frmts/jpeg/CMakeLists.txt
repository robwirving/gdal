add_gdal_driver(
  TARGET gdal_JPEG
  SOURCES vsidataio.h jpgdataset.h vsidataio.cpp jpgdataset.cpp
  BUILTIN)
gdal_standard_includes(gdal_JPEG)

target_include_directories(gdal_JPEG PRIVATE ${GDAL_RASTER_FORMAT_SOURCE_DIR}/mem)
if (GDAL_USE_JPEG12_INTERNAL)
  target_sources(gdal_JPEG PRIVATE jpgdataset_12.cpp vsidataio_12.cpp)
  target_include_directories(gdal_JPEG PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_definitions(gdal_JPEG PRIVATE JPEG_DUAL_MODE_8_12)
endif ()

if (GDAL_USE_JPEG_INTERNAL)
  target_include_directories(gdal_JPEG PRIVATE $<TARGET_PROPERTY:libjpeg,SOURCE_DIR>)
  if (RENAME_INTERNAL_JPEG_SYMBOLS)
    target_compile_definitions(gdal_JPEG PRIVATE -DRENAME_INTERNAL_LIBJPEG_SYMBOLS)
  endif ()
else ()
  gdal_target_link_libraries(gdal_JPEG PRIVATE JPEG::JPEG)
endif ()

if (GDAL_USE_ZLIB_INTERNAL)
  target_include_directories(gdal_JPEG PRIVATE $<TARGET_PROPERTY:libz,SOURCE_DIR>)
else ()
  gdal_target_link_libraries(gdal_JPEG PRIVATE ZLIB::ZLIB)
endif ()
if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
  # These files has CPL_CVSID("$id$") which break ODR and UNITY_BUILD
  set_property(SOURCE jpgdataset.cpp vsidataio.cpp PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
endif ()
