set(include_dirs  
                  ./
                  ./Fonts)

idf_component_register(
  SRCS
  "GUI_BMPfile.c"
  "GUI_Paint.c"
  "Fonts/font14CN.c"
  "Fonts/font18CN.c"
  "Fonts/font22CN.c"
  "Fonts/font24.c"
  "Fonts/font24CN.c"
  PRIV_REQUIRES driver fatfs sdmmc sdcard_bsp
  INCLUDE_DIRS 
  ${include_dirs})
