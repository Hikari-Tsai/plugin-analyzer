# MCP：讓 LLM 直接量測插件

`plugin-analyzer-cli mcp` 是一個 **MCP（Model Context Protocol）server**。接上 Claude 這類 LLM 用戶端後，你可以直接用自然語言要它量測插件，例如：

> 「幫我比較 Pro-C 2 和內建壓縮器的 attack 時間」
> 「這個飽和器在 100 Hz 和 5 kHz 的失真差多少？有沒有混疊？」
> 「列出我電腦裡所有 FabFilter 的插件，然後量 Pro-L 2 的完整規格」

LLM 會自己挑選合適的量測、設定參數、讀取結果，再用白話向你解釋。

---

## 設定

先取得 `plugin-analyzer-cli`（GitHub Release 的 `CLI.zip`，或自己編譯，見 [CLI.md](CLI.md)），記下它的**完整路徑**。

### Claude Code
```bash
claude mcp add plugin-analyzer -- /完整路徑/plugin-analyzer-cli mcp
```

### Claude Desktop
在 `claude_desktop_config.json` 加上：
```json
{
  "mcpServers": {
    "plugin-analyzer": {
      "command": "/完整路徑/plugin-analyzer-cli",
      "args": ["mcp"]
    }
  }
}
```
Windows 的路徑寫成 `"C:\\Tools\\plugin-analyzer-cli.exe"`。

### 其他 MCP 用戶端
指令是 `plugin-analyzer-cli`、參數是 `mcp`，傳輸方式是 **stdio**。

---

## 提供的工具

| 工具 | 作用 |
|---|---|
| `describe` | 列出所有量測模式、設定（含預設值、單位、範圍）、內建處理器，以及**每個數值名稱的意義** |
| `list_plugins` | 找出已安裝的 VST3／AU 插件（只列檔案，不載入），可用名稱篩選 |
| `plugin_info` | 載入一個插件，回傳名稱、廠商、版本、延遲、聲道數和所有參數 |
| `measure` | 對一條或兩條 chain 執行量測，回傳有名稱的數值（例如 `thd_percent`、`attack_ms`、`rt60_t30_s`），需要時也可以附上曲線 |

### 操作你正在用的視窗（App，或 DAW 裡的插件）

| 工具 | 作用 |
|---|---|
| `list_instances` | 列出正在執行、而且允許 LLM 控制的 Plugin Analyzer（App 或插件），包括所在的 DAW 和軌道名稱 |
| `instance_state` | 你目前的設定：兩條 chain 裡的插件、bypass、延遲、你正在看的分頁和它最新的數值；插件版還有 Play 路由和回報給 DAW 的延遲 |
| `instance_parameters` | 某個 slot 裡插件的參數，可以用名稱篩選 |
| `instance_measure` | 用你目前的 chain 執行一次量測。**不會切換你畫面上的分頁**；DAW 裡的插件就算視窗關著也能量 |
| `instance_set_parameters` | 調整某個 slot 的插件參數。你畫面上會看到變化；插件版的 Play 1 / Play 2 也會跟著變 |
| `instance_load_plugin` / `instance_remove_plugin` | 在某個 slot 載入或移除插件 |
| `instance_set_bypass` | 切換某個 slot 的 bypass |
| `instance_set_play` | 插件版：切換 Off / Play 1 / Play 2 |
| `instance_snapshot` | 回傳視窗的截圖（PNG），LLM 可以直接看到你畫面上的圖表 |

例如：「看一下我 Pro Tools 裡 Vocal 軌上的 Plugin Analyzer，Chain 1 那個壓縮器的 attack 是多少？」「把 Chain 2 的 Drive 調到 50%，然後比較兩條 chain 的 THD。」

**要先開啟：** 在 Plugin Analyzer（App 或插件視窗）按 **Settings**，勾選 **Allow LLM control (MCP) of this window**。這個設定預設是關閉的，開啟後會記住，App 和插件共用這個設定。開啟時狀態列會顯示「MCP on」。

另外有一個資源（resource）`plugin-analyzer://guide`：簡短的使用說明，包括「什麼問題用哪種量測」和「數字怎麼讀」，用戶端可以把它附加到對話裡。

`measure` 的參數格式和 CLI 的 JSON 請求完全相同，詳細說明見 [CLI.md](CLI.md)；每個數值的意義見 [METRICS.md](METRICS.md)。

---

## 安全性

**載入插件就是執行插件的程式碼**，所以 server 預設只允許載入：
- 內建測試處理器（`builtin:<id>`）
- 系統標準插件資料夾裡的插件：
  - macOS：`/Library/Audio/Plug-Ins/VST3`、`/Library/Audio/Plug-Ins/Components`，以及家目錄底下對應的資料夾
  - Windows：`C:\Program Files\Common Files\VST3`、`%LOCALAPPDATA%\Programs\Common\VST3`

其他位置的插件會被拒絕，並在錯誤訊息裡說明。需要時可以放寬：

| 選項 | 作用 |
|---|---|
| `mcp --allow-dir <資料夾>` | 額外允許某個資料夾（可以重複），例如你自己開發中的插件的建置資料夾 |
| `mcp --allow-any-path` | 允許任何路徑（只在你信任這台電腦上所有插件時使用） |

**操作正在執行的視窗（`instance_*` 工具）時：**
- 預設關閉，必須在 Settings 裡主動開啟。
- 每個開著的 App 或插件只監聽 `127.0.0.1`（本機），其他電腦連不進來。
- 每次啟動都會產生一組隨機 token，寫在只有你的帳號讀得到的探索檔裡（macOS：`~/Library/Application Support/HMusic/PluginAnalyzer/instances/`，權限 600；Windows：`%APPDATA%\HMusic\PluginAnalyzer\instances\`）。沒有 token 的連線會被拒絕；token 也不會出現在 MCP 的回應裡。
- 載入插件一樣受上面資料夾限制的保護。
- 會改變你設定的工具（調參數、載入、移除、bypass、Play）都有標註，server 也會提醒 LLM 在改變之前先告訴你。

其他保護措施：
- 所有量測都是唯讀的：不會修改插件檔案，也不會修改你 DAW 或 App 裡的設定。每次量測都使用全新的插件實例。
- 有些插件會自己往 stdout 印訊息，這會弄壞 MCP 的通訊。server 啟動時會把協定輸出和一般輸出分開，插件印出的東西會被導到 stderr。
- server 只透過 stdio 和啟動它的用戶端溝通，不開任何網路連接埠。

---

## 和用戶端協定相關的細節

| 項目 | 支援 |
|---|---|
| 協定版本 | `2025-06-18`、`2025-03-26`、`2024-11-05`（依用戶端要求協商） |
| 工具結果 | 文字（JSON 字串）；`2025-06-18` 以上另外提供 `structuredContent` |
| 錯誤 | 工具執行錯誤以 `isError: true` 回傳，訊息會列出可用的選項；協定錯誤使用 JSON-RPC 錯誤碼（`-32700`、`-32601`） |
| 回應大小 | `measure` 預設只回傳數值（不含曲線），一次 THD 量測約 3 KB；要曲線時再設定 `max_points` |
| 記錄 | server 的運作記錄寫到 stderr（例如每次工具呼叫花了多久） |

## 測試

[`Tests/mcp_smoke.py`](../Tests/mcp_smoke.py) 會啟動 server，照協定完整走一遍（握手、列出工具、量測、錯誤情況、資源），CI 在 macOS 和 Windows 上都會執行。

[`Tests/live_smoke.py`](../Tests/live_smoke.py) 測試操作正在執行的視窗：先開啟一個允許 LLM 控制的 App 或插件視窗，再執行它。它只會動 Chain 2 的第 5、6 格，結束前會清掉。因為需要開著的視窗，所以只在本機執行，不在 CI 上跑。

本機也可以加上一個真正的插件一起測：
```bash
python3 Tests/mcp_smoke.py build/PluginAnalyzerCli_artefacts/Release/plugin-analyzer-cli "/Library/Audio/Plug-Ins/VST3/某個插件.vst3"
```
