# plugin-analyzer-cli：命令列與 JSON 介面

`plugin-analyzer-cli` 不開介面，直接量測插件並輸出 JSON。適合用在：

- 寫腳本批次量測多個插件或多組參數
- 在 CI 裡做插件的回歸測試（例如「THD 不能超過 0.01%」）
- 給其他程式或 LLM 使用（MCP server 的 `measure` 工具使用同一套 JSON 格式，見 [MCP.md](MCP.md)）

每個量測的意思請看 [METRICS.md](METRICS.md)。

---

## 取得與執行

- **下載**：GitHub Release 裡的 `PluginAnalyzer-<版本>-macOS-CLI.zip` / `-Windows-CLI.zip`
- **自己編譯**：`cmake --build build --target PluginAnalyzerCli`，產出在 `build/PluginAnalyzerCli_artefacts/Release/plugin-analyzer-cli`
- macOS 從瀏覽器下載的版本要先解除隔離：`xattr -d com.apple.quarantine plugin-analyzer-cli`

---

## 指令

### `describe`：列出所有模式、設定和內建處理器
```bash
plugin-analyzer-cli describe --pretty
```
輸出包含每個設定的名稱、型別、單位、預設值和說明。不確定要怎麼設定時，先看這個。

### `plugin-info`：看一個插件的基本資料和參數
```bash
plugin-analyzer-cli plugin-info "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 4.vst3" --pretty
```
會列出名稱、廠商、格式、版本、延遲（樣本）、聲道數，以及**所有參數**（名稱、ID、目前值）。設定參數前，先用這個查參數的正確名稱。

### `measure`：用命令列參數跑一次量測
```bash
plugin-analyzer-cli measure <mode> [選項]
```

| 選項 | 說明 | 例子 |
|---|---|---|
| `--plugin <插件>` | 加到 chain 1（可以重複，依序串接） | `--plugin builtin:compressor` |
| `--chain2 <插件>` | 加到 chain 2（可以重複） | `--chain2 "Pro-C 2.vst3"` |
| `--param C.S.名稱=值` | 設定參數：第 C 條 chain、第 S 個 slot（從 1 開始），名稱或 ID 都可以 | `--param "1.1.Band 1 Drive=50"` |
| `--set 設定=值` | 量測設定（名稱見 `describe`）。陣列用逗號 | `--set harmonic_freq_hz=100`、`--set ar_levels_db=-40,0,-40` |
| `--difference` | 量 chain 1 減 chain 2（null test） | |
| `--points N` | 每條曲線最多幾個點（預設 200，0 = 只要數字） | `--points 0` |
| `--pretty` | 縮排輸出 | |
| `--out 檔案` | 寫進檔案，不印到畫面 | `--out result.json` |

**插件的寫法：** `builtin:<id>`（內建測試處理器，例如 `builtin:saturator`），或 `.vst3` / `.component` 的路徑。

**參數值：** 用文字給，由插件自己解讀，例如 `6`、`6 dB`、`Warm Tape`。也可以在 JSON 請求裡用 `{ "normalized": 0.25 }` 直接指定 0–1 的原始值。輸出會回報每個參數**實際被設成什麼**，方便確認插件有沒有正確解讀。

### `mcp`：以 MCP server 執行
```bash
plugin-analyzer-cli mcp [--allow-dir <資料夾>]... [--allow-any-path]
```
給 Claude 等 LLM 用戶端使用，透過 stdio 走 MCP 協定。設定方式見 [MCP.md](MCP.md)。

### `run`：執行 JSON 請求檔
```bash
plugin-analyzer-cli run request.json
cat request.json | plugin-analyzer-cli run -
```

---

## 模式

| 模式 | 測什麼 | 主要數值（metrics） |
|---|---|---|
| `linear` | 頻率響應、相位、群延遲、脈衝響應、串音 | `gain_db_at_1khz`、`response_min/max_db_20hz_20khz` |
| `thd` | 單一正弦波的諧波失真 | `thd_percent`、`thdn_percent`、`h2_dbc`…`h9_dbc`、`non_harmonic_db` |
| `imd` | SMPTE 互調失真 | `imd_percent`、`imd_db` |
| `sweep1d` | THD、增益、非諧波隨頻率的變化 | （曲線） |
| `sweep2d` | 掃頻的時間-頻率圖 | （只提供尺寸，圖像請在 App 看） |
| `thd_vs_level` | THD+N、THD、增益隨輸入音量的變化 | （曲線） |
| `oscilloscope` | 輸出波形 | （曲線） |
| `waveshaper` | 轉移曲線（輸出對輸入） | （曲線） |
| `ramp` | 動態轉移曲線 | （曲線） |
| `attack_release` | 啟動與釋放時間 | `attack_ms`、`release_ms` |
| `hammerstein` | 各階非線性濾波器 | `g1_db_at_1khz`… |
| `reverb` | 殘響長度 | `rt60_t30_s`、`rt60_t20_s`、`edt_s` |
| `performance` | CPU 用量 | `cpu_percent_block_<N>`、`cpu_silence_percent_block_<N>` |
| `specs` | 整份規格表 | `gain_db`、`impulse_peak_offset_samples`、`thd_percent_at_minus1dbfs`、`dynamic_range_db_aes17`、`true_peak_dbtp`… 共 20 多項 |

---

## 範例

**飽和器在 100 Hz 的失真：**
```bash
plugin-analyzer-cli measure thd --plugin "Saturator.vst3" --param "1.1.Drive=50" \
  --set harmonic_freq_hz=100 --set harmonic_level_db=-12 --points 0 --pretty
```

**比較兩個 EQ 有多像（差值）：**
```bash
plugin-analyzer-cli measure linear --plugin "EQ-A.vst3" --chain2 "EQ-B.vst3" --difference --pretty
```

**一次拿完整規格表，只要數字：**
```bash
plugin-analyzer-cli measure specs --plugin "Limiter.vst3" --points 0 --out limiter-specs.json
```

**JSON 請求檔（和上面的 `measure` 等效，也是之後 MCP 會用的格式）：**
```json
{
  "mode": "thd",
  "settings": { "harmonic_freq_hz": 100, "harmonic_level_db": -12 },
  "chain1": [
    { "plugin": "/Library/Audio/Plug-Ins/VST3/Saturator.vst3", "params": { "Drive": "50" } }
  ],
  "chain2": [ { "plugin": "builtin:saturator" } ],
  "difference": false,
  "max_points": 0
}
```
`chain1` / `chain2` 的每一格可以是物件，也可以直接寫插件字串，例如 `"chain1": ["builtin:compressor"]`。

---

## 輸出格式

```json
{
  "tool": { "name": "plugin-analyzer", "version": "0.1.1" },
  "mode": "thd",
  "sample_rate": 48000,
  "channels": "left/right",
  "chains": [
    { "chain": 1, "latency_samples": 0,
      "slots": [ { "slot": 1, "name": "Tanh Saturator", "manufacturer": "built-in", "format": "built-in",
                   "params": { "drive": "6.0000000 dB" }, "latency_samples": 0 } ] }
  ],
  "metrics": [
    { "name": "thd_percent", "chain": "1", "channel": "L", "value": 1.27, "unit": "%" }
  ],
  "specs":   [ { "group": "Distortion", "name": "THD @ 999.0 Hz, -1 dBFS", "chain": "1", "value": "28.073%  (-11.0 dB)" } ],
  "info":    [ "[1 L]  f0 999.0 Hz   THD 1.271% (-37.9 dB) ..." ],
  "curves":  [ { "kind": "spectrum", "chain": "1", "channel": "L", "x_unit": "Hz", "y_unit": "dBFS",
                 "y_max": -18.5, "x_at_y_max": 999.0, "points": [[20.1, -180.2], ...] } ],
  "elapsed_ms": 103
}
```

| 欄位 | 說明 |
|---|---|
| `metrics` | **最重要的部分**：有名稱、數值、單位的結果。`chain` 是 `"1"`、`"2"` 或 `"1-2"`（差值）；`channel` 是 `L`/`R`（或 `Mid`/`Side`），`all` 代表整條 chain |
| `specs` | 只有 `specs` 模式才有，是規格表的文字版 |
| `info` | 和 App 圖表上方顯示的文字相同 |
| `curves` | 曲線資料，最多 `max_points` 點。抽樣時頻譜保留峰值、波形保留最大振幅。另外附上最大值、最小值和它們的位置 |
| `heatmaps` | `sweep2d` 和 `reverb` 的時頻圖，只提供尺寸資訊 |
| `chains[].slots[].params` | 每個參數**實際設定後**的值（由插件回報） |

**錯誤：** 輸出 `{ "error": "說明" }`，exit code 為 1。錯誤訊息會列出可用的選項，例如打錯參數名稱時，會列出該插件的所有參數名稱。命令列用法錯誤時 exit code 為 2。

---

## 注意事項

- **載入插件就是執行第三方程式碼。** 只載入你信任的插件。
- 有些插件會在終端機印出自己的訊息，可能混進 JSON 輸出。要給程式解析時，建議用 `--out` 寫到檔案。
- 每次執行都是全新的插件實例，不會讀取 App 或 DAW 裡的設定。要指定插件的狀態，請用 `--param` 或 `params`。
- 已知問題：FabFilter Saturn 2 在 CLI 裡輸出全為靜音（同一台電腦上的 Pro-Q 4、Pro-C 2、Pro-L 2、Pro-MB 都正常），原因待查；App 裡是否相同尚未確認。
