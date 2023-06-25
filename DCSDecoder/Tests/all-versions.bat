@echo off

pushd %~dp0

if not exist roms (
    echo Before running this script, you must create a subdirectory called ROMS, and
    echo download the PinMame ROM images mentioned in the test.  The ROM images are
    echo packaged as .zip files.  You can download them from sites, such as
    echo vpforums.org.
    popd
    exit /b
)

set progexe=..\..\Release\DCSExplorer

if not exist %progexe%.exe (
    echo This script runs against the x86 Release build %progexe%.exe
    echo Please run the Visual Studio build, selecting configuration x86 Release
    echo before building.
    popd
    exit /b
)

for %%i in (
   ij_l7
   jd_l7
   sttng_l7
   corv_21
   dm_h6
   pop_lx5
   rs_l6
   fs_lx5
   ts_lx5
   wcs_l2
   afm_113b
   congo_21
   dh_lx2
   i500_11r
   jb_10r
   jm_12r
   nf_11x
   tom_13
   wd_12
   sc_18
   ss_15
   totan_14
   cv_14
   mm_10
   nbaf_31
   ngg_13
   cc_13k
   mb_10
   cp_16
) do (
  echo *** Detecting version for %%i
  %progexe% --info roms\%%i | find "Version:"
  echo.
)

popd
