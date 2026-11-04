# Quy trình cập nhật PowerfulMoon

## 1. Bot tự theo dõi Power Moon

[Upstream candidate bot](https://github.com/yngvifreyr1810/PowerfulMoon/actions/workflows/upstream-sync.yml) chạy trên máy chủ GitHub **mỗi 2 giờ**. Không cần mở máy cá nhân, Codex, Gemini hoặc dùng quota AI. Có thể kiểm tra ngay bằng **Run workflow**, chọn nhánh `main`.

Khi thấy phiên bản ổn định mới trên GitLab, bot lấy mã nguồn đúng commit của phiên bản đó, thử ghép với PowerfulMoon trong một bản sao riêng và giữ các thay đổi của fork nếu Git ghép được. Bot không tải EXE Power Moon gốc về ghi đè PowerfulMoon.

* **Ghép được:** tạo nhánh `automation/upstream-v…`, mở issue có tiêu đề **Upstream v…: PowerfulMoon review** và chạy build bản ứng viên.
* **Xung đột hoặc thay đổi giấy phép/phụ thuộc cần xem xét:** dừng, ghi lỗi trong issue. Cần sửa mã nguồn rồi build lại; bot không tự đoán cách giải quyết.
* **Build/test lỗi:** ghi kết quả thất bại và đường dẫn log vào issue; không phát hành.

GitHub có thể chạy lịch muộn. Nếu repository không hoạt động trong thời gian dài, GitHub có thể tắt lịch; vào Actions để bật lại. Bật thông báo cho repository/issue nếu muốn nhận email từ GitHub.

## 2. Lấy bản ứng viên để kiểm thử

Mở issue ở [Issues](https://github.com/yngvifreyr1810/PowerfulMoon/issues), bấm **Open run and artifacts**, hoặc vào [Build review candidate](https://github.com/yngvifreyr1810/PowerfulMoon/actions/workflows/candidate.yml).

Chỉ dùng run có trạng thái **Success**. Ở cuối trang run, tải artifact `PowerfulMoon-candidate-<commit>`, rồi giải nén. Bên trong có Setup, ZIP portable, SHA-256, báo cáo kiểm tra nội dung và `release.json`.

Ghi lại hai giá trị để duyệt đúng bản:

* **Run ID:** dãy số cuối URL dạng `/actions/runs/123456789`.
* **Commit SHA:** đủ 40 ký tự ở cuối tên artifact; issue cũng hiển thị giá trị này.

Artifact lưu 30 ngày. Nếu hết hạn, chạy lại **Build review candidate** với commit cần thử và kiểm tra lại artifact vừa tạo.

## 3. Kiểm thử trước khi duyệt

Lưu và thoát game trước khi cài. Nên sao lưu save/settings hoặc dùng một bản sao thư mục đang chơi để thử nâng cấp. Chạy Setup của ứng viên và chọn **đúng thư mục game hiện có**. Các file chương trình bị thay thế được sao lưu trong `powerfulmoon-backups/`; save, mod, dữ liệu game và cấu hình tay cầm được giữ.

Kiểm tra:

* Game mở được, đúng phiên bản, load được save và giữ mod/settings cũ.
* Switch 2 Pro Controller kết nối Bluetooth, nút/cần/GL/GR hoạt động; motion đúng hướng và rung phản hồi trong gameplay. Thử **Test vibration (2 seconds)**.
* Bật VRR cùng nội suy đầu ra 120 fps, kiểm tra hình ảnh/FPS. Logic game vẫn chạy ở 60 Hz.
* Thoát rồi mở lại: cấu hình vẫn còn. Nút **F1 → Controls → Check for updates** mở được cửa sổ cập nhật. Vì ứng viên chưa phát hành nên ứng dụng khác chưa được đề nghị cập nhật lên bản này.

Build/test tự động không thay thế việc kiểm thử gameplay trên phần cứng thật. Có thể gửi Run ID và commit cho Codex kiểm tra thêm trước khi duyệt.

## 4. Duyệt và phát hành

Chỉ thực hiện khi muốn **công khai bản vừa thử cho mọi người**:

1. Mở [Publish owner-reviewed candidate](https://github.com/yngvifreyr1810/PowerfulMoon/actions/workflows/publish-reviewed.yml), chọn **Run workflow**, nhánh `main`.
2. Điền `candidate_run` = Run ID đã thành công; `approved_sha` = commit 40 ký tự đã kiểm thử; `confirmation` = `PUBLISH`.
3. Run sẽ chờ tại môi trường `release-approval`. Mở run, chọn **Review deployments**, chọn môi trường đó rồi **Approve and deploy** bằng tài khoản chủ repository.
4. Job kiểm tra danh tính commit, checksum và báo cáo nội dung; ghép mã nguồn đã duyệt vào `main` để làm nền cho lần cập nhật sau; tải lên đúng bộ cài đã kiểm thử rồi mới công khai release. Nếu nhánh đã duyệt xung đột với `main` mới hơn, job dừng để xử lý trước. Không build lại một bản khác trong lúc phát hành.

Bot chạy theo lịch không có quyền tự vượt bước này. Không muốn phát hành thì không chạy workflow duyệt, hoặc chọn **Reject**. Nếu phát hiện lỗi, sửa và tạo ứng viên mới, sau đó duyệt SHA mới.

## 5. Người dùng cập nhật PowerfulMoon

Sau khi release công khai đầy đủ, ứng dụng mới nhìn thấy bản đó:

1. Mở **F1 → Controls → Check for updates**. Có thể bật/tắt kiểm tra tự động khi mở game; kiểm tra tự động tối đa một lần mỗi ngày.
2. Chọn **Download update**. Ứng dụng tải bộ cài PowerfulMoon từ GitHub và kiểm tra SHA-256.
3. Lưu tiến trình, thoát game rồi chọn **Install update** trong cửa sổ cập nhật. Setup mở với thư mục game đang dùng đã được chọn sẵn; kiểm tra thư mục rồi bấm **Install / Upgrade**.
4. Mở lại PowerfulMoon. Save, mod và cấu hình tay cầm vẫn nằm trong thư mục cũ.

Ứng dụng không tự cài khi đang chơi và không nhận bản gốc từ GitLab hoặc bản ứng viên chưa duyệt. Bản PowerfulMoon cũ chưa có nút Update cần cài bản có tính năng này **một lần**; các lần sau dùng quy trình trong app.

Không bước nào yêu cầu đưa game, keys, save hay tài nguyên Nintendo lên GitHub. Xem [giải thích kỹ thuật](UPDATES.md), [credits](../CREDITS.md) và [bảo mật](../SECURITY.md).
