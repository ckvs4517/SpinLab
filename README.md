# SpinLab 韌體

以 ESP32-C3 製作的戰鬥陀螺測速器。第一階段透過 GPIO 中斷擷取 IR 感測器 edge、記錄時間差並換算 RPM；目前輸出至序列埠，尚未包含 BLE、資料保存、SP 校正與 IMU。

## 專案結構

- `main/spinlab_main.c`：IR edge → period → RPM 的韌體入口；預設 IR 輸入為 GPIO 2、每圈 1 pulse。
- `sdkconfig.defaults`：ESP32-C3 預設建置設定。
- `go.ps1`：實際執行環境啟用、清理與建置的 PowerShell 腳本。
- `go.cmd`：讓終端機可用簡短的 `go` 指令呼叫 `go.ps1`。

## 開發環境

需求：Windows、Git、ESP32-C3 開發板與 USB 資料線。使用 ESP-IDF **v5.5.2**。

首次安裝 ESP-IDF Installation Manager：

```powershell
winget install --id Espressif.EIM-CLI --exact
```

關閉並重新開啟 PowerShell，再安裝 ESP32-C3 工具鏈：

```powershell
$eim = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Espressif.EIM-CLI_Microsoft.Winget.Source_8wekyb3d8bbwe\eim.exe"
& $eim install --path 'C:\Espressif' --idf-versions 'v5.5.2' --target 'esp32c3' --non-interactive true --install-all-prerequisites true --cleanup true --do-not-track true --create-bat-activation-script true
```

## 常用指令

在 VS Code 或 PowerShell 終端機中，先進入專案根目錄：

```powershell
Set-Location C:\KAI\SpinLab
```

建議使用 `go` 腳本；它會自動載入 ESP-IDF 環境。

```powershell
go rebuild  # 刪除 build/，再產生新的 build\spinlab_firmware.bin
go clean    # 僅刪除 build/
```

若終端機找不到 `go`，使用：

```powershell
.\go.ps1 rebuild
```

燒錄並開啟序列監控（將 `COM5` 改為實際連接埠）：

```powershell
idf.py -p COM5 flash monitor
```

若只需重新開啟監控：

```powershell
idf.py -p COM5 monitor
```

離開 monitor 使用 `Ctrl+]`，若無效可按 `Ctrl+C`。

## 首次硬體驗證 (目前尚未驗證)

1. 使用@cococat_3d 製作的測速器 Demo 韌體確認感測器和機構可正常測速。
2. 將 IR 訊號接至 GPIO 2，並與 ESP32-C3 共地；訊號必須是 3.3 V 邏輯。
3. 依光學圖樣調整 `SPINLAB_PULSES_PER_REVOLUTION`。
4. 燒錄後等待序列輸出：`IR diagnostic ready on GPIO 2`。
5. 轉動測試目標後，確認看到 `edge`、`period` 與 `rpm` 紀錄。

## 開發規則

- 不要提交 `build/`、`sdkconfig` 或量測資料；它們已由 `.gitignore` 排除。
- 保留原始 edge timestamp 是後續濾波、RPM 曲線和 SP 校正的基礎。
- 在基本測速穩定前，不加入 BLE、IMU 或網站功能。
