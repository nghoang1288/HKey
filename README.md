# HKey

Bộ gõ tiếng Việt thông minh cho Windows với khả năng tự chuyển đổi linh hoạt giữa tiếng Việt và tiếng Anh, hỗ trợ gõ tắt (macro), giao diện hiện đại và gọn nhẹ.

Phát triển bởi [nghoang1288](https://github.com/nghoang1288). Mã nguồn mở theo giấy phép GPL-3.0; dựa trên [Vikey](https://github.com/tonyminibot/Vikey) của Tony Dinh và [OpenKey](https://github.com/tuyenvm/OpenKey) của Tuyên Mai.

---

## Tính năng nổi bật

- **Gõ Telex thông minh**: Đặt dấu thanh kiểu mới hoặc cũ (hoà / hòa), kiểm tra chính tả, tự động phục hồi phím khi gõ sai.
- **Gõ tắt (Macro expansion)**:
  - Hỗ trợ file `macro.txt` tương thích định dạng `k:không` và `k||không` (chuẩn UniKey / EVKey).
  - Tự động nhận diện chữ hoa/thường: `k` $\rightarrow$ `không`, `K` $\rightarrow$ `Không`, `K`(Caps) $\rightarrow$ `KHÔNG`.
  - Thay thế phím nguyên tử (atomic `SendInput`) không nháy phím, không kẹt autocomplete trên trình duyệt.
  - Tự động cập nhật ngay khi sửa và lưu file `macro.txt`.
- **Tự động chuyển Anh / Việt**:
  - `more` $\rightarrow$ giữ nguyên tiếng Anh.
  - `treen` $\rightarrow$ `trên`.
  - `truowfng`, `truongwf`, `truwownfg` $\rightarrow$ `trường`.
  - `this makes sense` gõ tự nhiên không bị dính dấu tiếng Việt.
- **Hỗ trợ lập trình**: Giữ nguyên cú pháp `var`, `const`, `git push`, `obj.dir`, `my_var`...
- **Hoàn tác nhanh**: Bấm Delete (Backspace) ngay sau khi tự chuyển để hoàn tác.
- **Nhớ ngôn ngữ theo ứng dụng và trang web**: Tự động chuyển ngôn ngữ phù hợp theo từng cửa sổ hoặc từng tab trình duyệt (Chrome, Edge, Brave, Vivaldi, Opera, Firefox).
- **Phím chuyển linh hoạt**: Đặt phím tắt tùy chỉnh (ví dụ `Alt+Z`, `Ctrl+Shift`...) kèm âm báo nhẹ khi chuyển.
- **Tự động cập nhật**: Kiểm tra và cập nhật trực tiếp qua GitHub Releases.
- **Bản Portable**: Không cần cài đặt, chạy trực tiếp file `HKey.exe`.

---

## Cài đặt & Sử dụng

### 1. Bản Portable
1. Tải bản mới nhất từ [GitHub Releases](https://github.com/nghoang1288/HKey/releases).
2. Giải nén và chạy file `HKey.exe`.
3. Bảng gõ tắt nằm cùng thư mục trong file `macro.txt`.

### 2. Biên dịch từ mã nguồn

Yêu cầu CMake, Ninja và bộ công cụ C++ (MSVC hoặc GCC / MinGW-w64).

```powershell
# Biên dịch và đóng gói bản Portable
pwsh Windows/build_portable.ps1
```

---

## Giấy phép & Ghi nhận

HKey được phân phối theo giấy phép **GNU General Public License v3.0 (GPL-3.0)**.
Dựa trên Vikey của Tony Dinh và OpenKey của Tuyên Mai.
