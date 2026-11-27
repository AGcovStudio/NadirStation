@echo off
rem GOES SD Level-2 (approximate) from a stitched full disc.
rem Usage: run_goes_sd_l2.bat [stitched folder]
setlocal
set "ROOT=%~dp0.."
set "SRC=%~1"
if "%SRC%"=="" set "SRC=%ROOT%\build\FY2_DATA\GOES SD\full\stitched_v3"
python "%~dp0sd_l2.py" "%SRC%" --borders "%ROOT%\build\resources\maps\ne_10m_admin_0_countries_chn.geojson"
if errorlevel 1 echo Failed. Needs Python 3 with numpy and pillow:  pip install numpy pillow
pause
