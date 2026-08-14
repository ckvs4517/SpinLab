# SpinLab 韌體

以 ESP32-C3 製作的戰鬥陀螺測速器。第一階段透過 GPIO 中斷擷取 IR 感測器 edge、記錄時間差並換算 RPM；目前輸出至序列埠，尚未包含 BLE、資料保存、SP 校正與 IMU。

## 專案結構

- `main/spinlab_main.c`：IR edge → period → RPM 的韌體入口；依 Demo 機設定使用 GPIO0、內部上拉、下降沿與每圈 1 pulse。
- `sdkconfig.defaults`：ESP32-C3 預設建置設定。
- `go.ps1`：執行 ESP-IDF 環境啟用、清理與建置。
- `go.cmd`：讓終端機可用 `go` 呼叫 `go.ps1`。
- `scripts/idf-shell.cmd`：在目前終端機開啟已啟用 ESP-IDF 的 PowerShell 子殼層。

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

### `go` 建置腳本

`go` 會在自己的 PowerShell 行程中自動啟用 ESP-IDF，因此不必手動輸入環境設定。

```powershell
go rebuild  # idf.py fullclean，然後產生新的 build\spinlab_firmware.bin
go clean    # 只執行 idf.py fullclean
```

實際工作邏輯在 `go.ps1`；`go.cmd` 只是簡短啟動器。若終端機找不到 `go`，改用：

```powershell
.\go.ps1 rebuild
```

### 開啟 ESP-IDF shell

需要手動執行多個 `idf.py` 指令（例如燒錄與監控）時，執行：

```powershell
.\scripts\idf-shell.cmd
```

它會在目前終端機開啟 PowerShell 子殼層、自動載入 ESP-IDF 並切換至專案根目錄。看到 `SpinLab ESP-IDF environment ready.` 後即可直接執行 `idf.py`；完成時輸入 `exit` 回到原本終端機。

燒錄並開啟序列監控：

```powershell
idf.py -p COM6 flash monitor
```

只重新開啟監控：

```powershell
idf.py -p COM6 monitor
```

離開 monitor 使用 `Ctrl+]`，若無效可按 `Ctrl+C`。

## 首次硬體驗證

1. 使用原廠 Demo 韌體確認感測器和機構可正常測速。
2. 將 Demo 機的 RPM IR 訊號接至 GPIO0，並與 ESP32-C3 共地；訊號必須是 3.3 V 邏輯。GPIO0 也是開機 strapping 腳，重開機時不要讓外部電路將它拉低。
3. 依光學圖樣調整 `SPINLAB_PULSES_PER_REVOLUTION`。
4. 燒錄後等待序列輸出：`IR diagnostic ready: GPIO 0, pull-up, falling edge`。
5. 轉動測試目標後，確認看到 `edge`、`period` 與 `rpm` 紀錄。

## 開發規則

- 不要提交 `build/`、`sdkconfig` 或量測資料；它們已由 `.gitignore` 排除。
- 保留原始 edge timestamp 是後續濾波、RPM 曲線和 SP 校正的基礎。
- 在基本測速穩定前，不加入 BLE、IMU 或網站功能。
