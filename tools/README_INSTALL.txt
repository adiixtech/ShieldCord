ShieldCord - VirtualBox Installation Guide
==========================================

WHAT IS IN THIS FOLDER
----------------------
  setup\           ShieldCord-Setup-<version>.exe   <- install with THIS
  driver\          shieldcord_filter.sys, .inf, .cat, ShieldCordCert.cer
  service\         standalone engine binaries, and config.json
  service\driver\  the same driver payload, where shieldcord_driver_setup.exe
                   looks for it (it reads .\driver\ next to itself)
  ui\              the tray app, if you want to run it without installing
  test_tools\      grabber_test.c - a simulated token grabber, for testing
  scripts\         legacy .bat helpers. Superseded by the app; you do not need
                   them. Kept only for dev layouts.

The only file you need to install is Setup.exe. The rest is here so you can
run, test or repair the pieces individually.


WHY SETUP.EXE DOES NOT INSTALL THE DRIVER
-----------------------------------------
The kernel driver is signed with ShieldCord's own test certificate. Windows
will load a driver signed that way only while test signing is on, and test
signing is a boot setting that changes how the whole PC validates drivers.
That is not something an installer may switch on quietly.

So Setup.exe installs the service and the tray app. The APP then asks about
the driver, explains what test signing costs, installs it once you agree, and
tells you if it could not. That is why setup finishes with protection still
off - by design, not by failure.


==============================================================
STEP 1 - ONE-TIME VM SETUP   (do this once, then snapshot)
==============================================================

1a. Turn OFF Secure Boot, if this VM has it.
    VirtualBox: Settings -> System -> Motherboard -> uncheck "Enable EFI"

    `bcdedit /set testsigning on` is REFUSED while Secure Boot is enabled, and
    there is no way around that from inside Windows. On an EFI VM with Secure
    Boot on, this step is not optional.

1b. Turn OFF Memory Integrity (HVCI).
    Windows Security -> Device Security -> Core Isolation -> Memory Integrity -> OFF
    Then reboot.

    HVCI refuses a test-signed driver even in Test Mode, so leaving it on means
    the driver installs cleanly and then fails to load.

1c. Enable test signing. In an ADMIN Command Prompt:
      bcdedit /set testsigning on
    Reboot. Windows will paint a "Test Mode" watermark in the bottom-right of
    the desktop. That is expected - it is Windows saying driver signature
    enforcement is off, which is the trade test signing makes.

    ShieldCord hides that watermark (see 4b). Hiding it does NOT turn test
    signing off; the setting is unchanged and the app says so on its Settings
    page. It comes back if you switch the option off and restart the shell.

    DO NOT run `bcdedit /set nointegritychecks on`. It is not needed - turning
    Memory Integrity off in 1b is the actual requirement - and it disables
    integrity checking for the entire machine, which is a far bigger hole than
    this product should ever ask anyone to open.

1d. Take a VirtualBox snapshot and name it "Ready for driver testing".
    If the driver ever misbehaves, restore this instead of debugging it.


==============================================================
STEP 2 - INSTALL
==============================================================

Run as Administrator, from the shared folder:

    Z:\ShieldCord-VM\setup\ShieldCord-Setup-<version>.exe

It shows two tick boxes, both ticked, and nothing else to decide:

    [x] Kernel driver      puts the signed driver in {app}\driver for the app
                           to install in Step 3
    [x] Tray application   the tray app, plus the bundled .NET runtime if one
                           was bundled into this Setup.exe

Leave "Open ShieldCord to finish setup" ticked on the last page.

When setup finishes it tells you plainly that protection is NOT on yet. That
message is correct, not an error: the driver step is next.

Default install folder: C:\Program Files\ShieldCord


==============================================================
STEP 3 - FINISH SETUP IN THE APP
==============================================================

The app opens on the dashboard and offers "Set up protection". Click it.

  - it asks for Administrator approval (UAC)
  - it installs the kernel driver
  - it asks you to restart

Restart the VM.

After the restart the dashboard banner should read "Tokens protected", and the
dot row in that banner should show Driver / Memory monitor / Decoy all green.

If the driver could not be installed, the app says which of these it was:
Secure Boot still enabled, Memory Integrity still enabled, or test signing not
yet in effect. It will not claim protection it does not have.


==============================================================
STEP 4 - VERIFY
==============================================================

Admin Command Prompt:

    fltMC filters
      -> ShieldCordFilter   385200

    sc query ShieldCordFilter
      -> STATE : 4  RUNNING

    sc query ShieldCordSvc
      -> STATE : 4  RUNNING

The engine's own log is at:

    C:\ProgramData\ShieldCord\logs\shieldcord.log


==============================================================
STEP 4b - THE "TEST MODE" WATERMARK
==============================================================

With test signing on, Windows paints "Test Mode" in the bottom-right of the
desktop. ShieldCord hides it. Settings -> "This app" ->
"Hide the Test Mode watermark on the desktop", on by default.

How it works, and what it costs:

  - The watermark is drawn by shell32.dll INSIDE explorer.exe. It is not a
    kernel thing, and ShieldCord's driver is not involved.
  - shieldcord_watermark.dll is loaded into explorer.exe by the tray app and
    redirects the one call that fetches the watermark's text. It writes
    nothing to disk and adds no registry key.
  - The tray can do this WITHOUT elevation because it already runs in the same
    session, as the same user, at the same integrity level as explorer.
  - Nothing is permanent. The hook lives in explorer's memory and dies with
    explorer; the DLL pins itself so it cannot be unloaded early, which means
    the file stays locked until the shell restarts.

IMPORTANT - THIS WILL LOOK LIKE MALWARE TO ANTIVIRUS. Writing code into
explorer.exe is the same technique the token grabbers this product exists to
block. Windows Defender may flag, block, or quarantine shieldcord_watermark.dll
- in which case the watermark simply stays. If that happens, add an exclusion
for the ShieldCord folder KNOWINGLY: it is a real reduction in your own
defences, and the alternative is living with the watermark.

Verifying it worked:

  1. The watermark should be gone seconds after sign-in.
  2. Restart the shell (Task Manager -> Windows Explorer -> Restart) and
     confirm it is hidden again a few seconds later. This is the case that
     matters: a new explorer is a clean process and the hook has to go back in.
  3. Settings shows what actually happened. "hidden (could not confirm -
     desktop was covered)" is honest, not a failure: the app checks the pixels
     and will not claim success it could not see. It retries with the desktop
     visible.
  4. Switch the option off. The watermark comes back immediately, and stays
     back across a shell restart.
  5. Confirm the shell is unharmed: desktop icons, taskbar, Start menu, File
     Explorer, and right-click menus must all still draw their text.

If the shell restarts twice in a row shortly after the hook is applied,
ShieldCord stops and says so rather than re-injecting. That guard exists
because the one failure this must never have is a machine that repeatedly
kills its own desktop. Turning the setting off and on again clears it.

NOT removed, deliberately: the "Activate Windows" watermark (a different
mechanism, and the tools that do this cannot remove it either), and the
"Evaluation copy." / "For testing purposes only." strings.


==============================================================
STEP 5 - TEST THAT IT ACTUALLY BLOCKS
==============================================================

Setup.exe does not include a compiler. To build the test tool you need the
Visual C++ Build Tools in the VM:

    https://aka.ms/vs/17/release/vs_BuildTools.exe
    (select the "C++ build tools" workload)

Then, in a Developer Command Prompt:

    mkdir C:\ShieldCordTest
    cd C:\ShieldCordTest
    copy Z:\ShieldCord-VM\test_tools\grabber_test.c .
    cl grabber_test.c /Fe:grabber_test.exe

Test 1 - Discord closed:
    grabber_test.exe
    Expected: [+] BLOCKED (ACCESS DENIED) - Driver is working!

Test 2 - Discord open and logged in:
    (launch Discord in the VM and let it finish loading)
    grabber_test.exe
    Expected: still BLOCKED. The grabber's PID is not trusted, so it is denied
    even while Discord itself is running.

Test 3 - Service stopped:
    sc stop ShieldCordSvc
    grabber_test.exe
    Expected: OPENED. This is the fail-open design working as intended, not a
    bug and not a failure of the install.

    The driver drops back to allowing every open the moment its service
    disconnects (ScDisconnectNotify in comm_port.c). That is deliberate: it
    stops Discord from being locked out of its own token files if the engine
    crashes or is stopped by hand. The consequence is that protection depends
    on the SERVICE running, not merely on the driver being loaded - so a
    stopped service means an unprotected machine even though `fltMC filters`
    still lists ShieldCordFilter.

    Start it again and re-run the test to watch it come back:
    sc start ShieldCordSvc
    grabber_test.exe
    Expected: BLOCKED again.

A "[?] File not found" result means that app is not installed in the VM - it is
not a pass or a fail.


==============================================================
STEP 6 - STRESS TEST (optional)
==============================================================

Driver Verifier turns up bugs that would otherwise surface as a random BSOD.

Admin Command Prompt:
    verifier /standard /driver shieldcord_filter.sys
    (reboot when prompted)

Use the VM normally for 30+ minutes. Zero BSODs is a pass.

Turn it off when you are done:
    verifier /reset


==============================================================
STEP 7 - UNINSTALL
==============================================================

Use Settings -> Apps -> ShieldCord -> Uninstall. It asks whether to keep your
logs and settings, then stops the service and removes the driver.

To go back to an ordinary, unmodified Windows afterwards:

    bcdedit /set testsigning off
    (reboot)

And turn Memory Integrity back on in Windows Security if you want it.


==============================================================
CONFIG (optional)
==============================================================

The engine reads its settings from:

    C:\ProgramData\ShieldCord\config.json

A copy of the documented defaults is in service\config.json in this folder.
Copy it there if you want a file you can edit:

    mkdir C:\ProgramData\ShieldCord
    copy Z:\ShieldCord-VM\service\config.json C:\ProgramData\ShieldCord\

The file is OPTIONAL. Every key is individually optional: the engine applies a
default to any key it does not find, and those defaults have protection ON
(driverEnforcementEnabled, driverFileBlockEnabled, driverAlertsEnabled and
decoyFolderEnabled all default to true). A machine with no config.json at all
is fully protected.

What the file buys you is a visible, editable record of what is set.


==============================================================
TROUBLESHOOTING
==============================================================

Driver will not install or will not load:
  - bcdedit | findstr testsigning        must say "Yes"
  - Secure Boot must be OFF (Step 1a), or bcdedit refuses the line above
  - Memory Integrity must be OFF (Step 1b), or the driver loads and fails
  - The app says which of these it is. Read the message on the dashboard.

Driver loads but nothing is blocked:
  - The service must be RUNNING: sc query ShieldCordSvc
    The driver goes fail-open whenever the service is not connected, so a
    stopped or crashed service means no protection at all. This is the single
    most common cause.
  - Check C:\ProgramData\ShieldCord\logs\shieldcord.log for the arming line.

Everything looks green but the grabber test still opens the file:
  - That is the one result that matters. Check the protected path list in the
    log, and that the file you tested is inside one of them.

BSOD on install:
  - Restore the snapshot from Step 1d.
  - Report the STOP code; it names the driver that faulted.

The dashboard says "Driver not loaded" and offers "Set up protection":
  - Normal on a machine where Step 3 has not run yet, or after
    `bcdedit /set testsigning off`.

The dashboard says "Engine unreachable":
  - The service is not answering. Check: sc query ShieldCordSvc
