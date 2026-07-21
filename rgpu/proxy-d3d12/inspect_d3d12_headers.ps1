$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
& $objdump -p 'C:\Windows\System32\d3d12.dll' | Select-Object -First 60
