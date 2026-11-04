# Before you run / Trước khi chạy

Open source and readable scripts help inspection; they are not proof that any
download is harmless. This is the intended behavior of the controller update.

* PowerMoon-Switch2Pro-Install.cmd runs the local PowerMoon-Switch2Pro-Setup.ps1. You choose your existing
  PowerMoonRecomp.exe. It checks the supported 0.1.6 executable hash and hashes
  of the two supplied payload files, backs up PowerMoonRecomp.exe and any existing
  libc++.dll under controller-backups/, then replaces only those program files.
  It refuses unsupported versions and asks you to save/close a running game.
* PowerMoon-Switch2Pro-Restore.cmd restores that backup. PowerMoon-Switch2Pro-Play.cmd remembers your chosen folders in
  .local-settings.json beside the updater, starts the separately installed bridge
  if necessary and starts your game. No administrator rights are requested by
  this update. The bridge's separate first-time signed-driver install needs them.
* The new haptic transport uses only 127.0.0.1:26761. The controller additions
  introduce no telemetry or remote upload. The updater scripts do not fetch or
  execute remote script text, register autorun, disable antivirus or change firewall
  rules. Upstream's optional initial setup/compiler downloads remain upstream
  behavior; this update is intended for an existing installation.
* The update does not collect, supply or modify your game files, keys, save or
  extracted/generated game library. Do not upload those when reporting a problem.

Download only from this repository's Releases. Compare the ZIP against
CHECKSUMS-SHA256.txt with `Get-FileHash -Algorithm SHA256 <download.zip>` and
inspect the matching source tag, or build it yourself. Checksums check integrity,
not malware safety. **This project's executable is currently unsigned.** An
unknown-publisher or SmartScreen warning may occur. Keep Defender enabled; do
not add blanket exclusions or bypass an unexplained antivirus detection.
The cmd launchers set ExecutionPolicy Bypass only for their local PowerShell
process; they do not persist a system policy change.

To remove the update, close the game, run PowerMoon-Switch2Pro-Restore.cmd, then remove the extracted
updater folder. Backups remain in your game folder for your own recovery.
No background service or startup task is installed by these update scripts.

**Compatibility evidence:** gameplay tested only in TOTK Recomp and SMO / Power
Moon Recomp. No emulator has been tested; do not read the documentation as a
Ryujinx/Eglantine compatibility guarantee. Haptic frequency calibration is experimental.

**Tiếng Việt:** Script cài đặt cho bạn chọn bản 0.1.6, kiểm tra file và sao lưu
trước khi thay đúng hai file chương trình; không kèm game hay keys. PowerMoon-Switch2Pro-Play.cmd nhớ
thư mục đã chọn để mở bridge và game; PowerMoon-Switch2Pro-Restore.cmd khôi phục bản cũ. Exe chưa có
chữ ký số. Có source và SHA-256 để kiểm tra, nhưng không khẳng định tuyệt đối
“không có virus”. Không cần tắt Defender hay thêm ngoại lệ antivirus. Đến nay chỉ
test gameplay với TOTK Recomp và SMO Recomp, **chưa test trình giả lập nào**.
