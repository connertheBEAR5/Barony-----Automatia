# The main project creates an audited, binary-only payload. The NSIS commands
# below copy game assets from the user's existing Barony install, then place
# that payload beside the copied assets as a separate non-Steam game.

if (NOT DEFINED BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES)
	message(FATAL_ERROR
		"The side-by-side installer needs BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES.")
endif()

set(_barony_side_editor_shortcut_commands "")
if (EDITOR_ENABLED)
	set(_barony_side_editor_shortcut_commands [=[
StrCmp $R5 1 0 barony_automatia_side_editor_shortcut_skip
ExecWait '"$INSTDIR\barony_automatia_steam_shortcut.exe" --add "$R0" "Barony Automatia Editor" "$R2\editor.exe" "$R2" "$INSTDIR\automatia_steam_artwork"' $R3
IntCmpU $R3 0 barony_automatia_side_editor_shortcut_skip barony_automatia_side_editor_shortcut_failed barony_automatia_side_editor_shortcut_failed
barony_automatia_side_editor_shortcut_failed:
MessageBox MB_OK|MB_ICONEXCLAMATION "Automatia was installed, but Steam could not add the optional editor shortcut. Close Steam completely and run the installer again, or add this executable manually as a Non-Steam Game: $R2\editor.exe"
barony_automatia_side_editor_shortcut_skip:
]=])
endif()

set(_barony_side_runtime_file_arguments "")
set(_barony_side_verify_runtime_commands "")
set(_barony_side_delete_runtime_commands "")
foreach(_barony_side_runtime_file IN LISTS BARONY_AUTOMATIA_SIDEBYSIDE_RUNTIME_FILES)
	string(APPEND _barony_side_runtime_file_arguments " \"${_barony_side_runtime_file}\"")
	string(APPEND _barony_side_verify_runtime_commands
		"StrCpy $R4 \"${_barony_side_runtime_file}\"\n"
		"IfFileExists \"$R2\\${_barony_side_runtime_file}\" 0 barony_automatia_side_runtime_missing\n")
	string(APPEND _barony_side_delete_runtime_commands
		"Delete \"$R2\\${_barony_side_runtime_file}\"\n")
endforeach()

# NSIS CopyFiles starts shell copy work asynchronously.  Use robocopy instead
# so a completed process exit (and the verification below) proves every game
# runtime file has reached the separate Automatia directory.
set(_barony_side_copy_runtime_commands [=[
ExecWait '"$SYSDIR\robocopy.exe" "$INSTDIR" "$R2"@BARONY_AUTOMATIA_RUNTIME_FILE_ARGUMENTS@ /COPY:DAT /R:2 /W:1' $R3
IntCmpU $R3 7 barony_automatia_side_runtime_copy_ok barony_automatia_side_runtime_copy_ok barony_automatia_side_runtime_copy_failed
barony_automatia_side_runtime_copy_failed:
MessageBox MB_OK|MB_ICONSTOP "Automatia's runtime files could not be copied to $R2 (robocopy exit code $R3). Vanilla Barony was not modified."
Abort
barony_automatia_side_runtime_copy_ok:
]=])
string(REPLACE "@BARONY_AUTOMATIA_RUNTIME_FILE_ARGUMENTS@"
	"${_barony_side_runtime_file_arguments}" _barony_side_copy_runtime_commands
	"${_barony_side_copy_runtime_commands}")

set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS [=[
SetRegView 64
ReadRegStr $R0 HKCU "Software\Valve\Steam" "SteamPath"
StrCmp $R0 "" barony_automatia_side_try_lm64
Goto barony_automatia_side_have_steam_root
barony_automatia_side_try_lm64:
ReadRegStr $R0 HKLM "Software\Valve\Steam" "InstallPath"
StrCmp $R0 "" barony_automatia_side_try_32
Goto barony_automatia_side_have_steam_root
barony_automatia_side_try_32:
SetRegView 32
ReadRegStr $R0 HKCU "Software\Valve\Steam" "SteamPath"
StrCmp $R0 "" barony_automatia_side_try_lm32
Goto barony_automatia_side_have_steam_root
barony_automatia_side_try_lm32:
ReadRegStr $R0 HKLM "Software\Valve\Steam" "InstallPath"
barony_automatia_side_have_steam_root:
StrCmp $R0 "" barony_automatia_side_default_steam_root
Goto barony_automatia_side_try_primary_library
barony_automatia_side_default_steam_root:
StrCpy $R0 "$PROGRAMFILES32\Steam"
barony_automatia_side_try_primary_library:
StrCpy $R6 "$R0\steamapps\common"
StrCpy $R1 "$R6\Barony"
IfFileExists "$R1\barony.exe" barony_automatia_side_steam_found barony_automatia_side_try_library_file
barony_automatia_side_try_library_file:
IfFileExists "$R0\steamapps\libraryfolders.vdf" barony_automatia_side_open_library_file barony_automatia_side_no_steam
barony_automatia_side_open_library_file:
ClearErrors
FileOpen $R5 "$R0\steamapps\libraryfolders.vdf" r
IfErrors barony_automatia_side_no_steam
barony_automatia_side_read_library_line:
ClearErrors
FileRead $R5 $R8
IfErrors barony_automatia_side_library_done
barony_automatia_side_trim_library_line:
StrCpy $R9 $R8 1
StrCmp $R9 " " barony_automatia_side_trim_library_character
StrCmp $R9 "$\t" barony_automatia_side_trim_library_character
Goto barony_automatia_side_library_key_check
barony_automatia_side_trim_library_character:
StrCpy $R8 $R8 "" 1
Goto barony_automatia_side_trim_library_line
barony_automatia_side_library_key_check:
StrCpy $R9 $R8 6
StrCmp $R9 "$\"path$\"" barony_automatia_side_extract_library_path barony_automatia_side_read_library_line
barony_automatia_side_extract_library_path:
StrCpy $R8 $R8 "" 6
barony_automatia_side_trim_library_path:
StrCpy $R9 $R8 1
StrCmp $R9 " " barony_automatia_side_trim_library_path_character
StrCmp $R9 "$\t" barony_automatia_side_trim_library_path_character
Goto barony_automatia_side_library_path_quote_check
barony_automatia_side_trim_library_path_character:
StrCpy $R8 $R8 "" 1
Goto barony_automatia_side_trim_library_path
barony_automatia_side_library_path_quote_check:
StrCpy $R9 $R8 1
StrCmp $R9 "$\"" barony_automatia_side_scan_library_path barony_automatia_side_read_library_line
barony_automatia_side_scan_library_path:
StrCpy $R8 $R8 "" 1
StrCpy $R9 0
barony_automatia_side_library_path_character:
StrCpy $R7 $R8 1 $R9
StrCmp $R7 "$\"" barony_automatia_side_library_path_complete
StrCmp $R7 "" barony_automatia_side_read_library_line
IntOp $R9 $R9 + 1
Goto barony_automatia_side_library_path_character
barony_automatia_side_library_path_complete:
StrCpy $R6 $R8 $R9
StrCmp $R6 "" barony_automatia_side_read_library_line
StrCpy $R6 "$R6\steamapps\common"
StrCpy $R1 "$R6\Barony"
IfFileExists "$R1\barony.exe" barony_automatia_side_library_found barony_automatia_side_read_library_line
barony_automatia_side_library_found:
FileClose $R5
Goto barony_automatia_side_steam_found
barony_automatia_side_library_done:
FileClose $R5
Goto barony_automatia_side_no_steam
barony_automatia_side_no_steam:
MessageBox MB_OK|MB_ICONSTOP "Steam's Barony installation was not found. Install Barony through Steam first, then run this installer again."
Abort
barony_automatia_side_steam_found:
StrCpy $R2 "$R6\Barony Automatia"
StrCmp $R1 $R2 barony_automatia_side_unsafe_destination
WriteRegStr HKCU "Software\Automatia\Barony Steam Install" "SteamRoot" "$R0"
WriteRegStr HKCU "Software\Automatia\Barony Steam Install" "VanillaInstallDir" "$R1"
WriteRegStr HKCU "Software\Automatia\Barony Steam Install" "AutomatiaInstallDir" "$R2"
MessageBox MB_OK|MB_ICONINFORMATION "Automatia will be installed beside vanilla Barony. Exit Steam completely before continuing so the Automatia shortcut can be added."
StrCpy $R5 0
MessageBox MB_YESNO|MB_ICONQUESTION "Also add Barony Automatia Editor as a separate Steam Library shortcut?" IDYES barony_automatia_side_editor_shortcut_requested IDNO barony_automatia_side_editor_shortcut_choice_done
barony_automatia_side_editor_shortcut_requested:
StrCpy $R5 1
barony_automatia_side_editor_shortcut_choice_done:
Goto barony_automatia_side_destination_safe
barony_automatia_side_unsafe_destination:
MessageBox MB_OK|MB_ICONSTOP "Automatia refused to install because its destination matches vanilla Barony. Vanilla Barony is never modified by this installer."
Abort
barony_automatia_side_destination_safe:
]=])
string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}")
string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}")

set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [=[
ReadRegStr $R0 HKCU "Software\Automatia\Barony Steam Install" "SteamRoot"
ReadRegStr $R1 HKCU "Software\Automatia\Barony Steam Install" "VanillaInstallDir"
ReadRegStr $R2 HKCU "Software\Automatia\Barony Steam Install" "AutomatiaInstallDir"
ExecWait '"$INSTDIR\automatia_prerequisites\VC_redist.x64.exe" /install /quiet /norestart' $R4
StrCmp $R4 0 barony_automatia_side_redist_ok
StrCmp $R4 1638 barony_automatia_side_redist_ok
StrCmp $R4 3010 barony_automatia_side_redist_ok
MessageBox MB_OK|MB_ICONSTOP "The Microsoft Visual C++ x64 runtime could not be installed (exit code $R4). Automatia was not installed."
Abort
barony_automatia_side_redist_ok:
CreateDirectory "$R2"
; Copy every vanilla asset and data file, but never its executables, DLLs, or
; Steam cloud configuration. Automatia supplies its own compatible runtime.
ExecWait '"$SYSDIR\robocopy.exe" "$R1" "$R2" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /XF *.exe *.dll steam_appid.txt steam_autocloud.vdf' $R3
IntCmpU $R3 7 barony_automatia_side_assets_ok barony_automatia_side_assets_ok barony_automatia_side_assets_failed
barony_automatia_side_assets_failed:
MessageBox MB_OK|MB_ICONSTOP "The vanilla Barony asset copy failed. Automatia was not completed."
Abort
barony_automatia_side_assets_ok:
@BARONY_AUTOMATIA_RUNTIME_COPY_COMMANDS@
@BARONY_AUTOMATIA_RUNTIME_VERIFY_COMMANDS@
Goto barony_automatia_side_runtime_ready
barony_automatia_side_runtime_missing:
MessageBox MB_OK|MB_ICONSTOP "Automatia runtime file $R4 could not be written to $R2. Check that the Steam library folder is writable, then run this installer again."
Abort
barony_automatia_side_runtime_ready:
ExecWait '"$SYSDIR\robocopy.exe" "$INSTDIR\automatia_licenses" "$R2\automatia_licenses" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1' $R4
IntCmpU $R4 7 barony_automatia_side_licenses_ok barony_automatia_side_licenses_ok barony_automatia_side_licenses_failed
barony_automatia_side_licenses_failed:
MessageBox MB_OK|MB_ICONSTOP "Automatia's license material could not be copied. Automatia was not completed."
Abort
barony_automatia_side_licenses_ok:
ExecWait '"$SYSDIR\robocopy.exe" "$INSTDIR\automatia_third_party_source" "$R2\automatia_third_party_source" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1' $R4
IntCmpU $R4 7 barony_automatia_side_source_ok barony_automatia_side_source_ok barony_automatia_side_source_failed
barony_automatia_side_source_failed:
MessageBox MB_OK|MB_ICONSTOP "Automatia's corresponding source material could not be copied. Automatia was not completed."
Abort
barony_automatia_side_source_ok:
FileOpen $0 "$R2\barony_automatia_install.marker" w
FileClose $0
ExecWait '"$INSTDIR\barony_automatia_steam_shortcut.exe" --add "$R0" "Barony Automatia" "$R2\barony.exe" "$R2" "$INSTDIR\automatia_steam_artwork"' $R3
IntCmpU $R3 0 barony_automatia_side_shortcut_ok barony_automatia_side_shortcut_failed barony_automatia_side_shortcut_failed
barony_automatia_side_shortcut_failed:
MessageBox MB_OK|MB_ICONEXCLAMATION "Automatia was installed, but Steam could not add the library shortcut. Close Steam completely and run the installer again, or add this executable manually as a Non-Steam Game: $R2\barony.exe"
barony_automatia_side_shortcut_ok:
@BARONY_AUTOMATIA_EDITOR_SHORTCUT_COMMANDS@
]=])
string(REPLACE "@BARONY_AUTOMATIA_RUNTIME_COPY_COMMANDS@"
	"${_barony_side_copy_runtime_commands}" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
string(REPLACE "@BARONY_AUTOMATIA_RUNTIME_VERIFY_COMMANDS@"
	"${_barony_side_verify_runtime_commands}" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
string(REPLACE "@BARONY_AUTOMATIA_EDITOR_SHORTCUT_COMMANDS@"
	"${_barony_side_editor_shortcut_commands}" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")
string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_INSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}")

set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [=[
ReadRegStr $R0 HKCU "Software\Automatia\Barony Steam Install" "SteamRoot"
ReadRegStr $R2 HKCU "Software\Automatia\Barony Steam Install" "AutomatiaInstallDir"
IfFileExists "$INSTDIR\barony_automatia_steam_shortcut.exe" 0 barony_automatia_side_skip_shortcut_remove
ExecWait '"$INSTDIR\barony_automatia_steam_shortcut.exe" --remove "$R0" "Barony Automatia" "$R2\barony.exe" "$R2"' $R3
barony_automatia_side_skip_shortcut_remove:
; Preserve copied vanilla assets and save data. Only remove Automatia runtime files.
@BARONY_AUTOMATIA_RUNTIME_DELETE_COMMANDS@
RMDir /r "$R2\automatia_licenses"
RMDir /r "$R2\automatia_third_party_source"
Delete "$R2\barony_automatia_install.marker"
DeleteRegValue HKCU "Software\Automatia\Barony Steam Install" "SteamRoot"
DeleteRegValue HKCU "Software\Automatia\Barony Steam Install" "VanillaInstallDir"
DeleteRegValue HKCU "Software\Automatia\Barony Steam Install" "AutomatiaInstallDir"
DeleteRegKey /ifempty HKCU "Software\Automatia\Barony Steam Install"
]=])
string(REPLACE "@BARONY_AUTOMATIA_RUNTIME_DELETE_COMMANDS@"
	"${_barony_side_delete_runtime_commands}" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}")
string(REPLACE "\\" "\\\\" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}")
string(REPLACE "\"" "\\\"" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
	"${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}")
