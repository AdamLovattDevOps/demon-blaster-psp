@echo off
echo Building Demon Blaster for PSP...
docker run --rm -v %cd%:/build pspdev/pspdev sh -c "cd /build && make clean && make"
if %errorlevel% equ 0 (
    echo.
    echo Build successful! EBOOT.PBP is ready.
    echo Copy to: PSP/GAME/DemonBlaster/EBOOT.PBP
) else (
    echo.
    echo Build failed!
)
pause
