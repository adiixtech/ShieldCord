<div align="center">
  <img src="installer/shieldcord.ico" width="128" height="128" alt="ShieldCord Logo">
  <h1>ShieldCord</h1>
  <p><strong>A defensive Windows security tool that blocks token-grabbers and infostealer malware from stealing Discord and browser tokens.</strong></p>
  
  <p>
    <a href="https://github.com/adiixtech/ShieldCord/releases/latest"><img src="https://img.shields.io/github/v/release/adiixtech/ShieldCord?style=flat-square" alt="Latest Release"></a>
    <a href="https://github.com/adiixtech/ShieldCord/blob/main/LICENSE"><img src="https://img.shields.io/github/license/adiixtech/ShieldCord?style=flat-square" alt="License"></a>
  </p>
</div>

<hr>

## 🛡️ What is ShieldCord?

**ShieldCord** is a kernel-level security utility designed to protect your PC from infostealers. It acts as a physical barrier between your sensitive token/cookie files (used by Discord and Web Browsers) and malicious software attempting to steal them. 

Rather than relying on signature-based virus scanning (which modern malware easily bypasses), ShieldCord operates as a Windows Minifilter Driver. It sits at the lowest level of the operating system and **completely denies access** to your credentials, even if a token-grabber successfully infects your PC and gains Administrator privileges.

Only processes verified by a trusted Authenticode signature (like genuine Discord or Google Chrome) are allowed to read these files.

## ✨ Features

- **Kernel-Level Blocking**: Stops malware physically at the file system level.
- **Process Memory Monitoring**: Actively terminates malware attempting to inject or read memory from trusted processes (PROCESS_VM_READ).
- **Cryptographic Trust**: Verifies applications based on their real Publisher Signature, not just their filename (preventing malware from bypassing by renaming itself to discord.exe).
- **Decoy Folder Tracking**: Creates honeypot folders to instantly trap and flag infostealers.
- **Zero Privacy Intrusions**: Completely offline. No telemetry, no cloud connectivity, no accounts required.

## 🚀 Quick Start (Using the .exe)

The easiest way to use ShieldCord is by downloading the pre-built, signed installer.

1. **Download** the latest ShieldCord-Setup-x.x.x.exe from the [Releases Tab](../../releases).
2. **Install** the application.
3. Open the **ShieldCord UI** from your Start Menu or System Tray.
4. **Enable Protection**: Click the "Setup Driver" button inside the app. 
   - *Note: ShieldCord uses a test-signed driver. The app will ask you to restart your PC to allow test-signed drivers to load. Secure Boot must be disabled in your BIOS for this to work.*

## 💖 Support the Developer

ShieldCord is completely free and open-source under the GPLv3 license. I don't charge for the software or the protection it provides. 

If this tool saved your Discord account, or if you simply appreciate the hard work that went into it, please consider supporting the project! Your donations help pay for servers, coffee, and future development.

> [!NOTE]
> **Payment methods are currently being set up!** Links to GitHub Sponsors, Patreon, and Ko-fi will be available here soon. Thank you for your patience!

## 🛠️ Building from Source

If you prefer to compile ShieldCord yourself, you can build the entire stack using the provided PowerShell scripts.

### Prerequisites
- Visual Studio 2022 (C++ Desktop Workload)
- Windows Driver Kit (WDK) 10/11
- .NET 10.0 SDK
- Inno Setup 6.3+

### Build Command
Run the following in an elevated PowerShell prompt:
\\\powershell
# Compile the Driver, C++ Engine, C# UI, and build the Installer
.\build_all.ps1
\\\

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPLv3)** - see the [LICENSE](LICENSE) file for details.