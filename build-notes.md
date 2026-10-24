# Build Notes

## Decision: Drop Visual Studio project files

Per operator decision on t_039bd21b: drop `.sln`, `.vcxproj`, `.vcxproj.filters`,
and `build-windows.bat`. The CI pipeline uses CMake on all three targets (Linux,
macOS, Windows) and nothing else in the repo references those files. Removing them
eliminates a second build system that would diverge from CMake over time.

Developers and CI should use CMake exclusively:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Unity version

The plugin targets Unity 2023 LTS and later. Support for Unity 2022.x has been
dropped.

## Target platforms

- macOS (CoreBluetooth)
- Windows (WinRT)
- Linux (BlueZ/D-Bus)
