# Contributing to ShieldCord

First off, thank you for considering contributing to ShieldCord! It's people like you that make open-source tools great.

## Development Environment
To build ShieldCord from source, you will need:
- **Visual Studio 2022** with the "Desktop development with C++" workload.
- **Windows Driver Kit (WDK)** and Windows SDK (10 or 11).
- **.NET 10.0 SDK** (for the UI).
- **Inno Setup 6.3+** (to compile the installer).

All compilation is automated via the included uild_all.ps1 PowerShell script.

## Submitting Pull Requests
1. **Fork the repository** and create your branch from main.
2. **Ensure your code compiles** with uild_all.ps1 with 0 warnings.
3. **Follow the existing code style**.
4. **Test your changes** inside a Virtual Machine (DO NOT test the kernel driver on your host machine!).
5. Submit a detailed Pull Request explaining *what* you changed and *why*.

## Reporting Bugs
Before creating an issue, please check if one already exists. If you find a bug, open an issue using the Bug Report template and include as much information as possible (Windows version, error logs, etc.).

By contributing, you agree that your contributions will be licensed under the project's GPLv3 License.