# SpinLab 韌體

以 ESP32-C3 製作的戰鬥陀螺測速器。透過 GPIO 中斷保存每個 IR edge timestamp，一次動作結束後輸出完整 Raw Pulse Profile，並以 BLE 傳送 Reference SP 與射擊摘要；尚未包含正式 SP 校正與 IMU。

## 專案結構

- `main/spinlab_main.c`：Raw Edge Capture 韌體入口；依 Demo 機設定使用 GPIO0 與內部上拉，可切換下降緣或雙邊緣擷取。
- `main/spinlab_ble.c`／`spinlab_ble.h`：NimBLE Peripheral、GATT Service 與射擊結果通知協定。
- `sdkconfig.defaults`：ESP32-C3 預設建置設定。
- `go.ps1`：執行 ESP-IDF 環境啟用、清理與建置。
- `go.cmd`：讓終端機可用 `go` 呼叫 `go.ps1`。
- `idf-shell.cmd`：在目前終端機開啟已啟用 ESP-IDF 的 PowerShell 子殼層。

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
.\idf-shell.cmd
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
3. 依測試階段設定 `SPINLAB_CAPTURE_BOTH_EDGES`；雙邊緣診斷為 `1`，穩定的下降緣基準為 `0`。
4. 燒錄後等待序列輸出：`raw capture ready: GPIO 0`。
5. 完成一次 Launcher 動作並停止；最後一個 edge 後約 2 秒會輸出完整 `RAW` 資料與摘要。

## Sensor 穩定度測試

每次測試都從靜止開始：慢慢完整拉到底、停住約 1 秒，再放手讓繩子回捲。完成後等待 Raw Data 輸出，再開始下一次測試。

```text
RAW,index,timestamp_us,delta_us,rpm,gap_ratio,level,edge_type,classification
RAW,0,12345678,0,0.00,0.00,0,falling,first_edge
RAW,1,12350256,4578,6553.12,0.00,1,rising,valid
```

- `timestamp_us`：開機後的絕對微秒 timestamp。
- `delta_us`：與上一個 edge 的間隔；RPM 直接由此值計算，未平滑。
- `level`：edge 發生後的 GPIO level；`0` 為 falling，`1` 為 rising。
- `edge_type`：此次 transition 的方向，用來檢查雙邊緣是否規律交替。
- `short_interval`：小於 500 μs 的疑似 bounce／雜訊；資料仍保留。
- `phase_break_candidate`：相對前 3～5 個有效 interval 的中位數放大至少 4 倍；只是 Raw Data 的明顯 gap 標記。
- `overflow=yes`：單次動作超過 4096 個 edge，後段資料未保存。

Raw Data 後會輸出 `SHOT` 摘要：

```text
SHOT,status,capture_mode,pull_start_index,pull_end_index,pull_edges,n_transition,pull_active_duration_us,reversal_gap_us,pull_to_first_rewind_us,transition_rate_hz,pull_peak_rpm,reference_sp_low,reference_sp_mid,reference_sp_high,reference_sp_uncertainty,pull_falling_edges,pull_rising_edges,alternation_errors,reversal_index,reversal_type,rewind_anomaly
SHOT,valid,both_edges,0,33,34,34,185118,20506,205624,183.67,8174.39,7223.8,7598.6,8015.0,791.2,17,17,0,34,smooth_gaps,no
```

CSV 後會再顯示方便人眼閱讀的摘要，完整 Raw 與 CSV 仍會保留：

```text
================ SPINLAB SHOT RESULT ================
Status             : valid
Reference SP       : 7599  (experimental / uncalibrated)
Estimated range    : 7224 - 8015
Range width        : 791
Transitions (N)    : 34  [falling=17, rising=17]
Alternation errors : 0
Pull active time   : 185.118 ms
Release time range : 185.118 - 205.624 ms
Transition rate    : 183.67 Hz
Pull peak RPM      : 8174.39
Reversal           : index 34 (smooth_gaps)
Rewind anomaly     : no
=====================================================
```

- `status=valid`：分界前至少有 8 個 edge、已找到 reversal，且 buffer 未溢位。
- `n_transition`：雙邊緣模式下 Pull 區段的黑白 transition 數；下降緣模式輸出 `-1`。
- `pull_active_duration_us`：第一個到最後一個 Pull transition，不包含最後 edge 後無法直接觀測的釋放時間。
- `reversal_gap_us`：最後 Pull edge 到第一個 Rewind edge；真正釋放時間位於此 gap 內。
- `pull_to_first_rewind_us`：第一個 Pull edge 到第一個 Rewind edge，是釋放時間的觀測上界。
- `transition_rate_hz`：`n_transition / pull_active_duration`，目前只是 SP 校正候選特徵。
- `reference_sp_low`：使用 `pull_to_first_rewind_us` 計算的保守參考下界。
- `reference_sp_mid`：暫以 reversal gap 中點作為釋放時間的參考中間值。
- `reference_sp_high`：使用 `pull_active_duration_us` 計算的參考上界。
- `reference_sp_uncertainty`：上下界差值；越大表示釋放時間造成的不確定性越高。
- `pull_falling_edges`／`pull_rising_edges`：Pull 區段兩種 edge 的數量。
- `alternation_errors`：相鄰 edge level 相同的次數；正常雙邊緣資料應為 `0`。
- `reversal_type=strong_gap`：單一 interval 相對近期中位數放大至少 4 倍。
- `reversal_type=smooth_gaps`：連續兩個 interval 相對同一個 Pull 基準都放大至少 2.5 倍。
- `invalid_short_capture`：edge 太少，通常是操作或殘留轉動造成的孤立 capture。
- `no_reversal`：有足夠 edge，但此次資料無法可靠找到 Pull／Rewind 分界。
- `rewind_anomaly=yes`：回捲中途出現相對 Pull 基準至少 10 倍的間隔，而且之後仍有至少 2 個 edge；避免把自然停止的尾端 gap 誤判為卡住。

演算法只採用第一個可信 reversal；Raw Data 不會被裁切或刪除。比較多次測試時，以 `SHOT` 摘要為主，仍保留完整 `RAW` 供後續校正。Raw Data 輸出期間不接受下一次動作；看到 `raw capture armed for the next launcher action` 後再開始。

### 雙邊緣 SP 診斷

`SPINLAB_CAPTURE_BOTH_EDGES=1` 時，同時擷取 rising 與 falling transition，並以每圈 2 edges 換算診斷 RPM。這是為了核對 SP 參考研究的黑白反轉計數 `N`，尚未代表 SpinLab 已採用該 SP 公式。

`reference_sp_*` 使用第三方研究公式 `43.195 × (N/T秒) + 81.402`，沒有加入該作者成品硬體專用的 `+2` 計數補償。這些欄位只能稱為未校正的 Reference Estimated SP，不是官方 SP；請保留 Raw Data 並與原廠裝置實測比較。

測試時慢慢完整拉到底、停住約 1 秒再放手，共做 3 次。檢查 rising／falling 是否交替、是否出現 `short_interval`，以及 Pull transition count 是否約為原下降緣模式的兩倍。若雙邊緣訊號不穩，將設定改回 `0` 即可恢復已驗證的 falling-edge 基準。

## 充電狀態

Demo PCB 的充電管理 IC 使用 GPIO10 回報狀態：LOW 表示充電中；韌體每秒在序列埠顯示 `charge status: charging` 或 `not charging`。

此 GPIO 只讀取狀態，不能直接替電池充電。請只透過 Demo PCB 的 USB-C／充電管理電路為電池充電；不要將鋰電池直接接到 ESP32-C3 的 3.3 V 或 GPIO 腳位。

### 藍色狀態燈

ESP32-C3 SuperMini 的內建藍燈位於 GPIO8，為 LOW 時亮。紅燈是直接接電源的硬體電源燈，無法由韌體控制。藍燈狀態如下，充電燈號優先於 BLE 燈號：

- 充電中：每 1.2 秒快速雙閃。
- 未充電、BLE 已連線：恆亮。
- 未充電、BLE 未連線：每秒慢閃一次（亮 0.5 秒、滅 0.5 秒）。

## BLE 射擊結果傳輸

韌體開機後會以 `SpinLab` 名稱持續廣播。手機 App 連線並訂閱 Result Characteristic 後，每次 capture 完成會收到固定 20-byte Notify；未連線時測速仍可獨立運作，最新結果會保留供 App 之後 Read。

- Service UUID：`8f4e1000-9c3a-4f2b-a7d1-6b5c2e91a001`
- Result Characteristic UUID：`8f4e1000-9c3a-4f2b-a7d1-6b5c2e91a002`
- Characteristic properties：Read、Notify
- Byte order：little-endian

### Result Packet v1

| Offset | Type | Field | Unit／說明 |
|---:|---|---|---|
| 0 | `uint8` | version | 固定為 `1` |
| 1 | `uint8` | status | `0` valid、`1` invalid short、`2` no reversal、`3` overflow |
| 2 | `uint8` | flags | bit 0 雙邊緣、bit 1 回捲異常、bit 2 edge 交替錯誤 |
| 3 | `uint8` | reversal_type | `0` none、`1` strong gap、`2` smooth gaps |
| 4 | `uint16` | shot_id | 開機後遞增，溢位後循環 |
| 6 | `uint16` | reference_sp_mid | 未校正 Reference SP 中間值 |
| 8 | `uint16` | reference_sp_low | Reference SP 下界 |
| 10 | `uint16` | reference_sp_high | Reference SP 上界 |
| 12 | `uint16` | transitions | Pull `N_transition` |
| 14 | `uint16` | pull_active_time | 0.1 ms |
| 16 | `uint16` | reversal_gap | 0.1 ms |
| 18 | `uint16` | pull_peak_rpm | 診斷 RPM，四捨五入為整數 |

此封包可在 BLE 預設 ATT MTU 下完整通知，不需要分段重組。`pull_to_first_rewind` 可由 `pull_active_time + reversal_gap` 算出，transition rate 可由 `N / pull_active_time` 算出。

目前是 SpinLab 自有協定，不相容官方 Battle Pass App；第一版未啟用 pairing／加密，也不透過 BLE 傳送完整 Raw edges。USB 序列 Raw 與 CSV 仍保留供韌體診斷，後續 App 穩定後再設計 Raw 分段傳輸與連線安全。

## 開發規則

- 不要提交 `build/`、`sdkconfig` 或量測資料；它們已由 `.gitignore` 排除。
- 保留原始 edge timestamp 是後續濾波、RPM 曲線和 SP 校正的基礎。
- 在基本測速穩定前，不加入 BLE、IMU 或網站功能。
