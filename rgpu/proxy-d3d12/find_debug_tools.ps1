$names = @('cdb.exe','windbg.exe','windbgx.exe','symchk.exe','dumpbin.exe','llvm-pdbutil.exe','llvm-symbolizer.exe')
foreach ($name in $names) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) { [pscustomobject]@{ Name=$name; Path=$cmd.Source } }
}
Get-ChildItem 'C:\Program Files*\Windows Kits\10\Debuggers\x64\*.exe' -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -in $names } |
  ForEach-Object { [pscustomobject]@{ Name=$_.Name; Path=$_.FullName } }
