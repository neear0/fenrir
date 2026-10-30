/*
 * The FreeType modules CelosUI compiles, in place of FreeType's own ftmodule.h which registers every module.
 * This directory comes before FreeType's include/ on the include path, so <freetype/config/ftmodule.h> finds this file.
 * The sources compiled for each module are listed in CMakeLists.txt and cUI.vcxproj: TrueType and CFF/OpenType fonts,
 * the auto hinter and the anti-aliasing rasterizer.
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
