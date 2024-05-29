@echo off
del /f /q *.VC.db
del /f /q *.VC.opendb
del /f /q Debug\*.obj
del /f /q Debug\*.ilk
del /f /q Debug\*.pdb
del /f /q Debug\_log.*
del /f /q Release\*.obj
del /f /q Release\*.ilk
del /f /q Release\*.pdb
del /f /q Release\*.iobj
del /f /q Release\*.ipdb
del /f /q Release\_log.*
rmdir /s /q .vs
for %%f in (.\*.sln) do (
    rmdir /s /q %%~nf\Debug
    rmdir /s /q %%~nf\Release
    rmdir /s /q %%~nf\x64
    del /f /q %%~nf\_log.*
)