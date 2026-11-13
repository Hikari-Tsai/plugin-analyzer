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

[`Tests/mcp_smoke.py`](../Tests/mcp_smoke.py) 會啟動 server，照協定完整走一遍（握手、列出工具、量測、錯誤情況、資源），CI 在 macOS 和 Windows 上都會執行。本機也可以加上一個真正的插件一起測：
```bash
python3 Tests/mcp_smoke.py build/PluginAnalyzerCli_artefacts/Release/plugin-analyzer-cli "/Library/Audio/Plug-Ins/VST3/某個插件.vst3"
```
