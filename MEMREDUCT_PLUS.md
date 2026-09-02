# Mem Reduct Plus — PC Optimizer

Mem Reduct Plus extends the GPLv3 Mem Reduct codebase with conservative Windows PC optimization tools. The original copyright and license notices are preserved.

## Final Windows x64 package

The build artifact contains two executables:

- `MemReductPlus.exe` — the new Korean PC optimization dashboard.
- `memreduct.exe` — the original Mem Reduct application with the integrated startup application manager.

## PC optimization dashboard

`MemReductPlus.exe` provides:

- PC health dashboard and score using memory load, disk free space, process count, and startup count.
- Immediate memory cleanup by trimming accessible process working sets.
- Automatic memory cleanup every 15 minutes while the dashboard is running.
- Startup application analysis and conservative recommended disabling with backup before changes.
- High-memory process analysis showing the largest working sets without automatically terminating processes.
- Safe disk cleanup limited to files older than 24 hours in the user TEMP and Windows TEMP folders. Personal documents, Downloads, and Recycle Bin are not targeted.
- Performance mode that applies the Windows high-performance power scheme and reduces UI effects after saving the previous optimizer-managed settings.
- Before/after comparison for memory usage, available memory, disk free space, process count, and startup count.
- Change logging at `%LOCALAPPDATA%\MemReductPlus\changes.log`.
- `Restore all` for optimizer-managed startup and performance settings. Memory trimming and deleted temporary files are not reversible operations.

## Integrated startup manager

The startup manager in `memreduct.exe` scans:

- `HKCU` and `HKLM` `...\CurrentVersion\Run`
- 64-bit and 32-bit registry views
- the current user's Startup folder
- the common Startup folder

Each item is classified as protected, review, or disable-recommended. Protected Windows security, hardware, driver, input, audio, and related system entries are not disabled. Exact previous StartupApproved values are backed up before changes and can be restored.

## Safety boundaries

The optimizer deliberately does not disable Windows services, Windows Defender, Windows Update, device drivers, BitLocker, page-file settings, or installed applications. It does not delete user documents. High-usage processes are reported rather than force-terminated.

StartupApproved is an implementation mechanism used by supported Windows 10/11 environments rather than a documented universal startup-control API. Startup and performance changes should therefore be tested on the target PC before broad distribution.

## Build and verification

GitHub Actions builds on a Windows runner:

1. checkout this repository and the pinned Henry++ `routine` dependency;
2. build original Mem Reduct as `Release | x64`;
3. compile the standalone PC optimizer dashboard for x64 with the Visual C++ toolchain discovered through `vswhere`;
4. verify both executables exist and sanity-check the dashboard binary;
5. calculate SHA-256 and upload the final package as `memreduct-plus-final-x64`.

Administrator elevation can improve access to system-wide processes and settings. Some operations can be partially unavailable when Windows denies access; the program skips inaccessible processes rather than forcing access.
