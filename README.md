HEADLESS CROSS-PLATFORM COMPLETION PACKAGE
==========================================

Install from the maindev worktree:

Linux:
  cd ~/maindev
  unzip -o ~/Downloads/HEADLESS_CROSS_PLATFORM_completion.zip -d ~/maindev

Windows PowerShell example:
  Set-Location C:\path\to\maindev
  Expand-Archive -Force "$HOME\Downloads\HEADLESS_CROSS_PLATFORM_completion.zip" .

Launch a LAN server:

Linux:
  ./barony --headless --LAN --port=57165 --server-name=AutomatiaLAN

Windows Command Prompt or PowerShell:
  .\barony.exe --headless --LAN --port=57165 --server-name=AutomatiaLAN

Optional delayed startup:
  --autostart=30

Terminal commands on Linux and Windows:
  help
  status
  start
  shutdown

Windows may display a Microsoft Defender Firewall prompt the first time the UDP listener opens. Permit Private networks for LAN use. Do not permit Public networks unless you intentionally understand and accept the exposure.

CURRENT SECURITY BOUNDARY
-------------------------
Plain --headless opens no listener. --LAN explicitly opens the existing direct-connect UDP lobby. Public listing, password authentication, and in-progress late joining remain fail-closed because the required authentication and full-state snapshot protocols are not yet implemented.
