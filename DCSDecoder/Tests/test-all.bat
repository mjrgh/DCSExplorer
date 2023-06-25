@echo off

pushd %~dp0

if not exist roms (
    echo Before running this test, you must create a subdirectory called ROMS, and
    echo download the PinMame ROM images mentioned in the test.  The ROM images are
    echo packaged as .zip files.  You can download them from sites, such as
    echo vpforums.org.
    popd
    exit /b
)

set progexe=..\..\Release\DCSExplorer

if not exist %progexe%.exe (
    echo This test runs against the x86 Release build %progexe%.exe
    echo Please run the Visual Studio build, selecting configuration x86 Release
    echo before building.
    popd
    exit /b
)

if not exist results mkdir results
del /q results\*.log results\*.diff results\*.success

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
  echo *** Starting test: %%i
  %progexe% --vol=220 --autoplay --silent --terse --validate=results\%%i.log roms\%%i
  find "Validation Succeeded" results\%%i.log > nul
  if errorlevel 1 (echo Failed > results\%%i.diff) else (echo Success > results\%%i.success)
  echo.
)

if exist results\*.diff (
    echo *** VALIDATION FAILURES DETECTED ***
    dir results\*.diff
) else (
    echo *** SUCCESS ***
)
echo See RESULTS subdirectory for validation log files

popd
