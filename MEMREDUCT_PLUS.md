# Mem Reduct Plus — development notes

This is a modified build of [Mem Reduct](https://github.com/henrypp/memreduct),
licensed under GNU GPL v3. The startup application manager was added in 2026.
The original copyright and license notices are preserved.

## Phase 1 feature

The **Startup application optimization** menu adds a conservative manager for
applications that run at sign-in.

It currently scans:

- `HKCU` and `HKLM` `...\CurrentVersion\Run`
- 64-bit and 32-bit registry views
- the current user's Startup folder
- the common Startup folder

Each item is classified as:

- **Keep (protected):** Windows security, hardware, driver, input, audio, and
  other system-related entries. The manager refuses to disable these items.
- **Review:** unknown or preference-dependent applications. They can only be
  disabled after the user explicitly checks them.
- **Disable recommended:** a small allowlist of optional launchers and chat,
  media, meeting, and updater applications.

Before changing an item, its exact previous StartupApproved value is copied to
`HKCU\Software\MemReductPlus\StartupBackups`. **Restore all changes** writes the
original value back, or removes the value if it did not exist before.

## Safety boundaries

This phase deliberately does not modify Windows services, scheduled tasks,
Defender, Windows Update, drivers, page-file settings, personal files, or
installed applications. Disabling a startup application affects the next
sign-in; it does not close the currently running process.

Windows does not provide a documented general-purpose API for changing every
classic Win32 startup application's Task Manager state. This implementation
uses the StartupApproved state used by supported Windows 10/11 environments,
and keeps an exact restore record before every change. It requires real-device
validation before public distribution.

## Build layout

The upstream solution expects Henry++'s `routine` repository beside this
repository:

```text
parent/
├── memreduct-plus/
└── routine/
```

Open `memreduct.sln` in the Visual Studio version/toolset declared by the
project, then build `Release | x64`. Administrator elevation is required for
system-wide startup entries and the existing memory cleaning features.

