# vcredist/

Three Microsoft Visual C++ runtime DLLs, extracted directly from the
official Microsoft installer (`https://aka.ms/vs/17/release/vc_redist.x64.exe`,
Visual C++ 2015-2022 x64 runtime, version 14.44.35211.0):

- `msvcp140.dll`
- `vcruntime140.dll`
- `vcruntime140_1.dll`

## Why these are here

`dll/Transit.dll` (the proprietary vendor DLL - see `dll/`'s own
gitignore comment) is built with MSVC and dynamically links against
these three at load time. A machine without the Visual C++
Redistributable installed fails to load Transit.dll with
`LoadLibraryA` error 126 ("module not found") - a real, confirmed
report, not a hypothetical.

This is Microsoft's own documented "app-local deployment" method for
the VC++ runtime: shipping these exact, unmodified DLLs next to an
application's exe is explicitly permitted by Microsoft's
redistribution terms, as an alternative to requiring a separate
Redistributable install - see
https://learn.microsoft.com/en-us/cpp/windows/deploying-native-desktop-applications-visual-cpp.
`installer.nsi` installs them next to the app's exe (not inside
`dll/`) - see that file's own comment on why the location matters.

## How these were obtained

Downloaded the official bootstrapper above, then extracted with 7-Zip:
the bootstrapper is a self-extracting exe with an embedded cab file at
a fixed offset, which in turn embeds several MSI installer databases as
further nested cabs. The one needed here is the "Visual C++ 2022 X64
Minimum Runtime" payload cab, which directly contains these three DLLs
(named `<name>.dll_amd64` inside that cab - renamed on extraction).
Nothing here has been modified from what that cab contains.

To reproduce/update (e.g. when Microsoft ships a newer runtime):

```
curl -L -o vc_redist.x64.exe https://aka.ms/vs/17/release/vc_redist.x64.exe
7z l vc_redist.x64.exe                     # find the embedded cab's offset (MSCF signature)
dd if=vc_redist.x64.exe of=tail.cab bs=1 skip=<offset>
7z l tail.cab                              # lists a0..aN - find the one MSI naming
                                            # "X64 Minimum Runtime" (metadata only) and
                                            # the cab immediately after it (the payload)
7z x tail.cab -oti <payload-name>
7z x ti/<payload-name> msvcp140.dll_amd64 vcruntime140.dll_amd64 vcruntime140_1.dll_amd64
```
