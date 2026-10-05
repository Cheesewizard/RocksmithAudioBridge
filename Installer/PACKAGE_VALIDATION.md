# Installer package input validation

Run from the repository root in PowerShell:

```powershell
& .\Installer\ValidatePackageResources.ps1
```

This read-only check verifies that the two native installer resources point at
the authoritative Master/Release outputs, that those files exist and
are x86 PE images, and that any retained legacy root-level copies are not used
as resource inputs.
